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
    mode = Mode::BASE_TX;
    um980Init(cmdSerial, lat, lon, height);
    if (rtk2go)  rtk2go->resume();
    if (onocoy)  onocoy->resume();
    if (rtkdata) rtkdata->resume();
    Serial.printf("[Main] Base TX mode. ntrip://%s:%d/%s\n",
                  WiFi.localIP().toString().c_str(), NTRIP_PORT, NTRIP_MOUNTPOINT);
}

void startSurvey() {
    mode = Mode::SURVEY;
    storage.clearPosition();
    // Disconnect NTRIP clients — no data should be sent during survey
    if (rtk2go)  rtk2go->disconnect();
    if (onocoy)  onocoy->disconnect();
    if (rtkdata) rtkdata->disconnect();
    survey.start(cmdSerial);
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
    Serial.printf("[Main] RTK2go: %s  Onocoy: %s  RTKdata: %s\n",
                  r2gEnabled ? "enabled" : "disabled",
                  oncEnabled ? "enabled" : "disabled",
                  rtkEnabled ? "enabled" : "disabled");

    rtk2go  = new NtripPushClient(RTK2GO_HOST,  RTK2GO_PORT,
                                   r2gEnabled ? r2g.mountpoint.c_str() : "",
                                   r2gEnabled ? r2g.password.c_str()   : "",
                                   "RTK2go", NtripPushClient::Protocol::V1);
    onocoy  = new NtripPushClient(ONOCOY_HOST,  ONOCOY_PORT,
                                   oncEnabled ? onc.mountpoint.c_str() : "",
                                   oncEnabled ? onc.password.c_str()   : "",
                                   "Onocoy", NtripPushClient::Protocol::V2);
    rtkdata = new NtripPushClient(RTKDATA_HOST, RTKDATA_PORT,
                                   rtkEnabled ? rtk.mountpoint.c_str() : "",
                                   rtkEnabled ? rtk.password.c_str()   : "",
                                   "RTKdata", NtripPushClient::Protocol::V1);

    // Each client runs on its own FreeRTOS task on Core 0 — non-blocking from main loop
    rtk2go->startTask();
    onocoy->startTask();
    rtkdata->startTask();

    localCaster.begin();

    webStatus.begin();
    webStatus.onSurveyRequested([]() {
        Serial.println("[Main] Survey requested via web.");
        startSurvey();
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

        size_t len = 0;
        while (dataSerial.available() && len < sizeof(rtcmBuf)) {
            rtcmBuf[len++] = (uint8_t)dataSerial.read();
        }

        if (len > 0) {
            localCaster.update(rtcmBuf, len);
            rtk2go->push(rtcmBuf, len);
            onocoy->push(rtcmBuf, len);
            rtkdata->push(rtcmBuf, len);
            rtcmByteCount += len;
        } else {
            localCaster.update(nullptr, 0);
        }

        // Drain per-provider byte counters into rolling accumulators
        uint32_t r2gB = rtk2go->bytesSent(); rtk2go->clearBytesSent();
        uint32_t oncB = onocoy->bytesSent();  onocoy->clearBytesSent();
        uint32_t rtkB = rtkdata->bytesSent(); rtkdata->clearBytesSent();
        r2gStats.accumMin += r2gB; r2gStats.accumHr += r2gB;
        oncStats.accumMin += oncB; oncStats.accumHr += oncB;
        rtkStats.accumMin += rtkB; rtkStats.accumHr += rtkB;
        rtcmStats.accumMin += len; rtcmStats.accumHr += len;

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

        webStatus.setBaseTxMode(true);
        webStatus.setNtripLocalClients(localCaster.clientCount());
        webStatus.setRtk2goConnected(rtk2go->connected());
        webStatus.setRtk2goStatus(rtk2go->lastStatus());
        webStatus.setOnocoyConnected(onocoy->connected());
        webStatus.setOnocoyStatus(onocoy->lastStatus());
        webStatus.setRtkdataConnected(rtkdata->connected());
        webStatus.setRtkdataStatus(rtkdata->lastStatus());
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
