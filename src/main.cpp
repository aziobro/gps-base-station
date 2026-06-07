#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "storage.h"
#include "wifi_manager.h"
#include "um980.h"
#include "survey.h"
#include "ntrip_caster.h"
#include "ntrip_client.h"
#include "web_status.h"

// ---------------------------------------------------------------------------
// Serial ports
//   Serial      (UART0) — USB debug console
//   cmdSerial   (UART1) — UM980 COM2: config commands + BESTPOS responses
//   dataSerial  (UART2) — UM980 COM3: clean RTCM binary output
// ---------------------------------------------------------------------------
HardwareSerial cmdSerial(1);
HardwareSerial dataSerial(2);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Storage       storage;
SurveyManager survey;
NtripCaster   localCaster;

NtripPushClient *rtk2go   = nullptr;
NtripPushClient *onocoy   = nullptr;
NtripPushClient *rtkdata  = nullptr;

WebStatus webStatus(survey, storage);

enum class Mode { SURVEY, BASE_TX };
Mode mode = Mode::SURVEY;

static uint32_t rtcmByteCount = 0;
static uint32_t rtcmBps       = 0;
static unsigned long bpsTimer = 0;

// Per-window stats — accumulated then snapshotted at each window boundary
struct TxStats {
    uint32_t accumMin = 0; uint32_t lastMin = 0;
    uint32_t accumHr  = 0; uint32_t lastHr  = 0;
};
static TxStats rtcmStats, r2gStats, oncStats, rtkStats;
static unsigned long minTimer = 0;
static unsigned long hrTimer  = 0;

// Loop rate — counts main-loop iterations per second (Core 1 health indicator)
static uint32_t loopCount    = 0;
static uint32_t loopRate     = 0;
static unsigned long loopTimer = 0;

static uint8_t rtcmBuf[1024];
static size_t rtcmLen = 0;
static unsigned long rtcmBatchStarted = 0;
static constexpr unsigned long RTCM_BATCH_MS = 200;

// ---------------------------------------------------------------------------
// WiFi watchdog — non-blocking, called every loop iteration
// ---------------------------------------------------------------------------
static unsigned long wifiLostAt      = 0;
static bool          wifiReconnecting = false;

void checkWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (wifiReconnecting) {
            Serial.printf("[WiFi] Reconnected. IP: %s\n",
                          WiFi.localIP().toString().c_str());
        }
        wifiLostAt       = 0;
        wifiReconnecting = false;
        return;
    }
    unsigned long now = millis();
    if (!wifiReconnecting) {
        Serial.println("[WiFi] Lost connection — reconnecting...");
        WiFi.reconnect();
        wifiLostAt       = now;
        wifiReconnecting = true;
    } else if (now - wifiLostAt > 30000) {
        Serial.println("[WiFi] Reconnect timed out — restarting.");
        ESP.restart();
    }
}

void startBaseTx(double lat, double lon, double height) {
    localCaster.suspendAndWait();
    if (rtk2go)  rtk2go->suspendAndWait();
    if (onocoy)  onocoy->suspendAndWait();
    if (rtkdata) rtkdata->suspendAndWait();

    survey.reset();
    while (dataSerial.available()) dataSerial.read();
    rtcmLen = 0;

    mode = Mode::BASE_TX;
    webStatus.setBaseTxMode(true);
    um980Init(cmdSerial, lat, lon, height);
    if (rtk2go)  rtk2go->resume();
    if (onocoy)  onocoy->resume();
    if (rtkdata) rtkdata->resume();
    localCaster.resume();
    Serial.printf("[Main] Base TX mode. ntrip://%s:%d/%s\n",
                  WiFi.localIP().toString().c_str(), NTRIP_PORT, NTRIP_MOUNTPOINT);
}

void startSurvey() {
    mode = Mode::SURVEY;
    webStatus.setBaseTxMode(false);
    storage.clearPosition();
    // Wait for every worker to close before collecting a new base position.
    localCaster.suspendAndWait();
    if (rtk2go)  rtk2go->suspendAndWait();
    if (onocoy)  onocoy->suspendAndWait();
    if (rtkdata) rtkdata->suspendAndWait();
    survey.start(cmdSerial);
    while (dataSerial.available()) dataSerial.read();
    rtcmLen = 0;
    Serial.printf("[Survey] Monitoring page: http://%s/\n",
                  WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[GPS Base Station] Booting...");

    // Large RX buffer so RTCM bursts don't overflow during network blocking ops.
    // Default is 256 bytes; at ~900 B/s a 3 s reconnect attempt would overflow it.
    dataSerial.setRxBufferSize(4096);
    cmdSerial.begin(UM980_BAUD,  SERIAL_8N1, UM980_CMD_RX_PIN,  UM980_CMD_TX_PIN);
    dataSerial.begin(UM980_BAUD, SERIAL_8N1, UM980_DATA_RX_PIN, UM980_DATA_TX_PIN);

    storage.begin();

    // Connect to WiFi — falls back to AP provisioning mode if no credentials work.
    // In AP mode this blocks forever (running the provisioning server) until the
    // user saves credentials and the device restarts.
    {
        WiFiManager wm;
        if (wm.begin(storage) == WiFiManager::State::AP_MODE) {
            Serial.println("[Main] Running in AP provisioning mode.");
            while (true) wm.update();
        }
    }

    // Load credentials — NVS takes priority over config.h defaults.
    // Pass empty strings if not configured; NtripPushClient will wait until
    // credentials are set before attempting a connection.
    ServiceCreds r2g = storage.hasCredentials("rtk2go")
                        ? storage.loadCreds("rtk2go")
                        : ServiceCreds{RTK2GO_MOUNTPOINT, RTK2GO_PASSWORD};

    ServiceCreds onc = storage.hasCredentials("onocoy")
                        ? storage.loadCreds("onocoy")
                        : ServiceCreds{ONOCOY_MOUNTPOINT, ONOCOY_PASSWORD};

    ServiceCreds rtk = storage.hasCredentials("rtkdata")
                        ? storage.loadCreds("rtkdata")
                        : ServiceCreds{RTKDATA_MOUNTPOINT, RTKDATA_PASSWORD};

    bool r2gEnabled = storage.serviceEnabled("rtk2go");
    bool oncEnabled = storage.serviceEnabled("onocoy");
    bool rtkEnabled = storage.serviceEnabled("rtkdata");
    bool r2gActive = r2gEnabled && r2g.mountpoint.length() > 0;
    bool oncActive = oncEnabled && onc.mountpoint.length() > 0;
    bool rtkActive = rtkEnabled && rtk.mountpoint.length() > 0;
    Serial.printf("[Main] RTK2go: %s  Onocoy: %s  RTKdata: %s\n",
                  r2gEnabled ? "enabled" : "disabled",
                  oncEnabled ? "enabled" : "disabled",
                  rtkEnabled ? "enabled" : "disabled");

    rtk2go  = new NtripPushClient(RTK2GO_HOST,  RTK2GO_PORT,
                                   r2gActive ? r2g.mountpoint.c_str() : "",
                                   r2gActive ? r2g.password.c_str()   : "",
                                   "RTK2go", NtripPushClient::Protocol::V1);
    onocoy  = new NtripPushClient(ONOCOY_HOST,  ONOCOY_PORT,
                                   oncActive ? onc.mountpoint.c_str() : "",
                                   oncActive ? onc.password.c_str()   : "",
                                   "Onocoy", NtripPushClient::Protocol::V2);
    rtkdata = new NtripPushClient(RTKDATA_HOST, RTKDATA_PORT,
                                   rtkActive ? rtk.mountpoint.c_str() : "",
                                   rtkActive ? rtk.password.c_str()   : "",
                                   "RTKdata", NtripPushClient::Protocol::V1);

    // Each client runs on its own FreeRTOS task on Core 0 — non-blocking from main loop
    bool r2gTaskOk = !r2gActive || rtk2go->startTask();
    bool oncTaskOk = !oncActive || onocoy->startTask();
    bool rtkTaskOk = !rtkActive || rtkdata->startTask();
    if (!r2gTaskOk || !oncTaskOk || !rtkTaskOk) {
        Serial.println("[Main] One or more NTRIP tasks failed to start.");
    }

    if (!localCaster.begin()) {
        Serial.println("[Main] Local NTRIP caster failed to start.");
    }

    webStatus.begin();
    webStatus.onSurveyRequested([]() {
        Serial.println("[Main] Survey requested via web.");
        startSurvey();
    });
    webStatus.onPositionSet([](double lat, double lon, double hgt) {
        Serial.printf("[Main] Manual position set via web: %.8f, %.8f, %.4f\n", lat, lon, hgt);
        startBaseTx(lat, lon, hgt);
    });
    webStatus.onOtaStart([]() {
        Serial.println("[Main] OTA starting - suspending all NTRIP streams.");
        localCaster.suspendAndWait();
        if (rtk2go)  rtk2go->requestSuspend();
        if (onocoy)  onocoy->requestSuspend();
        if (rtkdata) rtkdata->requestSuspend();
        delay(250);
    });
    webStatus.onOtaFinished([](bool success) {
        if (success) return;
        Serial.println("[Main] OTA failed - resuming NTRIP streams.");
        localCaster.resume();
        if (rtk2go)  rtk2go->resume();
        if (onocoy)  onocoy->resume();
        if (rtkdata) rtkdata->resume();
    });

    BasePosition stored = storage.loadPosition();
    if (stored.valid) {
        Serial.println("[Main] Stored position found — entering base TX mode.");
        startBaseTx(stored.lat, stored.lon, stored.height);
    } else {
        Serial.println("[Main] No stored position — starting survey-in.");
        startSurvey();
    }

    bpsTimer = minTimer = hrTimer = loopTimer = millis();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
    checkWiFi();

    webStatus.update();

    if (mode == Mode::SURVEY) {
        // COM3 may still contain bytes emitted before UNLOGALL was processed.
        // Discard continuously so no stale corrections survive into BASE_TX.
        while (dataSerial.available()) dataSerial.read();

        while (cmdSerial.available()) {
            survey.feed((uint8_t)cmdSerial.read());
        }

        if (survey.isDone()) {
            const SurveyResult &r = survey.result();
            storage.savePosition(r.lat, r.lon, r.height);
            startBaseTx(r.lat, r.lon, r.height);
        }

    } else {
        // Keep satellite counts updated in base TX mode via GNGSA on COM2
        while (cmdSerial.available()) {
            survey.feed((uint8_t)cmdSerial.read());
        }

        while (dataSerial.available() && rtcmLen < sizeof(rtcmBuf)) {
            if (rtcmLen == 0) rtcmBatchStarted = millis();
            rtcmBuf[rtcmLen++] = (uint8_t)dataSerial.read();
        }

        size_t flushedLen = 0;
        if (rtcmLen == sizeof(rtcmBuf) ||
            (rtcmLen > 0 && millis() - rtcmBatchStarted >= RTCM_BATCH_MS)) {
            flushedLen = rtcmLen;
            localCaster.update(rtcmBuf, rtcmLen);
            rtk2go->push(rtcmBuf, rtcmLen);
            onocoy->push(rtcmBuf, rtcmLen);
            rtkdata->push(rtcmBuf, rtcmLen);
            rtcmByteCount += rtcmLen;
            rtcmLen = 0;
        }

        // Drain per-provider byte counters into rolling accumulators
        uint32_t r2gB = rtk2go->takeBytesSent();
        uint32_t oncB = onocoy->takeBytesSent();
        uint32_t rtkB = rtkdata->takeBytesSent();
        r2gStats.accumMin += r2gB; r2gStats.accumHr += r2gB;
        oncStats.accumMin += oncB; oncStats.accumHr += oncB;
        rtkStats.accumMin += rtkB; rtkStats.accumHr += rtkB;
        rtcmStats.accumMin += flushedLen; rtcmStats.accumHr += flushedLen;

        unsigned long now = millis();

        if (now - bpsTimer >= 1000) {
            rtcmBps       = rtcmByteCount;
            rtcmByteCount = 0;
            bpsTimer      = now;
        }

        if (now - minTimer >= 60000) {
            rtcmStats.lastMin = rtcmStats.accumMin; rtcmStats.accumMin = 0;
            r2gStats.lastMin  = r2gStats.accumMin;  r2gStats.accumMin  = 0;
            oncStats.lastMin  = oncStats.accumMin;  oncStats.accumMin  = 0;
            rtkStats.lastMin  = rtkStats.accumMin;  rtkStats.accumMin  = 0;
            minTimer = now;
        }

        if (now - hrTimer >= 3600000UL) {
            rtcmStats.lastHr = rtcmStats.accumHr; rtcmStats.accumHr = 0;
            r2gStats.lastHr  = r2gStats.accumHr;  r2gStats.accumHr  = 0;
            oncStats.lastHr  = oncStats.accumHr;  oncStats.accumHr  = 0;
            rtkStats.lastHr  = rtkStats.accumHr;  rtkStats.accumHr  = 0;
            hrTimer = now;
        }

        webStatus.setNtripLocalClients(localCaster.clientCount());
        auto r2gState = rtk2go->snapshot();
        auto oncState = onocoy->snapshot();
        auto rtkState = rtkdata->snapshot();
        webStatus.setRtk2goConnected(r2gState.connected);
        webStatus.setRtk2goStatus(r2gState.status);
        webStatus.setOnocoyConnected(oncState.connected);
        webStatus.setOnocoyStatus(oncState.status);
        webStatus.setRtkdataConnected(rtkState.connected);
        webStatus.setRtkdataStatus(rtkState.status);
        webStatus.setRtcmBytesPerSec(rtcmBps);
        webStatus.setStats(rtcmStats.lastMin, rtcmStats.lastHr,
                           r2gStats.lastMin,  r2gStats.lastHr,
                           oncStats.lastMin,  oncStats.lastHr,
                           rtkStats.lastMin,  rtkStats.lastHr);
    }

    // Loop rate counter (both modes)
    loopCount++;
    unsigned long nowL = millis();
    if (nowL - loopTimer >= 1000) {
        loopRate  = loopCount;
        loopCount = 0;
        loopTimer = nowL;
        webStatus.setSysStats(
            ESP.getFreeHeap(),
            ESP.getMinFreeHeap(),
            loopRate,
            rtk2go  ? rtk2go->stackWatermark()  : 0,
            onocoy  ? onocoy->stackWatermark()   : 0,
            rtkdata ? rtkdata->stackWatermark()  : 0
        );
    }
}
