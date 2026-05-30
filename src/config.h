#pragma once

// =============================================================================
// WiFi
// =============================================================================
#define WIFI_SSID     "WeAreOnThePath!"
#define WIFI_PASSWORD "itfloats"

// =============================================================================
// Local NTRIP Caster  (rovers on the same network connect here)
// =============================================================================
#define NTRIP_PORT       2101
#define NTRIP_MOUNTPOINT "BASE0"
#define NTRIP_PASSWORD   ""        // leave empty to allow any client

// =============================================================================
// RTK2go  — register your mountpoint at rtk2go.com first
// =============================================================================
#define RTK2GO_HOST       "ntrip.rtk2go.com"
#define RTK2GO_PORT       2101
#define RTK2GO_MOUNTPOINT "YOUR_RTK2GO_MOUNTPOINT"
#define RTK2GO_PASSWORD   "YOUR_RTK2GO_PASSWORD"

// =============================================================================
// Onocoy  — register your mountpoint at console.onocoy.com first
// =============================================================================
#define ONOCOY_HOST       "servers.onocoy.com"
#define ONOCOY_PORT       2101
#define ONOCOY_MOUNTPOINT "YOUR_ONOCOY_MOUNTPOINT"
#define ONOCOY_PASSWORD   "YOUR_ONOCOY_PASSWORD"

// =============================================================================
// RTKdata.online  — register at rtkdata.online, uses NTRIP v1
// =============================================================================
#define RTKDATA_HOST       "rtkdata.online"
#define RTKDATA_PORT       2101
#define RTKDATA_MOUNTPOINT "YOUR_RTKDATA_MOUNTPOINT"
#define RTKDATA_PASSWORD   "YOUR_RTKDATA_PASSWORD"

// =============================================================================
// Survey-in
// =============================================================================
#define SURVEY_MIN_TIME   300    // minimum collection time in seconds (5 min)
#define SURVEY_MAX_SIGMA  0.50f  // maximum position std-dev in metres to accept

// =============================================================================
// UM980 Serial ports
//
// Two UARTs are used to keep config/response traffic separate from the
// clean RTCM binary stream:
//
//   Serial1 (CMD)  — bidirectional config channel
//     ESP32 IO18 (RX1) ← UM980 COM2 TX
//     ESP32 IO19 (TX1) → UM980 COM2 RX
//
//   Serial2 (DATA) — RTCM output only (RX only needed)
//     ESP32 IO16 (RX2) ← UM980 COM3 TX
//     ESP32 IO17 (TX2) → UM980 COM3 RX  (connected but unused after init)
//
// Note: UM980 COM1 is on the USB-C connector and is used for direct
// debugging/configuration from a PC only — not connected to the ESP32.
// =============================================================================
#define UM980_BAUD        115200

#define UM980_CMD_RX_PIN  18   // Serial1 RX  ← UM980 COM2 TX
#define UM980_CMD_TX_PIN  19   // Serial1 TX  → UM980 COM2 RX

#define UM980_DATA_RX_PIN 16   // Serial2 RX  ← UM980 COM3 TX
#define UM980_DATA_TX_PIN 17   // Serial2 TX  → UM980 COM3 RX

// =============================================================================
// RTCM message output rates on UM980 COM3 (interval in seconds)
// =============================================================================
#define RTCM_RATE_BASE_POS  5   // 1005  – base antenna position
#define RTCM_RATE_GPS       1   // 1074  – GPS MSM4
#define RTCM_RATE_GLO       1   // 1084  – GLONASS MSM4
#define RTCM_RATE_GAL       1   // 1094  – Galileo MSM4
#define RTCM_RATE_BDS       1   // 1124  – BeiDou MSM4
