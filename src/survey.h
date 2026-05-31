#pragma once
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "storage.h"

enum class SurveyState {
    IDLE,
    COLLECTING,
    DONE,
    ERROR
};

struct SurveyResult {
    double lat    = 0;
    double lon    = 0;
    double height = 0;
    float  latSigma = 0;
    float  lonSigma = 0;
    float  hgtSigma = 0;
    uint32_t elapsedSec = 0;
};

// ---------------------------------------------------------------------------
// SurveyManager
//
// Collects BESTPOS fixes from the UM980 and computes the mean base position
// and 3-D standard deviation using Welford's online algorithm.
//
// We do NOT rely on the UM980's internal CONFIG BASE TIME sigma output because
// that field reports per-fix instantaneous accuracy (~2 m), not the mean's
// standard deviation.  By averaging N fixes ourselves:
//   σ_mean ≈ σ_single / √N
// e.g. 2.1 m / √60 ≈ 0.27 m after the 300 s minimum at 5 s sample rate.
// ---------------------------------------------------------------------------
class SurveyManager {
public:
    SurveyState state() const { return _state; }
    bool isDone()        const { return _state == SurveyState::DONE; }
    const SurveyResult& result() const { return _result; }

    void start(HardwareSerial &ser) {
        _ser     = &ser;
        _state   = SurveyState::COLLECTING;
        _startMs = millis();
        _lineBuf = "";
        _result  = SurveyResult{};

        // Welford accumulators
        _n = 0;
        _meanLat = _meanLon = _meanHgt = 0;
        _M2Lat   = _M2Lon   = _M2Hgt  = 0;

        // Constellation counts
        _svGPS = _svGLO = _svGAL = _svBDS = 0;

        // Live / history
        _live         = {};
        _historyCount = 0;
        _historyHead  = 0;

        _ser->println("UNLOGALL COM2");
        delay(200);
        _ser->println("UNLOGALL COM3");
        delay(200);

        // BESTPOS every 5 s — we average these ourselves
        _ser->println("LOG BESTPOSA ONTIME 5");
        delay(200);

        // NMEA 4.10 GNGSA every 10 s for per-constellation counts
        _ser->println("LOG GNGSA ONTIME 10");
        delay(200);

        // GSV every 10 s — per-satellite azimuth, elevation, SNR for sky plot
        _ser->println("LOG GPGSV ONTIME 10");
        delay(200);
        _ser->println("LOG GLGSV ONTIME 10");
        delay(200);
        _ser->println("LOG GAGSV ONTIME 10");
        delay(200);
        _ser->println("LOG GBGSV ONTIME 10");
        delay(200);

        Serial.printf("[Survey] Started (self-averaging). Min time: %ds, target σ: %.2fm\n",
                      SURVEY_MIN_TIME, SURVEY_MAX_SIGMA);
    }

    void feed(uint8_t byte) {
        if (byte == '\n') {
            // Always parse GNGSA (satellite counts shown on status page in all modes)
            // Only parse BESTPOS when actively collecting
            if (_lineBuf.startsWith("$GNGSA")) {
                parseGngsa(_lineBuf);
            } else if (_lineBuf.startsWith("$GPGSV")) {
                parseGsv(_lineBuf, 1);
            } else if (_lineBuf.startsWith("$GLGSV")) {
                parseGsv(_lineBuf, 2);
            } else if (_lineBuf.startsWith("$GAGSV")) {
                parseGsv(_lineBuf, 3);
            } else if (_lineBuf.startsWith("$GBGSV")) {
                parseGsv(_lineBuf, 4);
            } else if (_state == SurveyState::COLLECTING &&
                       _lineBuf.startsWith("#BESTPOSA")) {
                parseBestPos(_lineBuf);
            }
            _lineBuf = "";
        } else if (byte != '\r') {
            _lineBuf += (char)byte;
            if (_lineBuf.length() > 512) _lineBuf = "";
        }
    }

    void reset() {
        _state        = SurveyState::IDLE;
        _lineBuf      = "";
        _result       = SurveyResult{};
        _n            = 0;
        _svGPS = _svGLO = _svGAL = _svBDS = 0;
        _live         = {};
        _historyCount = 0;
        _historyHead  = 0;
        if (_ser) _ser->println("UNLOGALL THISPORT");
    }

    uint32_t elapsedSec() const {
        if (_state != SurveyState::COLLECTING) return _result.elapsedSec;
        return (millis() - _startMs) / 1000;
    }

    // -----------------------------------------------------------------------
    // Live data & history (consumed by web UI)
    // -----------------------------------------------------------------------
    // Individual satellite position (from GSV sentences)
    struct SatInfo {
        uint8_t  prn;        // satellite PRN / slot number
        uint8_t  elevation;  // degrees above horizon (0–90)
        uint16_t azimuth;    // degrees clockwise from North (0–359)
        uint8_t  snr;        // signal/noise ratio dBHz (0 = not tracking)
        uint8_t  system;     // 1=GPS 2=GLO 3=GAL 4=BDS
    };

    static constexpr int MAX_SATS = 64;

    int getSatellites(SatInfo *out, int maxLen) const {
        int n = min(_satCount, maxLen);
        memcpy(out, _satList, n * sizeof(SatInfo));
        return n;
    }

    struct LiveData {
        double   lat = 0, lon = 0, hgt = 0;   // current running mean
        float    sigma = 0;                    // our computed 3-D σ of the mean
        float    sigmaInst = 0;                // per-fix σ from BESTPOS (info only)
        int      svUsed = 0, svTracked = 0;
        int      svGPS = 0, svGLO = 0, svGAL = 0, svBDS = 0;
        uint32_t elapsed = 0;
        int      samples = 0;
        bool     valid = false;
    };

    static constexpr int HISTORY_SIZE = 120;
    struct HistorySample { uint32_t t; float sigma; };

    const LiveData& liveData() const { return _live; }

    int getHistory(HistorySample *out, int maxLen) const {
        int n = min(_historyCount, maxLen);
        int start = (_historyCount < HISTORY_SIZE) ? 0 : _historyHead;
        for (int i = 0; i < n; i++)
            out[i] = _histBuf[(start + i) % HISTORY_SIZE];
        return n;
    }

private:
    HardwareSerial *_ser   = nullptr;
    SurveyState     _state = SurveyState::IDLE;
    unsigned long   _startMs = 0;
    String          _lineBuf;
    SurveyResult    _result;

    // Welford's online mean/variance (in degrees for lat/lon, metres for hgt)
    int    _n = 0;
    double _meanLat, _meanLon, _meanHgt;
    double _M2Lat,   _M2Lon,   _M2Hgt;

    // Per-constellation counts from GNGSA
    int _svGPS = 0, _svGLO = 0, _svGAL = 0, _svBDS = 0;

    // Per-satellite positions from GSV — rebuilt per constellation each update cycle
    SatInfo _satList[MAX_SATS];
    int     _satCount = 0;

    // GSV accumulation buffer (one constellation at a time)
    SatInfo  _gsvTmp[20];
    int      _gsvTmpCnt      = 0;
    int      _gsvTotalMsgs   = 0;
    uint8_t  _gsvSystem      = 0;

    LiveData      _live;
    HistorySample _histBuf[HISTORY_SIZE];
    int           _historyCount = 0;
    int           _historyHead  = 0;

    // ------------------------------------------------------------------
    // Welford update for one variable
    static void welfordUpdate(double x, int n, double &mean, double &M2) {
        double delta  = x - mean;
        mean         += delta / n;
        double delta2 = x - mean;
        M2           += delta * delta2;
    }

    // Standard deviation of the accumulated mean (σ / √n)
    // Returns 9999 if n < 2.
    static float sigmaOfMean(double M2, int n) {
        if (n < 2) return 9999.f;
        double variance = M2 / (n - 1);          // sample variance
        double sigma    = sqrt(variance);         // per-fix std dev
        return (float)(sigma / sqrt((double)n));  // std dev of the mean
    }

    // Convert lat/lon degree std-dev → metres
    static float degToM(double sigDeg, double latDeg) {
        // 1° latitude ≈ 111,319.5 m; longitude scaled by cos(lat)
        return (float)(sigDeg * 111319.5);
    }
    static float lonDegToM(double sigDeg, double latDeg) {
        return (float)(sigDeg * 111319.5 * cos(latDeg * M_PI / 180.0));
    }

    // ------------------------------------------------------------------
    void parseBestPos(const String &line) {
        int semi = line.indexOf(';');
        if (semi < 0) return;
        String data = line.substring(semi + 1);
        int star = data.indexOf('*');
        if (star >= 0) data = data.substring(0, star);

        String tokens[20];
        int count = 0, pos = 0;
        for (int i = 0; i <= (int)data.length() && count < 20; i++) {
            if (i == (int)data.length() || data[i] == ',') {
                tokens[count++] = data.substring(pos, i);
                pos = i + 1;
            }
        }
        if (count < 9) return;

        String solStatus = tokens[0];
        if (solStatus != "SOL_COMPUTED") return;  // skip until fix

        double lat    = tokens[2].toDouble();
        double lon    = tokens[3].toDouble();
        double hgt    = tokens[4].toDouble();
        float  latSig = tokens[7].toFloat();   // per-fix sigma (info only)
        float  lonSig = tokens[8].toFloat();
        float  hgtSig = (count > 9) ? tokens[9].toFloat() : 0;
        int    svUsed = (count > 13) ? tokens[13].toInt() : 0;
        int    svTrkd = (count > 14) ? tokens[14].toInt() : 0;

        float instSig = max(latSig, max(lonSig, hgtSig));

        // Welford update
        _n++;
        welfordUpdate(lat, _n, _meanLat, _M2Lat);
        welfordUpdate(lon, _n, _meanLon, _M2Lon);
        welfordUpdate(hgt, _n, _meanHgt, _M2Hgt);

        // Compute σ of the accumulated mean in 3-D metres
        float sLat = sigmaOfMean(_M2Lat, _n);
        float sLon = sigmaOfMean(_M2Lon, _n);
        float sHgt = sigmaOfMean(_M2Hgt, _n);

        float sLatM = degToM(sLat, lat);
        float sLonM = lonDegToM(sLon, lat);
        float sigma3d = sqrt(sLatM*sLatM + sLonM*sLonM + (float)(sHgt*sHgt));

        uint32_t elapsed = (millis() - _startMs) / 1000;

        // Build constellation string
        char svBuf[48] = "";
        if (_svGPS || _svGLO || _svGAL || _svBDS)
            snprintf(svBuf, sizeof(svBuf), "  GPS:%d GLO:%d GAL:%d BDS:%d",
                     _svGPS, _svGLO, _svGAL, _svBDS);

        Serial.printf("[Survey] %us  n=%d  σ_mean=%.4fm  σ_inst=%.3fm  svs=%d/%d%s\n"
                      "         mean: lat=%.8f  lon=%.8f  hgt=%.3f\n",
                      elapsed, _n, sigma3d, instSig, svUsed, svTrkd, svBuf,
                      _meanLat, _meanLon, _meanHgt);

        // Update live data
        _live = { _meanLat, _meanLon, _meanHgt,
                  sigma3d, instSig,
                  svUsed, svTrkd,
                  _svGPS, _svGLO, _svGAL, _svBDS,
                  elapsed, _n, true };

        // Push our computed sigma to history (skip first sample — sigma is undefined)
        if (_n >= 2) {
            _histBuf[(_historyHead + _historyCount) % HISTORY_SIZE] = { elapsed, sigma3d };
            if (_historyCount < HISTORY_SIZE) {
                _historyCount++;
            } else {
                _historyHead = (_historyHead + 1) % HISTORY_SIZE;
            }
        }

        // Convergence check on our own σ
        bool timeOk  = elapsed >= (uint32_t)SURVEY_MIN_TIME;
        bool sigmaOk = _n >= 2 && sigma3d <= SURVEY_MAX_SIGMA;

        if (timeOk && sigmaOk) {
            _result.lat        = _meanLat;
            _result.lon        = _meanLon;
            _result.height     = _meanHgt;
            _result.latSigma   = sLatM;
            _result.lonSigma   = sLonM;
            _result.hgtSigma   = sHgt;
            _result.elapsedSec = elapsed;
            _state             = SurveyState::DONE;

            _ser->println("UNLOGALL THISPORT");

            Serial.printf("[Survey] DONE after %us (%d samples). "
                          "Mean: %.8f, %.8f, %.4f  σ3D=%.4fm\n",
                          elapsed, _n, _meanLat, _meanLon, _meanHgt, sigma3d);
        }
    }

    // ------------------------------------------------------------------
    // NMEA 4.10 $GNGSA,mode,fix,PRN×12,PDOP,HDOP,VDOP,sysID*cs
    void parseGngsa(const String &line) {
        String tokens[20];
        int count = 0, pos = 0;
        while (pos <= (int)line.length() && count < 20) {
            int next = line.indexOf(',', pos);
            if (next < 0) next = line.length();
            tokens[count++] = line.substring(pos, next);
            pos = next + 1;
        }
        if (count < 19) return;

        int svCount = 0;
        for (int i = 3; i <= 14 && i < count; i++) {
            String t = tokens[i];
            int s = t.indexOf('*'); if (s >= 0) t = t.substring(0, s);
            if (t.length() > 0) svCount++;
        }

        String sysField = tokens[18];
        int s = sysField.indexOf('*'); if (s >= 0) sysField = sysField.substring(0, s);
        switch (sysField.toInt()) {
            case 1: _svGPS = svCount; break;
            case 2: _svGLO = svCount; break;
            case 3: _svGAL = svCount; break;
            case 4: _svBDS = svCount; break;
        }

        // Mirror into _live immediately so status page reflects current counts
        // regardless of whether a survey is in progress.
        _live.svGPS = _svGPS;
        _live.svGLO = _svGLO;
        _live.svGAL = _svGAL;
        _live.svBDS = _svBDS;
    }

    // ------------------------------------------------------------------
    // NMEA GSV: $G?GSV,numMsgs,msgNum,totalSVs,PRN,elev,azim,snr,...*cs
    // Called with system: 1=GPS 2=GLO 3=GAL 4=BDS
    void parseGsv(const String &line, uint8_t system) {
        // Tokenise (strip checksum first)
        String s = line;
        int star = s.lastIndexOf('*');
        if (star >= 0) s = s.substring(0, star);

        String tok[20];
        int count = 0, pos = 0;
        while (pos <= (int)s.length() && count < 20) {
            int next = s.indexOf(',', pos);
            if (next < 0) next = s.length();
            tok[count++] = s.substring(pos, next);
            pos = next + 1;
        }
        if (count < 4) return;

        int numMsgs = tok[1].toInt();
        int msgNum  = tok[2].toInt();
        if (numMsgs < 1 || msgNum < 1) return;

        // First sentence of a new constellation batch — reset accumulator
        if (msgNum == 1 || system != _gsvSystem) {
            _gsvTmpCnt    = 0;
            _gsvTotalMsgs = numMsgs;
            _gsvSystem    = system;
        }

        // Extract up to 4 satellites from this sentence (fields 4,5,6,7 / 8,9,10,11 ...)
        for (int i = 4; i + 2 < count && _gsvTmpCnt < 20; i += 4) {
            int prn  = tok[i].toInt();
            int elev = tok[i+1].toInt();
            int azim = tok[i+2].toInt();
            int snr  = (i+3 < count) ? tok[i+3].toInt() : 0;
            if (prn == 0) continue;
            _gsvTmp[_gsvTmpCnt++] = { (uint8_t)prn, (uint8_t)elev,
                                       (uint16_t)azim, (uint8_t)snr, system };
        }

        // Last sentence for this constellation — commit to main satellite list
        if (msgNum == numMsgs) {
            // Remove old entries for this system, then append fresh ones
            int out = 0;
            for (int i = 0; i < _satCount; i++) {
                if (_satList[i].system != system)
                    _satList[out++] = _satList[i];
            }
            for (int i = 0; i < _gsvTmpCnt && out < MAX_SATS; i++)
                _satList[out++] = _gsvTmp[i];
            _satCount = out;
        }
    }
};
