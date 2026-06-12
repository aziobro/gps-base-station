# GPS RTK Base Station

An ESP32-P4-based GNSS RTK base station using the Unicore UM980 receiver. It estimates or accepts a fixed antenna position, then streams RTCM3 correction data to multiple NTRIP casters simultaneously. Rover precision can be centimetre-level, but absolute accuracy is limited by the accuracy of the configured base position.

## Features

- **Automated survey-in** — one-minute block averaging avoids treating correlated fixes as independent
- **Multi-constellation RTCM3** — GPS (1074), GLONASS (1084), Galileo (1094), BeiDou (1124) + base position (1005)
- **Three simultaneous NTRIP push destinations** — RTK2go, Onocoy, RTKdata.online (each independently reconnecting via FreeRTOS task)
- **Real-time stream buffering** — RTCM is batched into short 200 ms chunks; stale corrections are not queued while a provider reconnects
- **Local NTRIP caster** — port 2101, up to 8 simultaneous rover clients on the local network
- **SD card file browser** — browse, download, rename, delete, and create directories via the web UI; `logs/` and `rawdata/` directories created automatically on first mount
- **SD card storage stats** — used / free / total displayed on both the status page and file browser with a visual progress bar
- **RINEX raw data collection** — toggle from the status page to record raw GNSS observations (30-second epochs, 1-hour files) to `/sdcard/rawdata/` in RINEX 3.03 format for post-processing with OPUS or similar services; multi-file merge download (up to 15+ files) with live progress indicator built in
- **On-device touchscreen UI** — 720×720 capacitive touch LCD with a tabbed LVGL interface (Status, NTRIP, Position, System, Debug); configure WiFi and NTRIP credentials, browse the SD card, start a survey, and toggle RINEX collection without a browser
- **C6 coprocessor firmware update** — flash the bundled ESP32-C6 WiFi firmware from the System tab; the running and available versions are shown side by side
- **Web status page** — live satellite counts, RTCM throughput, provider state, WiFi signal strength, heap, SD card storage, and RINEX collection status
- **Web console log viewer** — `/logs` streams the recent ESP-IDF console output (NTRIP, WiFi, SD errors) for remote debugging without a serial cable
- **NTRIP push diagnostics** — per-service connection uptime, reconnect count, last error, and data-freshness shown on the status page
- **Web configuration page** — configure all NTRIP credentials with enable/disable toggles per service
- **HTTPS administration** — TLS-protected status, configuration, sky plot, SD card, and OTA pages
- **OTA firmware updates** — upload new firmware via HTTPS, no USB required after initial flash
- **WiFi provisioning** — WPA2-secured hotspot (AP) mode with network scan if no WiFi is configured; AP password is configurable on the `/config` page and stored in NVS
- **NVS storage** — base position and credentials survive power cycles
- **Password-protected web UI** — Basic Auth on all pages

---

## Hardware

| Component | Notes |
|-----------|-------|
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4B | ESP32-P4 host (400 MHz, 768 KB SRAM) + ESP32-C6 WiFi coprocessor over SDIO |
| 4" 720×720 MIPI-DSI LCD (ST7703) + GT911 touch | Onboard; drives the tabbed LVGL interface |
| Unicore UM980 | Multi-constellation GNSS receiver (GPS/GLONASS/Galileo/BeiDou) |
| microSD card | Optional — used for data logging and file browser |

The ESP32-C6 WiFi coprocessor communicates with the P4 host over SDIO via `esp_hosted`. Because the SDMMC peripheral is held exclusively by `esp_hosted` in ESP-IDF 6.x, the microSD card is accessed over SPI2 instead (same physical pins, independent peripheral).

The onboard ESP32-C6 WiFi firmware (`network_adapter`, the `esp_hosted` slave) is bundled in the build as `main/c6_slave_fw.bin` and can be reflashed in place from the **System** tab — see *C6 Coprocessor Firmware* below.

---

## Wiring

The ESP32-P4 uses two UART ports to communicate with the UM980 (connected to the P3 header):

```
UM980                        ESP32-P4 (P3 header)
─────                        ────────────────────
COM2 TX  ──────────────────► GPIO2   (UART1 RX — config responses)
COM2 RX  ◄────────────────── GPIO3   (UART1 TX — config commands)

COM3 TX  ──────────────────► GPIO21  (UART2 RX — RTCM binary data)
COM3 RX  ◄────────────────── GPIO22  (UART2 TX — unused after init)
```

**Do not use GPIO16/17** — reserved for the P4↔C6 WiFi coprocessor UART.  
**Do not use GPIO37/38** — wired to the onboard CH343P USB-UART chip.

### microSD card pins (SPI mode)

| SD card signal | ESP32-P4 GPIO |
|----------------|--------------|
| CLK | 43 |
| CMD / MOSI | 44 |
| D0 / MISO | 39 |
| D3 / CS | 42 |

The SD card supply (VDDPST) is powered through on-chip LDO channel 4 (`ESP_LDO_VO4`), which the firmware enables before mounting.

---

## Software

Native [ESP-IDF](https://github.com/espressif/esp-idf) v6.0.1 project. Uses `esp_wifi` (via `esp_hosted`), `esp_https_server`, `esp_ota_ops`, `esp_vfs_fat` (SDSPI), NVS, FreeRTOS, and lwIP directly. The on-device UI is built with [LVGL](https://lvgl.io/) 9 on the Waveshare BSP (MIPI-DSI + GT911 touch).

---

## Building & Flashing

### Prerequisites

1. Install ESP-IDF v6.0.1 (the repo expects it under `.tools/esp-idf-v6.0.1`).
2. Clone this repository.

```bash
git clone https://github.com/aziobro/gps-base-station.git
cd gps-base-station
```

Copy the sample config and edit it for your machine (toolchain path, serial port, device IP, certificate SAN). `config.env` is git-ignored, so machine-specific values stay out of version control:

```bash
cp config.sample.env config.env
$EDITOR config.env
```

Every value is optional — `idf.sh`, `tools/release.sh`, and `tools/generate-https-certs.sh` fall back to built-in defaults, and a real environment variable (e.g. `PORT=/dev/ttyX ./idf.sh flash`) always overrides `config.env`. See [config.sample.env](config.sample.env) for the documented list.

`idf.sh` is a thin wrapper that activates the ESP-IDF environment and runs `idf.py`:

```bash
./idf.sh build            # build
./idf.sh flash monitor    # flash over USB and open the serial monitor
./idf.sh fullclean        # clean
```

Override the serial port with `PORT=/dev/tty.yourdevice ./idf.sh flash`.

> **Always build through `./idf.sh`** (or `tools/release.sh`, which calls it). It sources `export.sh` so `idf.py` runs inside the IDF Python virtualenv (`~/.espressif/python_env/idf6.0_py3.14_env`). Invoking `idf.py` directly from a normal shell picks up the system/pyenv Python instead and fails with `No module named 'click'`.

To run ad-hoc `idf.py` commands (e.g. against a subproject), activate the environment once in your current shell and then call `idf.py` directly:

```bash
source ./idf.sh                 # activate the IDF env in this shell
idf.py -C bootstrap build       # build the recovery firmware (see below)
```

Generate a device-local certificate authority and server certificate before the first build:

```bash
./tools/generate-https-certs.sh
```

The generated private keys in `main/certs/` are ignored by Git. Back them up securely if future firmware must continue using the same trusted certificate.

Runtime credentials (Wi-Fi, NTRIP, admin password) are configured through the web interface and retained in the `gps_base` NVS namespace.

### First Flash (USB)

The first install must go over USB so the bootloader and partition table land alongside the application:

```bash
./idf.sh set-target esp32p4   # one-time, on a fresh build tree
./idf.sh flash monitor
```

### Recovery Firmware (Bootstrap)

`bootstrap/` is a minimal (~920 KB) standalone firmware whose only job is to bring up WiFi and serve the HTTPS `/update` page. It exists to break the **OTA catch-22**: if the main application won't boot, or a build won't fit the OTA slot, you can recover over the air instead of needing USB access to the device.

It shares the production partition table (two 6 MB app slots) and reuses the parent project's `storage` and `wifi_manager` sources plus its already-downloaded `managed_components/`, so no extra dependency fetch is needed.

```bash
./tools/generate-https-certs.sh   # once — bootstrap/main/*.pem symlink into main/certs/
source ./idf.sh                   # activate the IDF env
idf.py -C bootstrap build         # → bootstrap/build/gps_bootstrap.bin
idf.py -C bootstrap flash         # flash the recovery image over USB
```

Once the bootstrap firmware is running, push the full application to it over the air exactly like a normal release:

```bash
ADMIN_PASSWORD=secret tools/release.sh ota <device-ip>
```

---

## Versioning & Releases

The firmware version has a **single source of truth**: `version.txt` in the repository root. ESP-IDF embeds it in the application descriptor at build time, so `esp_app_get_description()->version` — the value shown on the web status page and returned by `/status` — always matches `version.txt`.

`tools/release.sh` bumps the version, builds, pushes the update (USB or OTA), and then **verifies the device is actually running the new version** before declaring success:

```bash
# Build only — bump + build + confirm the binary embeds the new version.
tools/release.sh build [VERSION]

# USB — bump + build + flash over USB.
tools/release.sh usb [VERSION]

# OTA — bump + build + upload over HTTPS, then poll /status until confirmed.
ADMIN_PASSWORD=secret tools/release.sh ota <device-ip> [VERSION]
```

`VERSION` is optional; when omitted, the trailing integer of the current version is incremented (e.g. `2026.06.09-ota1` → `2026.06.09-ota2`).

A failed OTA rolls back automatically — the new image must pass a 30-second health check after reboot, otherwise the previous firmware is restored.

---

## First Boot

### WiFi Setup

If no WiFi credentials are stored, the device starts in AP mode:

1. Connect to the `GPS-BaseStation` WiFi network. It is **WPA2-secured**; the default password is `gpsbase-rtk` (change it later on the `/config` page — it is stored in NVS).
2. Navigate to `http://192.168.4.1`
3. Click **Scan**, select your network, enter the password, and click **Connect**
4. The device restarts and connects to your WiFi

### Web Interface

Once connected, find the device IP from your router or the serial monitor output. HTTP requests redirect to HTTPS, except while AP fallback is active.

| URL | Description |
|-----|-------------|
| `https://<ip>/` | Status page — satellite counts, RTCM throughput, service status, SD card stats |
| `https://<ip>/config` | Configuration — NTRIP credentials, service toggles, manual base position, WiFi, hotspot (AP) password |
| `https://<ip>/skyplot` | Satellite azimuth/elevation sky plot |
| `https://<ip>/files` | SD card file browser — browse, download, create folders, rename, delete; select multiple `.rnx` files to merge & download |
| `https://<ip>/logs` | Console log viewer — recent ESP-IDF output for remote debugging |
| `https://<ip>/update` | OTA firmware update |
| `http://<ip>/ca.crt` | Download the local CA certificate |

The web UI is password protected. On first access you'll be prompted to set an admin password.

### Trusting HTTPS

The ESP32-P4 uses a project-local certificate authority because public certificate services cannot validate a private LAN address.

1. Download `http://<device-ip>/ca.crt`.
2. Import it into the operating system trust store as a trusted root.
3. Open `https://<device-ip>/`.

---

## Touchscreen Interface

The onboard 720×720 touch LCD mirrors most of the web UI through a tabbed LVGL interface, so the base station can be set up and monitored without a browser:

| Tab | Contents |
|-----|----------|
| **Status** | Mode (SURVEY / BASE TX), RTCM throughput, satellite counts, fixed position, NTRIP caster status, and a **Start / Restart Survey** button |
| **NTRIP** | Per-service status (with connection uptime), bytes sent + data freshness, dropped batches, and **reconnect count**; a global enable/disable switch; per-service **Config** buttons (mountpoint + password) |
| **Position** | Fixed base position, survey quality (stability, sigma, blocks), and per-constellation satellite SNR detail |
| **System** | WiFi state + **Configure WiFi** (scan, select, station password, **and the hotspot/AP password**), SD card stats + **Browse SD Card**, **RINEX collection** toggle, firmware versions, and **Update C6 Firmware** |
| **Debug** | Scrolling on-device console log (the same buffer served at `/logs`) |

On-screen keyboards appear for all text entry (WiFi and NTRIP credentials). Changes made on the touchscreen and in the web UI share the same NVS storage.

### C6 Coprocessor Firmware

The **System** tab shows two firmware versions:

- **C6 running** — queried live from the ESP32-C6 over the `esp_hosted` link
- **C6 available** — parsed from the firmware bundled in this build (`main/c6_slave_fw.bin`), shown with its build date

Tapping **Update C6 Firmware** streams the bundled image to the coprocessor over SDIO, then reboots the host to resync. The device stays usable during the transfer (progress is shown on the System tab); WiFi drops only for the moment the C6 restarts.

> The running-version number reflects the C6's compiled `esp_hosted` protocol version, not the bundled binary's build metadata — so a custom rebuild that keeps the same protocol version will report the same number. Use the **C6 available** build date to confirm which image is bundled.

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

> Stability is not absolute accuracy. Autonomous averaging cannot remove common GNSS biases. For high absolute accuracy, enter a professionally surveyed or post-processed position on the configuration page.

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

## RINEX Raw Data Collection

The base station can record raw GNSS observations in **RINEX 3.03** format for post-processing with services such as [OPUS](https://geodesy.noaa.gov/OPUS/) (NOAA Online Positioning User Service) to obtain a more accurate absolute base position than autonomous survey-in can provide.

### How it works

1. Click **Start** next to the *RINEX collection* row on the status page
2. COM3 switches from RTCM binary output to ASCII `RANGEA` (raw observations) at 30-second intervals — RTCM push to RTK2go, Onocoy, and RTKdata is automatically suspended while collecting
3. One-hour files are written to `/sdcard/rawdata/` named `BASE_YYYYMMDD_HHMMSS.rnx`
4. Click **Stop** to close the current file and restore full RTCM output
5. Navigate to **SD card files → rawdata**, select two or more `.rnx` files (up to 15+ supported), and click **Merge & Download** — a progress indicator shows elapsed time and bytes received; do not navigate away until the download completes

### Uploading to OPUS

OPUS accepts RINEX 2.x and 3.x observation files. For best results:

- Collect at least **4 hours** of data (2 hours minimum for OPUS Static)
- The GPS L1 + L2 dual-frequency observations written by this firmware are compatible with both OPUS Static and OPUS Rapid Static
- The APPROX POSITION XYZ in the file header is set from the current survey-in position; OPUS computes the precise position independently

Once OPUS returns a result, enter the precise latitude, longitude, and ellipsoidal height on the **/config** page under **Manual Position** and click **Save Position** — the base will immediately reconfigure to broadcast corrections from that surveyed location.

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

### RTKdata.online

1. Register at [rtkdata.online](https://rtkdata.online)
2. Enter mountpoint and password in `/config`

---

## Architecture

```
                    ┌─────────────────────────────────────────┐
                    │           ESP32-P4                       │
                    │                                          │
UM980 COM2 ────────►│ UART1 (CMD)     SurveyManager           │
UM980 COM3 ────────►│ UART2 (DATA) ──► LocalCaster ─────────► │──► Rovers (port 2101)
                    │               │                          │
                    │               ├──► RTK2go task ─────────►│──► ntrip.rtk2go.com
                    │               ├──► Onocoy task ─────────►│──► servers.onocoy.com
                    │               └──► RTKdata task ─────────►│──► rtkdata.online
                    │                                          │
microSD ───────────►│ SPI2 (SDSPI)    VFS FAT                 │
                    │                                          │
                    │ HTTPS admin (443) ◄──────────────────────│◄── Browser
                    │ HTTP redirect/AP setup (80)              │
                    │                                          │
ESP32-C6 ──────────►│ SDIO (esp_hosted) WiFi                  │
                    └─────────────────────────────────────────┘
```

Each NTRIP push client runs on its own FreeRTOS task. The base station task enqueues RTCM packets non-blocking via a 12-packet queue per service. TCP connect, reconnect, and backoff logic runs in the background.

During OTA, active provider and local rover streams are suspended before flash writes begin; a failed OTA automatically resumes them.

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
| OTA upload fails | Firmware too large | Check Flash line in build output — must be under 100% |
| SD card shows "not mounted" | LDO channel 4 not enabled | Verify firmware version ≥ ota18 |
| SD card shows "calculating…" | `statvfs` not supported on FATFS | Verify firmware version ≥ ota22 (uses `esp_vfs_fat_info`) |
| RTK2go bans the device IP | Connected before UM980 was tracking | Verify firmware version ≥ ota23 — NTRIP clients now wait for first RTCM batch |
| RINEX collection won't start | Device not in Base TX mode | Complete survey-in or set a manual position first |
| RINEX file has no observations | UM980 not tracking satellites | Wait for satellite lock before starting collection |
| RINEX merge download stalls or progress stops | Large multi-file merges take time | The merge streams at ~22 KB/s; 15 one-hour files (~5 MB) take about 4 minutes — keep the tab open and wait |

---

## File Structure

```
main/
├── app_main.cpp       — ESP-IDF startup, UART init, SD mount, web server start
├── app_config.hpp     — compile-time constants (NTRIP hosts, AP SSID + default password, rates)
├── base_station.*     — SURVEY to BASE_TX state machine and RTCM fan-out
├── storage.*          — NVS persistence (position, WiFi, AP password, NTRIP credentials)
├── um980.*            — UM980 command channel
├── survey.*           — block-averaged survey and satellite parsing
├── local_caster.*     — local NTRIP caster (port 2101, up to 8 clients)
├── ntrip_push.*       — isolated upstream NTRIP workers (RTK2go, Onocoy, RTKdata) + diagnostics
├── sd_manager.*       — microSD via SDSPI (LDO enable, mount, browse, disk stats)
├── rinex_logger.*     — RINEX 3.03 observation file writer (RANGEA parser, 30 s epochs, hourly rotation)
├── log_buffer.*       — esp_log ring-buffer capture served at /logs
├── web_server.*       — authenticated status, config, logs, sky plot, SD browser, RINEX toggle, OTA
├── display.*          — Waveshare BSP bring-up (MIPI-DSI + GT911 touch)
├── ui.*               — LVGL tabbed touchscreen UI and C6 firmware update
├── lv_mem_custom.c    — routes LVGL allocations to PSRAM
├── c6_slave_fw.bin    — bundled ESP32-C6 esp_hosted firmware (embedded for on-device update)
└── wifi_manager.*     — event-driven station/AP recovery; WPA2 SoftAP
partitions.csv         — OTA flash layout (ota_0 / ota_1 / otadata)
sdkconfig.defaults     — ESP-IDF kconfig overrides (socket pool, lwIP tuning)
version.txt            — single source of truth for the firmware version
config.sample.env      — sample machine-specific build/deploy config (copy to config.env)
idf.sh                 — ESP-IDF environment + idf.py wrapper (reads config.env)
tools/
├── release.sh         — bump → build → push (USB/OTA) → verify on device
├── fw_version.py      — read the version embedded in a built .bin
└── generate-https-certs.sh — create the device-local CA + server cert
bootstrap/             — minimal recovery firmware (WiFi + HTTPS /update only)
├── main/              — bootstrap app + web server; symlinks parent storage/wifi/certs
├── partitions.csv     — same 6 MB OTA layout as production
└── CMakeLists.txt     — reuses parent managed_components via EXTRA_COMPONENT_DIRS
docs/agent-memory/     — version-controlled project knowledge notes (see below)
```

### Project knowledge notes

`docs/agent-memory/` holds version-controlled notes about the project's
architecture and hard-won fixes (build/deploy runbook, NTRIP root-cause, etc.)
so the knowledge survives a machine switch. These are also used as the AI
assistant's persistent memory: the local `~/.claude/.../memory` directory is a
symlink into this folder. On a fresh clone, recreate the symlink once if you use
that workflow:

```bash
ln -s "$(pwd)/docs/agent-memory" \
  ~/.claude/projects/-Users-<you>-Development-GPS/memory
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.
