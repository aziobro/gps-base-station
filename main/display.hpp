#pragma once
#include "esp_err.h"
#include "lvgl.h"

// Thin wrapper around the Waveshare BSP for the ESP32-P4-WIFI6-Touch-LCD-4B.
// The BSP handles MIPI DSI init, GT911 touch, LVGL port, backlight, and
// framebuffer allocation in PSRAM.
class Display {
public:
    static constexpr int kWidth  = 720;
    static constexpr int kHeight = 720;

    // Initialises the display, touch, and LVGL port.  Backlight is turned on.
    esp_err_t init();

    // LVGL display handle returned by the BSP — valid after init() succeeds.
    lv_display_t *handle() const { return disp_; }

private:
    lv_display_t *disp_ = nullptr;
};
