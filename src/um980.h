#pragma once
#include <Arduino.h>
#include "config.h"

inline void um980Cmd(HardwareSerial &ser, const char *cmd) {
    ser.println(cmd);
    delay(200);
}

// Configure the UM980 as a static RTCM3 base station at the given WGS84 position.
// ser = Serial1, connected to UM980 COM2 (config/response channel).
// RTCM output is directed to COM3, which is read by Serial2.
void um980Init(HardwareSerial &ser, double lat, double lon, double height) {
    delay(500);

    // Clear any existing scheduled output on both ports used by the ESP32
    um980Cmd(ser, "UNLOGALL COM2");
    um980Cmd(ser, "UNLOGALL COM3");

    // Fixed base position
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "CONFIG BASE GEODETIC %.8f %.8f %.4f",
             lat, lon, height);
    um980Cmd(ser, cmd);

    // Enable RTCM3 output on COM3
    // Format: LOG COM3 <message> ONTIME <rate_seconds>
    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1005 ONTIME %d", RTCM_RATE_BASE_POS);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1077 ONTIME %d", RTCM_RATE_GPS);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1087 ONTIME %d", RTCM_RATE_GLO);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1097 ONTIME %d", RTCM_RATE_GAL);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1117 ONTIME %d", RTCM_RATE_QZSS);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1127 ONTIME %d", RTCM_RATE_BDS);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1137 ONTIME %d", RTCM_RATE_NAVIC);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1230 ONTIME %d", RTCM_RATE_GLO_BIAS);
    um980Cmd(ser, cmd);

    // Satellite feedback on COM2 — constellation counts + individual azimuth/elevation/SNR
    um980Cmd(ser, "LOG COM2 GNGSA ONTIME 10");
    um980Cmd(ser, "LOG COM2 GPGSV ONTIME 10");   // GPS satellites in view
    um980Cmd(ser, "LOG COM2 GLGSV ONTIME 10");   // GLONASS
    um980Cmd(ser, "LOG COM2 GAGSV ONTIME 10");   // Galileo
    um980Cmd(ser, "LOG COM2 GBGSV ONTIME 10");   // BeiDou

    um980Cmd(ser, "SAVECONFIG");

    Serial.printf("[UM980] Base TX configured at %.8f, %.8f, %.4f\n", lat, lon, height);
}
