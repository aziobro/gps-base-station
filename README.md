# GPS RTK Base Station

An ESP32-based GNSS RTK base station using the Unicore UM980 receiver. It estimates or accepts a fixed antenna position, then streams RTCM3 correction data to multiple NTRIP casters simultaneously. Rover precision can be centimetre-level, but absolute accuracy is limited by the accuracy of the configured base position.

## Features

- **Automated survey-in** — one-minute block averaging avoids treating correlated fixes as independent
- **Multi-constellation RTCM3** — GPS (1074), GLONASS (1084), Galileo (1094), BeiDou (1124) + base position (1005)
- **Three simultaneous NTRIP push destinations** — RTK2go, Onocoy, RTKdata.online (each independently reconnecting via FreeRTOS task)
- **Real-time stream buffering** — RTCM is batched into short 200 ms chunks; stale corrections are not queued while a provider reconnects
- **Local NTRIP caster** — port 2101, up to 4 simultaneous rover clients on the local network
- **Web status page** — live satellite counts, RTCM throughput, provider state, WiFi signal strength
- **Web configuration page** — configure all NTRIP credentials with enable/disable toggles per service
- **OTA firmware updates** — upload new firmware via the web interface, no USB required
- **WiFi provisioning** — hotspot (AP) mode with network scan if no WiFi is configured
- **NVS storage** — base position and credentials survive power cycles
- **Password-protected web UI** — Basic Auth on all pages

---

## Hardware

| Component | Notes |
|-----------|-------|
| ESP32 Dev Module | Any standard 38-pin ESP32 dev board |
| Unicore UM980 | Multi-constellation GNSS receiver (GPS/GLONASS/Galileo/BeiDou) |
| USB-C cable | Powers and provides debug serial on UM980 COM1 |

---

## Wiring

The ESP32 uses two UART ports to communicate with the UM980:

- **Serial1 (CMD)** — bidirectional config channel (UM980 COM2)
- **Serial2 (DATA)** — RTCM3 binary output (UM980 COM3)

```
UM980                        ESP32
─────                        ─────
COM2 TX  ──────────────────► IO18  (Serial1 RX — config responses)
COM2 RX  ◄────────────────── IO19  (Serial1 TX — config commands)

COM3 TX  ──────────────────► IO16  (Serial2 RX — RTCM binary data)
COM3 RX  ◄────────────────── IO17  (Serial2 TX — unused after init)

COM1 TX/RX ◄──────────────── USB-C (direct PC debug only, not ESP32)
```

**Power:** Both boards are powered via their respective USB-C connectors.

**Logic levels:** Both the UM980 and ESP32 operate at 3.3 V logic — no level shifter required.

**Baud rate:** 115200 on both UART ports.

---

## Software

The migration branch is a native [ESP-IDF](https://github.com/espressif/esp-idf)
v6.0.1 project. The production Arduino firmware remains available on `main`
until hardware-in-the-loop validation is complete.

Native components use `esp_wifi`, `esp_http_server`, `esp_ota_ops`, NVS,
FreeRTOS, and lwIP sockets directly.

---

## Building & Flashing

### Prerequisites

1. Install ESP-IDF v6.0.1.
2. Clone this repository.

```bash
git clone https://github.com/aziobro/gps-base-station.git
cd gps-base-station
```

Runtime credentials are configured through the web interface and retained in
the existing `gps_base` NVS namespace.

### First Flash (USB)

```bash
source .tools/esp-idf-v6.0.1/export.sh
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

### Subsequent Updates (OTA)

Once native firmware and its bootloader have been installed over USB, navigate
to `http://<device-ip>/update` and upload `build/gps_base_station.bin`.

> Do not use application-only OTA for the first Arduino-to-ESP-IDF migration.
> OTA does not replace the Arduino bootloader, so native rollback support would
> not yet be available. Install the native bootloader, partition table, and
> application together over USB for the first migration.

---

## First Boot

### WiFi Setup

If no WiFi credentials are stored, the device starts in AP mode:

1. Connect to the `GPS-BaseStation` WiFi network (open, no password)
2. Navigate to `http://192.168.4.1`
3. Click **Scan**, select your network, enter the password, and click **Connect**
4. The device restarts and connects to your WiFi

### Web Interface

Once connected, find the device IP from your router or the serial monitor output. Navigate to:

| URL | Description |
|-----|-------------|
| `http://<ip>/` | Status page — live satellite counts, RTCM throughput, service status |
| `http://<ip>/config` | Configuration — NTRIP credentials, service toggles, manual base position |
| `http://<ip>/skyplot` | Satellite azimuth/elevation sky plot |
| `http://<ip>/update` | OTA firmware update |

The web UI is password protected. On first access you'll be prompted to set an admin password.

---

## Operation

### Survey-in Mode

On first boot (or after pressing **Start New Survey-in**), the device enters survey-in mode:

1. The UM980 streams BESTPOS fixes every 5 seconds via COM2
2. The ESP32 computes a running mean and one-minute block means using Welford's algorithm
3. Survey completes when all conditions are met:
   - Minimum 300 seconds elapsed
   - At least 5 complete one-minute blocks collected
   - 3D block-mean stability ≤ 0.50 m
4. The converged position is saved to NVS flash
5. The device automatically switches to Base TX mode

The survey panel shows block-to-block position stability, satellite counts, and a convergence chart.

> Stability is not absolute accuracy. Autonomous averaging cannot remove common
> GNSS biases and does not establish survey-grade absolute coordinates. For high absolute accuracy,
> enter a professionally surveyed or externally post-processed position on the
> configuration page.

### Base TX Mode

Once a position is stored, the device:

- Configures the UM980 as a fixed base at the surveyed position
- Streams RTCM3 messages (1005/1074/1084/1094/1124) from UM980 COM3
- Pushes data concurrently to enabled NTRIP services via FreeRTOS tasks
- Serves RTCM to local rovers via the built-in NTRIP caster on port 2101

### RTCM Output Rates

| Message | Constellation | Rate |
|---------|---------------|------|
| 1005 | Base position | Every 5 s |
| 1074 | GPS MSM4 | Every 1 s |
| 1084 | GLONASS MSM4 | Every 1 s |
| 1094 | Galileo MSM4 | Every 1 s |
| 1124 | BeiDou MSM4 | Every 1 s |

Typical throughput: **~850–950 B/s** (~7 kbps) with good satellite visibility.

---

## NTRIP Services

### Local Caster

Rovers on the same network can connect directly:

```
Host:       <device-ip>
Port:       2101
Mountpoint: BASE0
Password:   (leave empty)
```

### RTK2go

1. Register a mountpoint at [rtk2go.com](http://rtk2go.com)
2. Enter the mountpoint name and your registered email as the password in `/config`

### Onocoy

1. Register at [console.onocoy.com](https://console.onocoy.com)
2. The **base station mountpoint** (used by this firmware to push data) is different from the **rover mountpoint** (used by rover clients to pull corrections)
3. Enter the base station mountpoint and API key in `/config`
4. Protocol: NTRIP v2 (automatically selected)

### RTKdata.online

1. Register at [rtkdata.online](https://rtkdata.online)
2. Enter mountpoint and password in `/config`

---

## Architecture

```
                    ┌─────────────────────────────────────────┐
                    │              ESP32                       │
                    │                                          │
UM980 COM2 ────────►│ Serial1 (CMD)   SurveyManager           │
UM980 COM3 ────────►│ Serial2 (DATA)  ──► localCaster ──────► │──► Rovers (port 2101)
                    │                  │                       │
                    │                  ├──► RTK2go task ──────►│──► ntrip.rtk2go.com
                    │                  ├──► Onocoy task ──────►│──► servers.onocoy.com
                    │                  └──► RTKdata task ─────►│──► rtkdata.online
                    │                                          │
                    │  WebServer (port 80) ◄───────────────────│◄── Browser
                    └─────────────────────────────────────────┘
```

Each NTRIP push client runs on its own FreeRTOS task on Core 0. The native base
station task on Core 1 enqueues RTCM packets non-blocking via a 12-packet queue
per service. TCP connect, reconnect, and backoff logic runs in the background.

During OTA, active provider and local rover streams are suspended before flash
writes begin; a failed OTA automatically resumes them.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Satellite counts all zero | GNGSA not yet received | Wait up to 10 s after boot |
| Survey never converges | Poor sky view or indoors | Move outside with clear sky view |
| NTRIP service shows "TCP connect failed" | Wrong host/port, or server down | Check credentials in `/config` |
| NTRIP shows "rejected: ICY 401" | Wrong password | Re-enter password in `/config` |
| Web page not responding | Device overloaded or WiFi drop | Wait ~15 s; device auto-reconnects |
| OTA upload fails | Firmware too large | Check Flash: line in build output — must be under 100% |

---

## File Structure

```
main/
├── app_main.cpp       — native ESP-IDF startup
├── base_station.*     — SURVEY to BASE_TX state machine and RTCM fan-out
├── storage.*          — Arduino-compatible NVS persistence
├── um980.*            — UM980 command channel
├── survey.*           — block-averaged survey and satellite parsing
├── local_caster.*     — local NTRIP caster
├── ntrip_push.*       — isolated upstream NTRIP workers
├── web_server.*       — authenticated status, config, sky plot, and OTA
└── wifi_manager.*     — event-driven station/AP recovery
partitions.csv         — Arduino-compatible flash layout
sdkconfig.defaults     — native ESP-IDF configuration
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.
