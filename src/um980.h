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

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1074 ONTIME %d", RTCM_RATE_GPS);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1084 ONTIME %d", RTCM_RATE_GLO);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1094 ONTIME %d", RTCM_RATE_GAL);
    um980Cmd(ser, cmd);

    snprintf(cmd, sizeof(cmd), "LOG COM3 RTCM1124 ONTIME %d", RTCM_RATE_BDS);
    um980Cmd(ser, cmd);

    // Satellite count feedback on COM2 — keeps the status page updated in base TX mode
    um980Cmd(ser, "LOG COM2 GNGSA ONTIME 10");

    um980Cmd(ser, "SAVECONFIG");

    Serial.printf("[UM980] Base TX configured at %.8f, %.8f, %.4f\n", lat, lon, height);
}
