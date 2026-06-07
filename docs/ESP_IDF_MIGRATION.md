# ESP-IDF Migration

## Target

- ESP-IDF v6.0.1
- ESP32 dual-core target
- Native CMake and `idf.py`
- C++ application components with ESP-IDF C APIs

The deployed Arduino firmware remains the rollback baseline on the `main`
branch. Migration work is isolated on `codex/esp-idf-migration`.

## Why Native ESP-IDF

- Native HTTPS server support through `esp_https_server`
- Direct Wi-Fi and TCP event handling without Arduino wrappers
- Explicit task priorities, stack sizes, socket timeouts, and watchdog behavior
- Native OTA rollback and image validation
- Lower ambiguity around allocation and blocking behavior
- Long-term access to current Espressif security fixes

## Component Map

| Arduino implementation | ESP-IDF replacement |
| --- | --- |
| `HardwareSerial` | `driver/uart.h` |
| `Preferences` | `nvs_flash` and `nvs` |
| `WiFi` / callbacks | `esp_wifi`, `esp_event`, `esp_netif` |
| `WiFiClient` | lwIP sockets and `esp_tls` |
| `WebServer` | `esp_http_server`, then `esp_https_server` |
| `Update` | `esp_ota_ops` |
| `DNSServer` | lwIP UDP DNS responder |
| Arduino loop | explicit FreeRTOS tasks and queues |

## Current Status

- Native boot, logging, NVS compatibility, and UART drivers are complete.
- UM980 base configuration and block-averaged survey logic are ported.
- RTCM fan-out uses bounded queues and independent FreeRTOS network workers.
- RTK2go v1, Onocoy v2, RTKdata v1, and the local caster are ported.
- Wi-Fi station recovery, AP fallback, periodic reconnect, and AP scan are ported.
- Authenticated status, configuration, sky plot, and OTA pages are ported.
- OTA streams directly to flash and tolerates temporary receive timeouts.
- Native rollback is enabled and application validation is delayed for 30 seconds.

Still required before replacing production firmware:

1. Install the native bootloader, partition table, and app over USB.
2. Validate NVS compatibility, UART traffic, survey completion, all enabled
   NTRIP connections, local caster output, AP recovery, and OTA rollback on
   bench hardware.
3. Add captive DNS if automatic portal pop-up remains desirable.
4. Port the heading/yaw TCP NMEA path; that implementation is not present in
   the current repository and therefore is not part of this native image yet.
5. Add HTTPS certificate provisioning and HTTP-to-HTTPS redirect.

## Build

```sh
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$HOME/.platformio/packages/tool-ninja:$PATH"
source .tools/esp-idf-v6.0.1/export.sh
idf.py set-target esp32
idf.py build
```

Do not OTA this migration image onto the remote production unit. The native
storage layer uses the same `gps_base` NVS namespace, key names, and value types
as Arduino Preferences. The partition table is also identical to the deployed
Arduino table because web OTA does not replace the partition table. That same
limitation means an application-only framework migration cannot install the
rollback-capable native bootloader; the first migration must be performed over
USB on bench hardware.
