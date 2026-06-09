#pragma once

#include "driver/uart.h"
#include "esp_err.h"

class Um980 {
public:
    explicit Um980(uart_port_t command_uart) : command_uart_(command_uart) {}

    esp_err_t configure_base(double lat, double lon, double height);
    esp_err_t configure_survey_output();
    esp_err_t configure_raw_output();   // RANGEA at 30 s on COM3 (disables RTCM)
    esp_err_t stop_output();

private:
    uart_port_t command_uart_;

    esp_err_t command(const char *text);
    esp_err_t commandf(const char *format, ...);
    esp_err_t configure_satellite_output();
};
