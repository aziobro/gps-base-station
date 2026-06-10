#include "display.hpp"

#include "bsp/esp-bsp.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "display";
}

esp_err_t Display::init() {
    // Use field-by-field init to stay compatible with C++ strict mode —
    // ESP_LVGL_PORT_INIT_CONFIG() uses C99 designated initializers.
    bsp_display_cfg_t cfg = {};
    cfg.lvgl_port_cfg     = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.lvgl_port_cfg.task_stack = 16384;  // default 7168 is too small for complex UI refresh
    cfg.buffer_size       = BSP_LCD_DRAW_BUFF_SIZE;
    cfg.double_buffer     = BSP_LCD_DRAW_BUFF_DOUBLE;
    cfg.flags.buff_dma    = true;
    cfg.flags.buff_spiram = false;
    cfg.flags.sw_rotate   = false;

    disp_ = bsp_display_start_with_config(&cfg);
    if (!disp_) {
        ESP_LOGE(kTag, "BSP display init failed");
        return ESP_FAIL;
    }

    bsp_display_backlight_on();
    ESP_LOGI(kTag, "ST7703 720x720 display ready via Waveshare BSP");
    return ESP_OK;
}
