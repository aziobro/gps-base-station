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

## Migration Order

1. Boot, NVS, UART, partition table, logging
2. UM980 command channel and RTCM receive task
3. Stored position and survey engine
4. Wi-Fi station/AP state machine and captive provisioning
5. Local NTRIP caster and upstream NTRIP workers
6. HTTP status/configuration API and pages
7. OTA with rollback validation
8. HTTPS certificate provisioning and HTTP-to-HTTPS redirect
9. Hardware-in-the-loop validation before replacing the Arduino firmware

## Build

```sh
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$HOME/.platformio/packages/tool-ninja:$PATH"
source .tools/esp-idf-v6.0.1/export.sh
idf.py set-target esp32
idf.py build
```

Do not OTA this migration image until functional parity tests pass. The first
native build is intentionally limited to NVS and both UM980 UARTs.
