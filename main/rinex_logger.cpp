#include "rinex_logger.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

#include "esp_log.h"
#include "rinex_satid.h"
#include "sd_manager.hpp"

namespace {

constexpr char   kTag[]          = "rinex";
constexpr int    kSecondsPerFile = 3600;  // 1-hour files
constexpr int    kEpochInterval  = 30;    // seconds between epochs
constexpr double kEpochHalfWidth = 0.5;  // ±0.5 s window around boundary

// WGS-84 ellipsoid parameters.
constexpr double kA  = 6378137.0;
constexpr double kE2 = 0.00669437999014;

void geodetic_to_ecef(double lat_deg, double lon_deg, double h,
                      double &x, double &y, double &z) {
    const double kPi = 3.14159265358979323846;
    const double lat = lat_deg * kPi / 180.0;
    const double lon = lon_deg * kPi / 180.0;
    const double sl  = sin(lat);
    const double N   = kA / sqrt(1.0 - kE2 * sl * sl);
    x = (N + h) * cos(lat) * cos(lon);
    y = (N + h) * cos(lat) * sin(lon);
    z = (N * (1.0 - kE2) + h) * sl;
}

// Convert GPS week + time-of-week (seconds) to calendar fields (GPS timescale,
// not UTC — matches the "GPS" TIME SYSTEM declared in the RINEX header).
void gps_to_calendar(int week, double tow,
                     int &Y, int &Mo, int &D, int &h, int &m, double &s) {
    // GPS epoch 6 Jan 1980 00:00:00 UTC = Unix 315964800
    const int64_t unix_sec = 315964800LL + (int64_t)week * 604800LL +
                             static_cast<int64_t>(tow);
    struct tm t{};
    gmtime_r((const time_t *)&unix_sec, &t);
    Y  = t.tm_year + 1900;
    Mo = t.tm_mon + 1;
    D  = t.tm_mday;
    h  = t.tm_hour;
    m  = t.tm_min;
    s  = t.tm_sec + fmod(tow, 1.0);
}

// Write one observation value (14.3f) followed by two blank flag chars.
void write_obs(FILE *f, bool valid, double value) {
    if (valid) {
        fprintf(f, "%14.3f  ", value);
    } else {
        fprintf(f, "                ");  // 16 spaces: 14 + 2 flags
    }
}

}  // namespace

// ---------------------------------------------------------------------------

void RinexLogger::start(double lat, double lon, double height) {
    if (active_) return;
    lat_ = lat; lon_ = lon; height_ = height;
    geodetic_to_ecef(lat, lon, height, ecef_x_, ecef_y_, ecef_z_);
    in_msg_  = false;
    msg_len_ = 0;
    epochs_  = 0;
    files_   = 0;
    active_  = true;
    ESP_LOGI(kTag, "RINEX collection started (%.7f, %.7f, %.3f m)", lat, lon, height);
}

void RinexLogger::stop() {
    if (!active_) return;
    active_ = false;
    close_file();
    ESP_LOGI(kTag, "RINEX collection stopped");
}

RinexLogger::Status RinexLogger::status() const {
    return {active_.load(), epochs_, files_, current_file_};
}

void RinexLogger::feed(const uint8_t *data, size_t len) {
    if (!active_) return;
    for (size_t i = 0; i < len; ++i) {
        const char c = static_cast<char>(data[i]);
        if (!in_msg_) {
            if (c == '#') {
                in_msg_  = true;
                msg_len_ = 0;
                msg_buf_[msg_len_++] = c;
            }
        } else {
            if (msg_len_ < kMsgBufSize - 1) {
                msg_buf_[msg_len_++] = c;
            } else {
                in_msg_  = false;
                msg_len_ = 0;
                continue;
            }
            // Message ends with *XXXXXXXX\r\n — detect the \n
            if (c == '\n' && msg_len_ > 12) {
                // Verify there is a '*' within the last 12 chars
                bool has_star = false;
                const int start = (int)msg_len_ - 12;
                for (int j = (int)msg_len_ - 2; j >= start && j >= 0; --j) {
                    if (msg_buf_[j] == '*') { has_star = true; break; }
                }
                if (has_star) {
                    msg_buf_[msg_len_] = '\0';
                    if (strncmp(msg_buf_, "#RANGEA,", 8) == 0) {
                        process_message();
                    }
                    in_msg_  = false;
                    msg_len_ = 0;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------

// Return true if this signal type is the preferred secondary (L2/E5a/B3I).
bool RinexLogger::prefers_sig2(uint8_t sys, uint8_t sig) {
    switch (sys) {
        case 0: return sig == 5 || sig == 9;  // GPS L2P or L2C
        case 1: return sig == 5;              // GLO L2CA
        case 3: return sig == 11;             // GAL E5aQ
        case 4: return sig == 7;              // BDS B3I
        default: return false;
    }
}

void RinexLogger::process_message() {
    // Locate the ';' that separates header fields from the observation body.
    const char *semi = strchr(msg_buf_, ';');
    if (!semi) return;

    // -----------------------------------------------------------------------
    // Parse the comma-delimited header to extract time status, GPS week, TOW.
    // #RANGEA,port,seq,idle,TIMESTATUS,week,tow,recstat,...
    char hdr[256];
    size_t hlen = std::min((size_t)(semi - msg_buf_), sizeof(hdr) - 1);
    memcpy(hdr, msg_buf_, hlen);
    hdr[hlen] = '\0';

    int    gps_week  = 0;
    double gps_tow   = 0.0;
    bool   time_ok   = false;
    {
        int field = 0;
        char *ctx;
        for (char *tok = strtok_r(hdr, ",", &ctx);
             tok;
             tok = strtok_r(nullptr, ",", &ctx), ++field) {
            if (field == 4) {
                // Time status must be FINESTEERING or SATTIME for valid GPS time.
                time_ok = (strncmp(tok, "FINE", 4) == 0 ||
                           strncmp(tok, "SAT",  3) == 0);
            } else if (field == 5) {
                gps_week = atoi(tok);
            } else if (field == 6) {
                gps_tow = atof(tok);
            }
        }
    }
    if (!time_ok || gps_week <= 0) return;

    // Only accept epochs that land on a 30-second boundary (±0.5 s).
    const double rounded = floor(gps_tow / kEpochInterval + 0.5) * kEpochInterval;
    if (fabs(gps_tow - rounded) > kEpochHalfWidth) return;

    // -----------------------------------------------------------------------
    // Tokenise the observation body (after ';').
    // Format: num_obs, prn,glo_freq,psr,psr_std,adr,adr_std,dopp,cn0,lock,tstat, ...
    char *body = const_cast<char *>(semi + 1);

    std::vector<SatObs> sats;
    sats.reserve(48);

    char *ctx;
    char *tok = strtok_r(body, ",\r\n*", &ctx);
    if (!tok) return;
    const int num_obs = atoi(tok);
    if (num_obs <= 0 || num_obs > 120) return;

    int skipped = 0;  // observation records skipped as invalid/unsupported
    for (int i = 0; i < num_obs; ++i) {
        // Collect the 10 fields for this observation.
        char *f[10];
        bool ok = true;
        for (int j = 0; j < 10; ++j) {
            f[j] = strtok_r(nullptr, ",\r\n*", &ctx);
            if (!f[j]) { ok = false; break; }
            // Trim leading whitespace.
            while (*f[j] == ' ' || *f[j] == '\r' || *f[j] == '\n') ++f[j];
        }
        if (!ok) break;

        // PRN field may carry a letter prefix (G04) or be a plain integer.
        const char *prn_str = f[0];
        uint16_t prn = 0;
        if (isalpha((unsigned char)*prn_str)) {
            prn = (uint16_t)atoi(prn_str + 1);
        } else {
            prn = (uint16_t)atoi(prn_str);
        }
        if (prn == 0) continue;

        const int8_t  glo_freq = (int8_t)atoi(f[1]);
        const double  psr      = atof(f[2]);
        const double  adr      = atof(f[4]);  // carrier phase, cycles
        const float   dopp     = (float)atof(f[6]);
        const float   cn0      = (float)atof(f[7]);
        const uint32_t tstat   = (uint32_t)strtoul(f[9], nullptr, 16);

        // Satellite system lives in bits 16-18 of the Novatel/UM980 channel
        // tracking status; signal type in bits 21-25 (5 bits).
        const uint8_t  sys      = (tstat >> 16) & 0x7;
        const uint8_t  sig      = (tstat >> 21) & 0x1F;
        const bool     is_prim  = (tstat >> 23) & 0x1;

        // Validate the constellation + PRN before storing. Anything that can't
        // map to a legal RINEX id (unsupported system, GPS>32, GLONASS>24, ...)
        // is dropped and counted so it never reaches the file as a fake G##.
        char sat_id[4];
        if (!rinex::format_sat_id(sys, prn, sat_id)) {
            ++skipped;
            ESP_LOGD(kTag, "skip sat: sys=%u prn=%u (no valid RINEX id)", sys, prn);
            continue;
        }

        // Find or create the entry for this satellite.
        SatObs *sat = nullptr;
        for (auto &s : sats) {
            if (s.prn == prn && s.system == sys) { sat = &s; break; }
        }
        if (!sat) {
            sats.push_back({prn, sys, glo_freq, {}, {}});
            sat = &sats.back();
        }
        sat->glo_freq = glo_freq;

        SigObs obs;
        obs.pseudorange   = psr;
        // Unicore/Novatel ADR has the opposite sign to the RINEX carrier-phase
        // convention; negate so downstream processors (OPUS, RTKLIB) resolve
        // ambiguities correctly. (RTKLIB's RANGE decoder negates ADR likewise.)
        obs.carrier_phase = -adr;
        obs.doppler       = dopp;
        obs.cn0           = cn0;
        obs.valid         = true;

        if (is_prim) {
            if (!sat->sig1.valid) sat->sig1 = obs;
        } else {
            if (!sat->sig2.valid || prefers_sig2(sys, sig)) {
                sat->sig2 = obs;
            }
        }
    }

    if (skipped > 0) {
        ESP_LOGW(kTag, "epoch GPS %d %.0f: skipped %d invalid/unsupported sat records",
                 gps_week, rounded, skipped);
    }
    if (sats.empty()) return;

    rotate_if_needed(gps_week, rounded);

    if (!file_) {
        // Fix the file's declared systems from the first epoch's valid sats.
        present_mask_ = 0;
        for (const auto &s : sats) {
            if (rinex::find_system(s.system)) present_mask_ |= (1u << s.system);
        }
        open_file(gps_week, rounded);
    }
    if (file_)  write_epoch(gps_week, rounded, sats);
}

// ---------------------------------------------------------------------------

void RinexLogger::rotate_if_needed(int week, double tow) {
    if (!file_) return;
    const double elapsed =
        (double)(week - gps_week_open_) * 604800.0 + (tow - tow_open_);
    if (elapsed >= kSecondsPerFile) close_file();
}

void RinexLogger::open_file(int gps_week, double tow) {
    int Y, Mo, D, h, m;
    double s;
    gps_to_calendar(gps_week, tow, Y, Mo, D, h, m, s);

    char fname[128];
    snprintf(fname, sizeof(fname),
             "%s/rawdata/BASE_%04d%02d%02d_%02d%02d%02d.rnx",
             SdManager::kMountPoint, Y, Mo, D, h, m, (int)s);

    // "w+" so close_file() can seek back and patch TIME OF LAST OBS.
    file_ = fopen(fname, "w+");
    if (!file_) {
        ESP_LOGE(kTag, "Cannot open %s", fname);
        return;
    }
    current_file_  = fname;
    gps_week_open_ = gps_week;
    tow_open_      = tow;
    epochs_        = 0;
    last_obs_pos_  = 0;
    last_obs_week_ = gps_week;
    last_obs_tow_  = tow;

    write_header(gps_week, tow);
    ESP_LOGI(kTag, "Opened RINEX file: %s", fname);
}

void RinexLogger::close_file() {
    if (!file_) return;

    // Patch the TIME OF LAST OBS placeholder with the final epoch's time. The
    // replacement line is byte-for-byte the same width as the placeholder, so
    // it overwrites in place without shifting the epoch data that follows.
    if (last_obs_pos_ > 0) {
        int Y, Mo, D, h, m;
        double s;
        gps_to_calendar(last_obs_week_, last_obs_tow_, Y, Mo, D, h, m, s);
        fflush(file_);
        if (fseek(file_, last_obs_pos_, SEEK_SET) == 0) {
            fprintf(file_,
                "%6d%6d%6d%6d%6d%13.7f     GPS         TIME OF LAST OBS\n",
                Y, Mo, D, h, m, s);
            fflush(file_);
        }
    }

    fclose(file_);
    file_ = nullptr;
    ++files_;
    ESP_LOGI(kTag, "Closed RINEX file: %s (%d epochs)", current_file_.c_str(), epochs_);
    current_file_.clear();
}

// ---------------------------------------------------------------------------

void RinexLogger::write_header(int gps_week, double tow) {
    int Y, Mo, D, h, m;
    double s;
    gps_to_calendar(gps_week, tow, Y, Mo, D, h, m, s);

    // All lines are exactly 80 chars: 60 content + 20 label.
    // Formats derived from RINEX 3.03 specification.
    fprintf(file_,
        "     3.03           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n");

    char date_col[21];
    snprintf(date_col, sizeof(date_col), "%04d%02d%02d %02d%02d%02d UTC",
             Y, Mo, D, h, m, (int)s);
    fprintf(file_, "%-20s%-20s%-20sPGM / RUN BY / DATE\n",
            "GPS_BASE", "ESP32_RTK", date_col);

    fprintf(file_, "%-60sMARKER NAME      \n", "GPS_BASE_STATION");
    fprintf(file_, "%-60sMARKER TYPE      \n", "GEODETIC");
    fprintf(file_, "%-20s%-40sOBSERVER / AGENCY\n", "UNKNOWN", "UNKNOWN");
    fprintf(file_, "%-20s%-20s%-20sREC # / TYPE / VERS\n",
            "0000", "UM980", "UNICORE");
    fprintf(file_, "%-20s%-40sANT # / TYPE     \n", "0000", "UNKNOWN");
    fprintf(file_, "%14.4f%14.4f%14.4f                  APPROX POSITION XYZ\n",
            ecef_x_, ecef_y_, ecef_z_);
    // 3×F14.4 (=42) + 18 blanks = 60 content columns; label must start at col 61.
    fprintf(file_, "%14.4f%14.4f%14.4f                  ANTENNA: DELTA H/E/N\n",
            0.0, 0.0, 0.0);

    // SYS / # / OBS TYPES — emit a line only for systems actually present in
    // this file (determined from the first epoch). 8 obs per system: C L D S
    // for two frequency bands.  ("%c    8 %s" puts the system letter in col 1,
    // the obs count in cols 4-6, and the codes from col 8 per RINEX 3.)
    int nsys = 0;
    const rinex::SystemDef *defs = rinex::systems(&nsys);
    for (int i = 0; i < nsys; ++i) {
        if (!(present_mask_ & (1u << defs[i].sys))) continue;
        char content[64];
        snprintf(content, sizeof(content), "%c    8 %s", defs[i].code, defs[i].obs);
        fprintf(file_, "%-60sSYS / # / OBS TYPES\n", content);
    }

    // Observation cadence.
    fprintf(file_, "%10.3f%50sINTERVAL\n", (double)kEpochInterval, "");

    // Format (5I6, F13.7, 5X, A3) = 51 cols, then 9 blanks = 60; label at col 61.
    fprintf(file_, "%6d%6d%6d%6d%6d%13.7f     GPS         TIME OF FIRST OBS\n",
            Y, Mo, D, h, m, s);

    // TIME OF LAST OBS: written now as a placeholder (= first-obs time, so the
    // header is valid even if never patched) and overwritten in close_file()
    // once the final epoch is known. Record its byte offset for that patch.
    last_obs_pos_ = ftell(file_);
    fprintf(file_, "%6d%6d%6d%6d%6d%13.7f     GPS         TIME OF LAST OBS\n",
            Y, Mo, D, h, m, s);

    fprintf(file_, "%-60sEND OF HEADER    \n", "");
}

// ---------------------------------------------------------------------------

void RinexLogger::write_epoch(int gps_week, double tow,
                              std::vector<SatObs> &sats) {
    int Y, Mo, D, h, m;
    double s;
    gps_to_calendar(gps_week, tow, Y, Mo, D, h, m, s);

    // Build the exact set of satellites that will be written: only systems
    // declared in this file's header, and only PRNs that map to a valid RINEX
    // id. Caching the id keeps the epoch-header count equal to the number of
    // observation rows that follow (task: count must match).
    struct EpochSat { const SatObs *sat; char id[4]; };
    std::vector<EpochSat> epoch;
    epoch.reserve(sats.size());
    for (auto &sat : sats) {
        if (!(present_mask_ & (1u << sat.system))) continue;
        char id[4];
        if (!rinex::format_sat_id(sat.system, sat.prn, id)) continue;
        epoch.push_back({&sat, {id[0], id[1], id[2], id[3]}});
    }
    if (epoch.empty()) return;

    // Epoch header: > YYYY MM DD hh mm ss.sssssss  EF  NS
    fprintf(file_, "> %4d%3d%3d%3d%3d%11.7f  0%3d\n",
            Y, Mo, D, h, m, s, (int)epoch.size());

    for (const EpochSat &es : epoch) {
        const SatObs *sat = es.sat;
        // Satellite identifier with its correct constellation prefix + PRN.
        fprintf(file_, "%s", es.id);

        // 8 observation values in declared order: C1, L1, D1, S1, C2, L2, D2, S2
        write_obs(file_, sat->sig1.valid, sat->sig1.pseudorange);
        write_obs(file_, sat->sig1.valid, sat->sig1.carrier_phase);
        write_obs(file_, sat->sig1.valid, (double)sat->sig1.doppler);
        write_obs(file_, sat->sig1.valid, (double)sat->sig1.cn0);
        write_obs(file_, sat->sig2.valid, sat->sig2.pseudorange);
        write_obs(file_, sat->sig2.valid, sat->sig2.carrier_phase);
        write_obs(file_, sat->sig2.valid, (double)sat->sig2.doppler);
        write_obs(file_, sat->sig2.valid, (double)sat->sig2.cn0);
        fprintf(file_, "\n");
    }

    // Flush stdio AND fsync so the FAT directory entry (size + cluster chain)
    // is committed to the card. Without the fsync a reboot loses the whole
    // file — the data is in the stdio/VFS buffer but the directory entry is
    // never updated, so the file reads back as 0 bytes / unrecognisable.
    fflush(file_);
    fsync(fileno(file_));
    ++epochs_;
    last_obs_week_ = gps_week;
    last_obs_tow_  = tow;
    ESP_LOGI(kTag, "Epoch GPS %d %.1f — %d satellites", gps_week, tow, (int)epoch.size());
}
