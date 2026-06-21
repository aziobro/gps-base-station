---
name: project_wifi_rpc_crash
description: Interrupt-watchdog crashes from UI/web polling WiFi status via a blocking esp_wifi_sta_get_ap_info RPC to the C6; fixed in ota95 by caching
metadata:
  node_type: memory
  type: project
---

Hard-won root cause + fix for the ESP32-P4's intermittent resets, slow web responses, and NTRIP reconnect storms. See [[project_ntrip]], [[project_structure]], [[project_build_deploy]].

## Symptom
Device reset intermittently (hours apart); `/status` `reset_reason` cycled through `interrupt watchdog`, `panic/exception`, and `software`. Alongside the resets: periodic multi-second `/status` slowness and *simultaneous* `write failed; reconnecting` on all three NTRIP push services. The base station rode through the nightly 3 am router reboot fine — the resets were **not** network-caused (gateway pings stayed clean while the device's own WiFi/C6 link stalled).

## Root cause (decoded 2026-06-19 from the USB-serial backtrace)
`WifiManager::ssid()` and `::rssi()` (`main/wifi_manager.cpp`) each issued a **blocking `esp_wifi_sta_get_ap_info()` RPC to the ESP32-C6 over SDIO** (esp-hosted). They were called on hot paths:
- `Ui::refresh()` (`main/ui.cpp` ~1586) — the 1 Hz LVGL timer, **twice per tick** (ssid + rssi), with the display lock held.
- `AdminWebServer::status_handler` / `root_handler` (`main/web_server.cpp`) — on every `/status` poll and `/` page load.

Under SDIO/C6 contention the RPC's FreeRTOS critical-section spinlock spun cross-core with interrupts off > 300 ms → **interrupt-watchdog reset**. Decoded chain: `Ui::refresh -> ssid()/rssi() -> xQueueSemaphoreTake -> xPortEnterCriticalTimeout -> spinlock_acquire`. The constant RPC traffic also contended the SDIO link behind the slow web + NTRIP churn.

## The fix (ota95, commit 5b9b337)
Cache ssid/rssi in WifiManager; never RPC on the hot path:
- One `esp_wifi_sta_get_ap_info()` refresh from `recovery_loop()` (the existing `wifi_recovery` task), throttled to ≤ 1 per 5 s; the throttle is reset on disconnect so a reconnect refreshes immediately.
- `ssid()`/`rssi()` return cached values, mutex-guarded; the SSID lives in a fixed `char[33]` (a `std::string` member tears under concurrent read/write); the lock is **never** held across the RPC. No caller changes needed.
- Cuts hot-path SDIO RPCs from ~2/s (UI) + 2/web-poll down to ≤ 1 per 5 s.
- **Confirmed:** ~20 h crash-free with flat ~0.4 s `/status` latency, vs ota94 crashing every few hours.

## Rules for next time
- Keep blocking esp_wifi / esp-hosted SDIO RPCs **off the LVGL refresh and the httpd handlers.** Every other such RPC is already correctly offloaded to a dedicated FreeRTOS task — C6 version query (`c6_version_task`), C6 OTA, WiFi scan, credential/AP apply. Don't reintroduce a synchronous one in `Ui::refresh()` or a web handler.
- `esp_netif_get_ip_info` (ip_address / AP ip) and the `connected_` / `access_point_active_` atomics are **P4-local** (not SDIO RPCs) — safe on hot paths.
- Remaining low-frequency inline-on-httpd blockers (the user-initiated WiFi scan and AP-password save) are left as-is — not the hot-path cause.

## Debugging method that found it
A Raspberry Pi capturing the P4's USB console caught the `Guru Meditation` + register dump on a reset. Decode with `riscv32-esp-elf-addr2line -pfiaC -e build/gps_base_station.elf <MEPC/RA + stack code addrs>` against the **exact** ELF for the running firmware — so preserve a copy of `build/gps_base_station.elf` before any re-OTA, or a captured backtrace won't decode. Run the capture *on the Pi itself*, not via a PC SSH terminal (the latter dies when the network drops).
