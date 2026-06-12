---
name: project_display
description: "Display implementation decisions — BSP approach, PSRAM fix, working config as of ota32"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2e309479-22de-4b3e-be3b-dcb9042bd8ca
---

## Display implementation: Waveshare BSP (working as of ota32, 2026-06-10)

Use `waveshare/esp32_p4_wifi6_touch_lcd_4b` BSP component. ST7703 720×720 MIPI DSI display,
GT911 capacitive touch — both working and confirmed booting.

**Why:** Manual MIPI DSI approach caused `assert failed: esp_startup_start_app` before app_main.
BSP handles ST7703 init, GT911 touch registration with LVGL, framebuffer in PSRAM, LVGL port task.

## The PSRAM + esp_hosted memory fix (ota32)

Root cause: PSRAM driver adds ~11.5KB to BSS/IRAM, shrinking internal SRAM from 109KB to 98KB.
esp_hosted's SDIO DMA mempool (~47KB, `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`) consumed enough
internal SRAM to leave only 7.3KB free before the main task (needs 10.7KB) was created, causing
`assert failed: esp_startup_start_app app_startup.c:83 (res == pdTRUE)`.

**Fix**: `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` in `sdkconfig.defaults`.
This makes esp_hosted's SDIO DMA buffers go to PSRAM first (ESP32-P4 GDMA can reach PSRAM via
cache), preserving internal SRAM. Result: 95KB internal free before main task creation.

This fix is in `sdkconfig.defaults`. Do NOT remove it — without it the device crash-loops.

## Known workarounds still in place

- `managed_components/espressif__esp_wifi_remote/Kconfig.idf_v6.0.1.in` (1 line: `rsource "./Kconfig.idf_v6.0.in"`) — fixes esp_wifi_remote Kconfig version mismatch with IDF 6.0.1
- `components/waveshare__esp32_p4_wifi6_touch_lcd_4b/esp32_p4_wifi6_touch_lcd_4b.c` — `bsp_enable_ldo_vo4()` made idempotent/non-fatal (LDO VO4 called twice: SD card init + display init)
- `components/waveshare__esp_lcd_st7703/include/esp_lcd_st7703.h` — patched for IDF 6.0 API (`in_color_format`/`out_color_format`, removed `flags.use_dma2d`)
- `E (11811) st7703: swap_xy is not supported by this panel` — benign, rotation works without it
- `W ledc: GPIO 26 is not usable` — backlight GPIO conflict warning, display backlight still works

## sdkconfig.defaults PSRAM section (as of ota32)

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
CONFIG_SPIRAM_MEMTEST=n
CONFIG_CACHE_L2_CACHE_256KB=y
CONFIG_CACHE_L2_CACHE_LINE_128B=y
# Move SDIO DMA buffers to PSRAM — ESP32-P4 GDMA can reach PSRAM via cache.
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y
```

## Files

- `main/display.hpp` / `main/display.cpp` — thin BSP wrapper, exposes `lv_display_t *handle()`
- `main/ui.hpp` / `main/ui.cpp` — 4-tab LVGL UI (Status/NTRIP/Position/System)
- `sdkconfig.defaults` — full config including all PSRAM and display settings

## Thread safety

BSP runs its own LVGL port task. Use `bsp_display_lock(0)` / `bsp_display_unlock()` when
modifying LVGL objects from outside that task.

## OTA history

- ota24: last known-good (no display code)
- ota25–ota31: all crashed due to SRAM exhaustion or PSRAM config issues
- ota32: first working build with PSRAM + display + WiFi all functional
