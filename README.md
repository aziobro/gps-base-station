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
- **HTTPS administration** — TLS-protected status, configuration, sky plot, and OTA pages
- **OTA firmware updates** — upload new firmware via HTTPS, no USB required
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

Native components use `esp_wifi`, `esp_https_server`, `esp_ota_ops`, NVS,
FreeRTOS, and lwIP sockets directly.

---

## Building & Flashing

This is a native [ESP-IDF](https://github.com/espressif/esp-idf) v6.0.1 project
targeting the **ESP32-P4**, whose Wi-Fi is provided by an onboard ESP32-C6
coprocessor over SDIO via `esp_hosted`.

### Prerequisites

1. Install ESP-IDF v6.0.1 (the repo expects it under `.tools/esp-idf-v6.0.1`).
2. Clone this repository.

```bash
git clone https://github.com/aziobro/gps-base-station.git
cd gps-base-station
```

`idf.sh` is a thin wrapper that activates the ESP-IDF environment (so you never
have to `source export.sh` by hand) and runs any `idf.py` command against the
board:

```bash
./idf.sh build            # build
./idf.sh flash monitor    # flash over USB and open the serial monitor
./idf.sh fullclean        # clean
```

Override the serial port with `PORT=/dev/tty.yourdevice ./idf.sh flash`.

Generate a device-local certificate authority and server certificate before
the first build:

```bash
./tools/generate-https-certs.sh
```

The generated private keys in `main/certs/` are ignored by Git. Back them up
securely if future firmware must continue using the same trusted certificate.

Runtime credentials (Wi-Fi, NTRIP, admin password) are configured through the
web interface and retained in the `gps_base` NVS namespace.

### First Flash (USB)

The first install must go over USB so the bootloader and partition table land
alongside the application:

```bash
./idf.sh set-target esp32p4   # one-time, on a fresh build tree
./idf.sh flash monitor
```

## Versioning & Releases

The firmware version has a **single source of truth**: `version.txt` in the
repository root. ESP-IDF embeds it in the application descriptor at build time,
so `esp_app_get_description()->version` — the value shown on the web status page
and returned by `/status` — always matches `version.txt`. Do not hard-code a
version anywhere else.

`tools/release.sh` bumps the version, builds, pushes the update (USB or OTA),
and then **verifies the device is actually running the new version** before
declaring success:

```bash
# Build only — bump + build + confirm the binary embeds the new version.
# No device required (good for CI). Auto-increments the trailing number.
tools/release.sh build [VERSION]

# USB — bump + build + flash over USB. Set DEVICE_HOST=<ip> to also confirm
# the live /status version once the device reboots.
tools/release.sh usb [VERSION]

# OTA — bump + build + upload over HTTPS to a running device, then poll
# /status until it reports the new version.
ADMIN_PASSWORD=secret tools/release.sh ota <device-ip> [VERSION]
```

`VERSION` is optional; when omitted, the trailing integer of the current
version is incremented (e.g. `2026.06.09-ota1` → `2026.06.09-ota2`). The
on-device check reads the version straight from `/status`, so a release only
passes once the new firmware is confirmed live.

Under the hood, `tools/fw_version.py` extracts the version embedded in a built
`.bin` by locating the `esp_app_desc_t` magic word — the same struct the device
reports at runtime — which is how the build-time check guarantees the binary
carries the intended version.

### Manual OTA

You can also update from a browser: navigate to `https://<device-ip>/update`,
upload `build/gps_base_station.bin`, then confirm the version on the status page.
`tools/release.sh ota` automates exactly this, plus verification.

A failed OTA rolls back automatically — the new image must pass a 30-second
health check after reboot, otherwise the previous firmware is restored.

---

## First Boot

### WiFi Setup

If no WiFi credentials are stored, the device starts in AP mode:

1. Connect to the `GPS-BaseStation` WiFi network (open, no password)
2. Navigate to `http://192.168.4.1`
3. Click **Scan**, select your network, enter the password, and click **Connect**
4. The device restarts and connects to your WiFi

### Web Interface

Once connected, find the device IP from your router or the serial monitor
output. HTTP requests redirect to HTTPS, except while AP fallback is active.

| URL | Description |
|-----|-------------|
| `https://<ip>/` | Status page — live satellite counts, RTCM throughput, service status |
| `https://<ip>/config` | Configuration — NTRIP credentials, service toggles, manual base position |
| `https://<ip>/skyplot` | Satellite azimuth/elevation sky plot |
| `https://<ip>/update` | OTA firmware update |
| `http://<ip>/ca.crt` | Download the local CA certificate |

The web UI is password protected. On first access you'll be prompted to set an admin password.

### Trusting HTTPS

The ESP32 uses a project-local certificate authority because public
certificate services cannot validate a private LAN address.

1. Download `http://<device-ip>/ca.crt`.
2. Import it into the operating system trust store as a trusted root.
3. Open `https://<device-ip>/`.

The generated server certificate covers `gps-base.local`, `192.168.8.195`,
and the fallback AP address `192.168.4.1`. The hostname requires local DNS or
a hosts-file entry if the network does not resolve `.local` names.

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
                    │ HTTPS admin (443) ◄──────────────────────│◄── Browser
                    │ HTTP redirect/AP setup (80)              │
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
| Browser reports untrusted HTTPS | Local CA not installed | Download `/ca.crt` over HTTP and trust it |
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
partitions.csv         — OTA flash layout (ota_0 / ota_1 / otadata)
sdkconfig.defaults     — native ESP-IDF configuration
version.txt            — single source of truth for the firmware version
idf.sh                 — ESP-IDF environment + idf.py wrapper
tools/
├── release.sh         — bump → build → push (USB/OTA) → verify on device
├── fw_version.py      — read the version embedded in a built .bin
└── generate-https-certs.sh — create the device-local CA + server cert
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.
