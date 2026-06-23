---
name: project_build_deploy
description: "How to build, flash (USB/OTA), and push the GPS base station firmware to git/GitHub"
metadata: 
  node_type: memory
  type: project
  originSessionId: e5ea740e-dcc4-4634-8fca-f230cb163b89
---

Runbook for building, deploying, and shipping firmware. See [[project_structure]] for module layout, [[project_hardware]] for the board.

## Build environment (the venv gotcha)
- **Always build via `./idf.sh` or `tools/release.sh`** — never call `idf.py` directly from a normal shell.
- `idf.sh` sources `export.sh`, which activates the IDF venv at `~/.espressif/python_env/idf6.0_py3.14_env` (Python 3.14.5). A raw `idf.py` picks up the pyenv shim instead → `No module named 'click'`.
- IDF: v6.0.1 at `.tools/esp-idf-v6.0.1/`. Toolchain: `riscv32-esp-elf esp-15.2.0_20251204` (hardcoded path in `idf.sh`). Target: `esp32p4`.
- For ad-hoc idf.py: `source ./idf.sh` first (activates env, idf.py lands on PATH), then `idf.py ...`.
- Build output: `build/gps_base_station.bin` (~2.8 MB; app slot is 6 MB).

## Build commands
```bash
./idf.sh build              # build main app
./idf.sh flash monitor      # USB flash + serial monitor
./idf.sh fullclean          # clean
PORT=/dev/tty.xyz ./idf.sh flash   # override serial port
```
Default PORT in idf.sh: `/dev/tty.usbmodem5B140747221`.

## Release (preferred — bumps version, builds, verifies on device)
`version.txt` is the single source of truth (format `YYYY.MM.DD-otaN`; trailing int auto-increments).
```bash
tools/release.sh build [VERSION]          # bump+build+verify binary, no device
tools/release.sh usb   [VERSION]          # bump+build+USB flash
ADMIN_PASSWORD=<admin-pw> tools/release.sh ota <device-ip> [VERSION]   # bump+build+OTA+poll /status
```
`tools/fw_version.py <bin>` extracts the embedded version (scans for esp_app_desc_t magic).

## Manual OTA (when not using release.sh)
```bash
curl -k -m 300 -u admin:<admin-pw> \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/gps_base_station.bin \
  https://<device-ip>/update
```
Must be raw `application/octet-stream` (NOT `curl -F` multipart → ESP_ERR_OTA_VALIDATE_FAILED). Failed OTA auto-rolls back after a 30 s health check.

## Device / credentials
- Device IP: `192.168.8.186` (DHCP — may change; `gps-base.local` mDNS also works).
- Admin user `admin`, password `<admin-pw>`. Web UI is Basic Auth on all pages.

## Recovery firmware (bootstrap)
- `bootstrap/` = minimal ~920 KB firmware: WiFi + HTTPS `/update` only. Breaks the OTA catch-22 (main app won't boot / won't fit).
- Build/flash: `source ./idf.sh && idf.py -C bootstrap build && idf.py -C bootstrap flash`. Needs certs first (`./tools/generate-https-certs.sh` — bootstrap/main/*.pem symlink into main/certs/).
- After it's running, OTA the full app onto it normally.

## Machine-specific config (config.env)
- `config.sample.env` (tracked) → copy to `config.env` (gitignored) and edit per machine.
- Sourced by `idf.sh`, `tools/release.sh`, `tools/generate-https-certs.sh`. Caller env vars override config.env; config.env overrides built-in fallbacks.
- Holds: `RISCV_TOOLCHAIN_BIN`, `PORT`, `DEVICE_HOST`, `ADMIN_USER`, `CERT_DNS`/`CERT_IP`/`CERT_AP_IP`. (Admin password kept out of it; pass `ADMIN_PASSWORD=` at runtime.)
- Built-in fallbacks still make the repo build out-of-the-box without a config.env.

## Git / GitHub
- Remote `origin` = https://github.com/aziobro/gps-base-station.git, default branch `main`.
- Commit only when the user asks. Commit message convention: `otaN: <summary>` matching the version bump.
- gitignored: `.tools/`, `build/`, `sdkconfig`, `managed_components/`, `main/certs/*.pem`, `bootstrap/dependencies.lock`, `bootstrap/main/*.pem`, `config.env`, `.claude/`.
- Tracked dep pins: root `dependencies.lock` (exact component hashes → reproducible builds). `main/idf_component.yml` ranges: esp_hosted `~2` (major pin for API stability), lvgl `>=9.3.0`, esp_wifi_remote `>=1.3.1`, idf `>=5.3.0`.

## Agent memory location (portable across machines)
- These memory files live in the **repo** at `docs/agent-memory/` (tracked in git).
- Claude's memory path `~/.claude/projects/<encoded>/memory` is a **symlink** to `docs/agent-memory/`.
- On a fresh machine after cloning, recreate the symlink once (the encoded path = the project's absolute path with `/`→`-`):
  `ln -s "$(pwd)/docs/agent-memory" ~/.claude/projects/-Users-<user>-Development-GPS/memory`

## HTTPS cert note
- `tools/generate-https-certs.sh` SAN is now parameterized via config.env (`CERT_DNS`/`CERT_IP`/`CERT_AP_IP`), default `gps-base.local` + `192.168.8.186` + `192.168.4.1`.
- Existing embedded cert still has the old `192.168.8.195`; regenerate certs (then rebuild + re-trust CA) to pick up the corrected IP. Access via `gps-base.local` is IP-independent regardless.

## Windows (PowerShell) build & deploy
On Windows the bash scripts above don't apply (`idf.sh`/`release.sh` hardcode `~/.espressif`, `/dev/tty.*`, and `source export.sh`). Use the PowerShell equivalents `idf.ps1` and `tools/release.ps1` (added June 2026 after the macOS→Windows move).
- **ESP-IDF v6.0.1** installed via the ESP-IDF Installation Manager (EIM): framework at `C:\esp\v6.0.1\esp-idf` (recorded in `%USERPROFILE%\.espressif\idf-env.json`), tools under `%USERPROFILE%\.espressif`, Python 3.12. Target `esp32p4`.
- PowerShell does **not** persist shell state between commands, so each wrapper activates IDF (dot-sources `export.ps1`) in the same process it runs `idf.py`.
- **Build / flash:**
  ```powershell
  .\idf.ps1 build                              # build
  $env:PORT='COM5'; .\idf.ps1 flash monitor    # USB flash + monitor (port via $env:PORT or config.win.ps1)
  .\idf.ps1 fullclean                          # clean
  ```
- **Release (bump → build → verify binary → push → verify on device):**
  ```powershell
  .\tools\release.ps1 build                                       # bump+build+verify binary, no device
  $env:ADMIN_PASSWORD='<admin-pw>'; .\tools\release.ps1 ota 192.168.8.186          # +OTA+poll /status
  .\tools\release.ps1 ota 192.168.8.186 2026.06.12-ota94          # explicit version (skip auto-bump)
  ```
  The device already runs `ota93`, so a real deploy must bump to **ota94 or higher** — `/status` verification can't confirm a re-flash of the same version.
- **config.win.ps1** (git-ignored; copy from `config.win.sample.ps1`): `$IdfPath`, `$SerialPort`, `$DeviceHost`, `$AdminUser`, `$AdminPassword`. A caller env var (`$env:PORT`, `$env:DEVICE_HOST`, `$env:ADMIN_PASSWORD`, …) overrides it.
- **Execution policy:** if scripts are blocked, launch with `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\release.ps1 ota 192.168.8.186`, or once per user `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`.
- **curl:** use `curl.exe` (real curl, in `System32`) for OTA / `/status` — a bare `curl` in PowerShell is an alias for `Invoke-WebRequest`.
- **Gotcha — moving/renaming the project folder:** CMake bakes absolute paths into `build/CMakeCache.txt`. After a move or rename (e.g. removing spaces: `GPS Base Station` → `GPSBaseStation`), delete the build dir before rebuilding (`Remove-Item -Recurse -Force build`), or cmake fails with "The current CMakeCache.txt directory … is different".
