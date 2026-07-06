---
name: project_structure
description: ESP-IDF project layout and key module responsibilities
metadata: 
  node_type: memory
  type: project
  originSessionId: 2e309479-22de-4b3e-be3b-dcb9042bd8ca
---

## Build system
- ESP-IDF v6.0.1 (at `.tools/esp-idf-v6.0.1/`)
- PlatformIO replaced by IDF; C++17, no RTTI, no exceptions
- Partitions: app0/app1 each 6MB (0x600000), OTA rollback enabled (was 1.5MB, expanded for 32MB flash)
- See [[project_build_deploy]] for build/OTA/USB/git workflow

## Key files
- `main/app_main.cpp` — startup: NVS, UART, WiFi, BaseStation, SD, Display/UI, WebServer, OTA validate task
- `main/display.hpp/.cpp` — Waveshare BSP wrapper (ST7703 + GT911 + LVGL)
- `main/ui.hpp/.cpp` — 5-tab LVGL touchscreen UI (Status/NTRIP/Position/System/Debug). WiFi modal also edits the hotspot/AP password (`on_ap_save` → storage + `apply_ap_settings()`). NTRIP tab shows reconnects/uptime/freshness. Debug tab renders `log_buffer::snapshot()` tail.
- `main/base_station.hpp/.cpp` — survey-in state machine → fixed base TX; exposes `status()`, `healthy()`
- `main/wifi_manager.hpp/.cpp` — esp_hosted + WiFi station/AP fallback; AP is WPA2 (password in NVS, default `config::kDefaultApPassword`), `apply_ap_settings()` reconfigures live
- `main/web_server.hpp/.cpp` — HTTPS admin panel (port 443): status, /config (NTRIP+WiFi+AP password), /logs console viewer, file browser, OTA
- `main/log_buffer.hpp/.cpp` — esp_log vprintf hook → 16 KB ring buffer; served at /logs AND rendered on the Debug tab (init early in app_main). **Single hook only** — do not re-add a second `esp_log_set_vprintf` (the old ui.cpp hook starved this one).
- `main/storage.hpp/.cpp` — NVS: base position, WiFi creds, AP password (`ap_pw`), antenna (`ant_model`/`ant_radome`/`ant_h`), admin pw, service creds, toggles
- `main/app_config.hpp` — compile-time constants: NTRIP hosts, AP SSID + default password, RTCM rates, survey params
- `main/sd_manager.hpp/.cpp` — SD card via SDSPI; RINEX and raw data directories
- `main/ntrip_push.hpp/.cpp` — pushes RTCM3 to RTK2go, Onocoy, RTKdata
- `main/net_health.hpp/.cpp` — periodic TCP-connect probe (8.8.8.8:53) for WAN reachability independent of WiFi association; see [[project_ntrip]]
- `main/local_caster.hpp/.cpp` — local NTRIP caster on port 2101 (up to 4 clients)
- `main/survey.hpp/.cpp` — UM980 CONFIG BASE TIME survey parser
- `main/um980.hpp/.cpp` — UM980 UART command/response driver
- `main/rinex_logger.hpp/.cpp` — RINEX 3.03 raw observation logging to SD. Classifies signals by type → band (GPS L1/L2/L5, others 2-band), configurable antenna header. See [[project_rinex]]
- `main/rinex_satid.h` — RINEX sat-id + SystemDef (obs codes, nobs/bands); shared with host unit test

## NTRIP mountpoints
- RTK2go: register at rtk2go.com
- Onocoy push: `SurelyPureBass`; rover pull: `pleasant-griffon-23912`

## OTA flow
- `validate_ota_task`: waits 30s, checks `station->healthy()` (heartbeat within 2s), marks valid or restarts
- `enable_rtk_task`: delays 20s before enabling outbound RTCM streams (allows HTTPS to come up first)
