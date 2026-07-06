---
name: project_deploy_test
description: "How to deploy + verify firmware: OTA via web (normal) and USB recovery flash from the monitoring Pi (when crash-looping / web unreachable)"
metadata:
  node_type: memory
  type: project
---

Two ways to get firmware onto the device and confirm it actually landed. See also [[project_build_deploy]] (build env) and [[project_hardware]] (pins, IPs, admin password).

## A. OTA via web (normal path)
Requires the device's HTTPS stack to be up and reachable.
```powershell
$env:ADMIN_PASSWORD='$ADMIN_PW'; .\tools\release.ps1 ota 192.168.8.186
```
`release.ps1` bumps `version.txt`, rebuilds (version baked into the image), uploads to `POST /update`, then polls `/status` until the device reports the new version string. It verifies by **version string**, so the bump is mandatory (re-flashing the same version can't be confirmed). The device writes the image to the *other* OTA slot, reboots into it, and `validate_ota_task` marks it valid after a ~30 s health check (else anti-rollback reverts).

## B. USB recovery flash from the Pi (when OTA can't be used)
Use when the device is crash-looping or otherwise not serving the web UI (OTA needs the web stack up). The monitoring Pi has the device on USB.

- **Pi:** `192.168.8.100`, user `aziobro` (SSH password is the user's — NOT stored in the repo). Device serial = `/dev/ttyACM0` (ESP32-P4 **native USB-Serial/JTAG** — it RE-ENUMERATES on every reset, which kills any plain `cat` reading it).
- **Tools on Windows:** PuTTY `plink`/`pscp` in `C:\Program Files\PuTTY`. PowerShell 5.1 mangles embedded quotes when calling native exes, so run remote scripts with **`plink -m <localscriptfile>`**, not inline quoted commands.
- **esptool on the Pi:** v5.x in `~/esptool-venv` (`python3 -m venv ~/esptool-venv && ~/esptool-venv/bin/pip install esptool`). Debian's packaged esptool is too old for esp32p4.
- **Serial capture is systemd-managed** (`esp32-log.service`, added 2026-07-05): auto-starts on Pi boot, `Restart=always` if the capture process dies. This means the old `pkill -f 'cat /dev/ttyACM0'` trick to free the port **no longer works** — systemd sees the process die and respawns it within ~2s, racing esptool for the port. Use `systemctl stop`/`start` instead (an explicit stop is honored, not fought by `Restart=always`).

Steps:
1. Build locally (`.\idf.ps1 build`). Copy images to the Pi:
   `pscp -scp -pw <pw> build\bootloader\bootloader.bin build\partition_table\partition-table.bin build\ota_data_initial.bin build\gps_base_station.bin aziobro@192.168.8.100:/home/aziobro/fw/`
2. Stop the serial capture to free the port: `sudo systemctl stop esp32-log.service` (confirm `sudo fuser /dev/ttyACM0` is blank).
3. Flash app0 + reset otadata so the bootloader picks app0 (minimal, low-risk; leaves bootloader/partition table untouched):
   `~/esptool-venv/bin/esptool --chip esp32p4 --port /dev/ttyACM0 -b 460800 --before default-reset --after hard-reset write-flash 0xe000 ~/fw/ota_data_initial.bin 0x10000 ~/fw/gps_base_station.bin`
   (Partition map: bootloader 0x2000, partition-table 0x8000, otadata 0xe000, app0 0x10000, app1 0x610000, coredump 0xc10000.)
4. Restart the serial capture: `sudo systemctl start esp32-log.service` (new timestamped log file each start; `~/logs/esp32p4-current.log` symlink always points at the live one).

Or use `~/flash-esp32.sh ~/fw/ota_data_initial.bin ~/fw/gps_base_station.bin` on the Pi, which wraps steps 2-4 (stop → flash → restart, restarts the logger even if the flash fails).

## C. Verify a deploy landed + is stable
- **Authoritative:** `curl -sk -u admin:$ADMIN_PW https://192.168.8.186/status` → check `"version"`, `"healthy":true`, and that `"uptime_sec"` climbs across calls (a reset = reboot/crash).
- **Serial (Pi):** `~/logs/esp32p4-*.log`. Boot banner shows `App version`, `Compile time`, and `Loaded app from partition at offset 0x10000` (app0) vs `0x610000` (app1) — the only reliable way to tell two same-version builds apart. **Crash signature to grep:** `abort() was called` / `Failed to allocate` / `esp-aes: Failed to allocate memory` / mbedtls `-0x0084` → internal-SRAM exhaustion (see [[ui-redesign]] Phase 6: keep HTTPS `max_open_sockets` low).
- Decode a backtrace PC with `riscv32-esp-elf-addr2line -pfiaC -e build/gps_base_station.elf <addr>` (only meaningful against the *matching* ELF).
