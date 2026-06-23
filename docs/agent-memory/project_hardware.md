---
name: project_hardware
description: "Hardware platform, pin assignments, and device identifiers for the GPS RTK base station"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2e309479-22de-4b3e-be3b-dcb9042bd8ca
---

## Board
ESP32-P4-WIFI6-Touch-LCD-4B (Waveshare)
- ESP32-P4 main SoC + ESP32-C6 WiFi6 coprocessor (via esp_hosted SDIO)
- ST7703 MIPI DSI 2-lane 720×720 IPS touch display (GT911 capacitive touch)
- 32MB PSRAM, 32MB flash

## UM980 GNSS Wiring
- COM2 (command): TX2→GPIO2 (ESP RX), RX2→GPIO3 (ESP TX) — UART1
- COM3 (RTCM data): TX3→GPIO21 (ESP RX), RX3→GPIO22 (ESP TX) — UART2

## USB Serial Ports (macOS)
- ESP32-P4 USB: /dev/tty.usbmodem* (on-board CH343P at GPIO37/38)
- UM980 serial: /dev/tty.usbserial-*

## WiFi Coprocessor
- C6 RST: GPIO54, SDIO: CLK=18, CMD=19, D0-D3=14-17
- GPIO16/17 reserved (P4↔C6), GPIO37/38 reserved (USB-UART)

## Display GPIO
- Backlight: GPIO26 (active LOW — set to 0 to turn on)
- LCD RST: GPIO27
- Touch SDA: GPIO7, SCL: GPIO8, RST: GPIO23
- MIPI LDO: channel 3 @ 2500mV

## OTA / Flashing
- Device IP (when on network): 192.168.8.186
- Admin password: <admin-pw>
- Flash via USB: idf.py -p /dev/tty.usbmodem* flash monitor
- OTA command: ADMIN_PASSWORD=<admin-pw> bash tools/release.sh ota 192.168.8.186
