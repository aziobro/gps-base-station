#pragma once

#include <string>

#include "driver/uart.h"
#include "esp_err.h"

class Um980 {
public:
    explicit Um980(uart_port_t command_uart) : command_uart_(command_uart) {}

    // antenna_model feeds CONFIG BASEANTENNAMODEL (embedded in RTCM
    // 1005/1006/1033's antenna descriptor field); pass an empty string to
    // leave the receiver's default (ADVNULLANTENNA) untouched.
    esp_err_t configure_base(
        double lat, double lon, double height, const std::string &antenna_model);
    esp_err_t configure_survey_output();
    esp_err_t configure_raw_output();   // RANGEA at 30 s on COM3 (disables RTCM)
    esp_err_t stop_output();
    // Switches the receiver's tracked-signal set from the factory default
    // (SIGNALGROUP 1: no NavIC, narrower GLONASS/Galileo band coverage) to
    // SIGNALGROUP 2 (adds GLONASS G3, Galileo E6, NavIC L5). The receiver
    // resets automatically when this value changes, so this is a separate,
    // explicit step (not part of configure_base()) -- call it, then call
    // configure_base() again afterward to reapply RTCM output.
    esp_err_t configure_signal_group();

private:
    uart_port_t command_uart_;

    esp_err_t command(const char *text);
    esp_err_t commandf(const char *format, ...);
    esp_err_t configure_satellite_output();
};
