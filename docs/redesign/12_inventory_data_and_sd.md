I now have a complete picture. Let me compile the inventory.

---

# Data Model, Capabilities & SD Backend Inventory

Files inventoried (all absolute under `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\`): `base_station.hpp/.cpp`, `sd_manager.hpp/.cpp`, `storage.hpp`, `survey.hpp`, `rinex_logger.hpp`, `wifi_manager.hpp`, `ntrip_push.hpp`, `local_caster.hpp`, `app_config.hpp`, `um980.hpp`, `web_server.hpp/.cpp`, `ui.hpp/.cpp`, `app_main.cpp`, `display.hpp`, `log_buffer.hpp`. Plus `docs/agent-memory/project_structure.md`, `project_rinex.md`.

Note on the prompt's path hint: source lives in `main/` (not `gps-base-station/main/`); working dir is already the project root.

---

## 1. STATUS / STATE FIELDS (everything a UI can read)

The single richest accessor is `BaseStation::status()` returning `BaseStationStatus` (by value, snapshot-safe). The web `/status` JSON handler (`web_server.cpp:875`) and the LVGL `Ui::refresh()` (`ui.cpp:1372`) are the two existing consumers â€” they read the *same* accessors, so a unified data contract is already de facto present. Below, grouped logically with the canonical accessor.

### 1.1 Base operation / mode
| Field | Type | Units | Source accessor |
|---|---|---|---|
| Base mode | `BaseMode {kSurvey,kTransmit}` | enum | `station.status().mode` (atomic `mode_`) |
| Healthy (task heartbeat < 2 s old) | `bool` | â€” | `BaseStation::healthy()` (`base_station.cpp:165`) |
| Streams enabled (user NTRIP master toggle) | `bool` | â€” | `BaseStation::streams_enabled()` |
| Streams suspended (effective) | `bool` | â€” | `BaseStation::streams_suspended()` (`effective_streams_suspended()`) |

`effective_streams_suspended()` is OR of: transient-suspend (boot/OTA hold-off), `!user_streams_enabled_`, not-in-TX-mode, no-RTCM-yet, raw-collection-active (`base_station.cpp:328`). UIs should treat "enabled but suspended" as a distinct visual state.

### 1.2 Position (the survey/base coordinate)
Two sources â€” the *stored* fixed position and the *live survey* estimate:
| Field | Type | Units | Accessor |
|---|---|---|---|
| Stored lat / lon / height | `double / double / double` | deg, deg, m (WGS-84) | `Storage::load_position()` â†’ `BasePosition{lat,lon,height,valid}` |
| Position valid | `bool` | â€” | `BasePosition::valid` |

(Web `/status` reads `load_position()` directly; UI reads it in `refresh()` at `ui.cpp:1416`. Note: this is an NVS read on every refresh â€” see Â§4 constraints.)

### 1.3 Survey-in progress (`SurveySnapshot` via `status().survey`, source `SurveyManager::snapshot()`)
| Field | Type | Units | Notes |
|---|---|---|---|
| `state` | `SurveyState {kIdle,kCollecting,kDone,kError}` | enum | |
| `lat / lon / height` | `double` | deg, deg, m | live mean estimate |
| `stability` | `float` | m | block-to-block std error; threshold `kSurveyMaxStabilityM = 0.50` |
| `instantaneous_sigma` | `float` | m | |
| `satellites_used` / `satellites_tracked` | `int` | count | |
| `gps / glonass / galileo / beidou` | `int` | count | per-constellation used |
| `samples` | `int` | count | epochs accumulated |
| `blocks` | `int` | count | 60-s blocks; min `kSurveyMinBlocks = 5` |
| `elapsed_sec` | `uint32_t` | s | min survey `kSurveyMinTimeSec = 300` |
| `valid` | `bool` | â€” | |

Survey completion criteria (for progress UI): `â‰¥300 s` AND `â‰¥5 blocks` AND `stability â‰¤ 0.50 m` (`app_config.hpp:10-13`). Web computes a progress bar as `min(elapsed,300)/300`; this is **time-only** and understates true completion (ignores blocks/stability). A redesign should show all three gates.

### 1.4 Satellites / sky plot
| Field | Type | Units | Accessor |
|---|---|---|---|
| Per-sat: `prn, elevation, azimuth, snr, system` | `uint16/uint8/uint16/uint8/uint8` | â€”, deg, deg, dB-Hz, sysid | `BaseStation::satellites(SatelliteInfo* out, size_t cap)` â†’ `SurveyManager::satellites()` |

Max 64 sats buffered (`survey.hpp:65`). `system` ids: 0=GPS 1=GLO 3=GAL 4=BDS 5=QZSS. Web `/skyplot/data` serializes these; touch has no sky plot today (web-only â€” but the data is available to both).

### 1.5 RTCM throughput
| Field | Type | Units | Accessor |
|---|---|---|---|
| RTCM bytes/sec | `uint32_t` | B/s | `status().rtcm_bytes_per_second` |
| RTCM bytes total | `uint64_t` | bytes | `status().rtcm_bytes_total` |

### 1.6 NTRIP push services (3 instances: rtk2go, onocoy, rtkdata) â€” `NtripStatus` each
Accessors: `status().rtk2go`, `.onocoy`, `.rtkdata` (each = `NtripPushClient::status()`).
| Field | Type | Units |
|---|---|---|
| `enabled` | `bool` | â€” |
| `connected` | `bool` | â€” |
| `message` | `std::string` | live state text ("connecting"/"connected"/â€¦) |
| `bytes_sent` | `uint64_t` | bytes |
| `dropped_batches` | `uint32_t` | count (queue-full drops) |
| `reconnects` | `uint32_t` | count |
| `last_error` | `std::string` | sticky failure reason |
| `connected_sec` | `uint32_t` | s (current connection age) |
| `last_send_age_sec` | `uint32_t` | s since last payload |
| `ever_sent` | `bool` | â€” |

Host/port are compile-time (`app_config.hpp`): rtk2go.com / servers.onocoy.com / rtkdata.online, all :2101. Onocoy uses NTRIP v2; the other two v1.

### 1.7 Local NTRIP caster
| Field | Type | Units | Accessor |
|---|---|---|---|
| Client count | `int` | count | `status().local_clients` |
| Client IPs | `ClientSnapshot {count, array<uint32_t,8>}` | IPv4 (be) | `status().local_client_ips` (`LocalCaster::client_snapshot()`) |

Listens on `kLocalNtripPort = 2101`, mountpoint `BASE0`, max 8 clients (`local_caster.hpp:13`).

### 1.8 RINEX logging (`RinexLogger::Status` via `BaseStation::rinex_status()`)
| Field | Type | Units |
|---|---|---|
| `active` | `bool` | â€” |
| `epochs` | `int` | count (current file; 120 epochs/file @ 30 s = 1 hr) |
| `files` | `int` | count (closed since start) |
| `current_file` | `std::string` | path under `/sdcard/rawdata/` |

Persisted enable flag (survives reboot): `Storage::rinex_collection_enabled()` (default off).

### 1.9 WiFi (all on `WifiManager`, all non-blocking â€” cached, see Â§4)
| Field | Type | Units | Accessor |
|---|---|---|---|
| State | `State {kDisconnected,kConnected,kAccessPoint}` | enum | `state()` |
| Connected | `bool` | â€” | `connected()` |
| AP active | `bool` | â€” | `access_point_active()` |
| RSSI | `int` | dBm | `rssi()` (cached, default âˆ’127) |
| Station SSID | `std::string` | â€” | `ssid()` (cached 32-byte buffer) |
| Station IP | `std::string` | â€” | `ip_address()` |
| AP SSID | `std::string` | â€” | `access_point_ssid()` ("" when down) |
| AP gateway IP | `std::string` | â€” | `access_point_ip()` |
| Scan results | `vector<WifiNetwork{ssid,rssi,secured}>` | â€” | `scan_networks()` (**blocking** â€” must run off UI/httpd task) |

AP defaults: SSID `GPS-BaseStation`, password `gpsbase-rtk` (config, overridable in NVS).

### 1.10 System / firmware / health
| Field | Type | Units | Source |
|---|---|---|---|
| Firmware version | `const char*` | â€” | `esp_app_get_description()->version` (from `version.txt`) |
| Compile date/time | string | â€” | `esp_app_get_description()->date/time` (UI System tab) |
| IDF framework version | string | â€” | `esp_get_idf_version()` |
| Uptime | seconds | s | `esp_timer_get_time()/1e6` |
| Reset reason | enumâ†’string | â€” | `esp_reset_reason()` â†’ `reset_reason_str()` |
| Free heap (8-bit) | `size_t` | bytes | `heap_caps_get_free_size(MALLOC_CAP_8BIT)` |
| Heap total | `size_t` | bytes | `heap_caps_get_total_size(...)` |
| Min-ever free heap | `size_t` | bytes | `heap_caps_get_minimum_free_size(...)` |
| C6 running fw version | `char[48]` | â€” | `esp_hosted_get_coprocessor_fwversion()` (**SDIO RPC â€” off-UI-task only**, see `ui.cpp:1352`) |
| C6 available fw version | `char[64]` | â€” | parsed from embedded `c6_slave_fw.bin` app-desc (local, no RPC) |

### 1.11 SD card
| Field | Type | Units | Accessor |
|---|---|---|---|
| Mounted | `bool` | â€” | `SdManager::is_mounted()` |
| Total / used bytes | `uint64_t` | bytes | `SdManager::disk_stats()` â†’ `DiskStats{total_bytes,used_bytes,valid}` |

### 1.12 Antenna metadata (NVS, feeds RINEX header)
| Field | Type | Units | Accessor |
|---|---|---|---|
| Model | `std::string` (â‰¤16) | â€” | `Storage::antenna_model()` default `HXCGPS500` |
| Radome | `std::string` (â‰¤4) | â€” | `Storage::antenna_radome()` default `NONE` |
| ARP delta-H | `double` | m | `Storage::antenna_height()` default 0.0 |

### 1.13 Console log
- `log_buffer::snapshot()` â†’ full ~16 KB ring buffer text; `log_buffer::snapshot_since(cursor)` â†’ incremental (`Snapshot{next,truncated,text}`). Web `/logs` uses the incremental cursor; touch Debug tab renders the tail. Web-only full console is acceptable divergence; touch shows a tail.

---

## 2. COMMANDS / ACTIONS (everything a UI can invoke)

All web actions are gated by `authorize()` (admin password / session). All state-changing base actions are queued onto a 4-deep FreeRTOS queue and processed on the base task â€” they return `ESP_OK`/`ESP_ERR_TIMEOUT` immediately (non-blocking, can fail if queue full).

| Action | Accessor / web route | Args | Preconditions | Effects |
|---|---|---|---|---|
| Start / restart survey | `BaseStation::request_survey()` Â· `POST /survey` | none | base task running | Clears stored position, enters survey mode, suspends all RTCM streams, reconfigures UM980 for survey output |
| Set fixed position (manual) | `BaseStation::request_position(lat,lon,height)` Â· `POST /config/position` | lat/lon/hgt | valid ranges (lat Â±90, lon Â±180, hgt âˆ’1000..20000) | Saves position to NVS, enters Base TX mode, configures UM980 base, exits raw collection if active |
| (auto) Surveyâ†’TX transition | internal on `take_completed_result()` | â€” | survey criteria met | Saves position, enters TX |
| Toggle RINEX raw collection | `BaseStation::request_raw_collection(bool)` Â· `POST /rinex/toggle` (`start=1/0`) | bool | **enabling requires Base TX mode** (else warns, no-op); SD mounted for files to write | Persists intent (resumes after reboot); suspends RTCM, switches UM980 to RANGEA 30 s, starts/stops `RinexLogger` with current antenna metadata |
| Global NTRIP on/off (master) | `BaseStation::set_streams_enabled(bool)` Â· `POST /ntrip/toggle` (`on=1/0`) | bool | â€” | Persists to NVS, applies suspend state to all 3 push clients + local caster |
| Configure NTRIP service creds | `Storage::save_service()/set_service_enabled()` + `BaseStation::reload_services()` Â· `POST /config` | name, mountpoint, password, enable | â€” | Persists creds, reconfigures the live push client (no reboot) |
| WiFi connect / set creds | `Storage::save_wifi()` Â· `POST /config/wifi` | ssid (â‰¤32), password (â‰¤64) | ssid non-empty | Persists; **reboots device** (`restart_task`) |
| WiFi scan | `WifiManager::scan_networks()` Â· `GET /wifi/scan` | none | â€” | Blocking scan; web returns JSON, touch runs `wifi_scan_task` |
| Set AP (hotspot) password | `Storage::save_ap_password()` + `WifiManager::apply_ap_settings()` Â· `POST /config/ap` | password (8â€“63) | length valid | Persists; applies live (drops current AP clients), no reboot |
| Set antenna metadata | `Storage::save_antenna()` Â· `POST /config/antenna` | model(â‰¤16), radome(â‰¤4), height | â€” | Persists; takes effect on next RINEX file |
| Set admin password | `Storage::save_admin_password()` Â· setup/config | password | â€” | Auth credential |
| Firmware OTA (P4 app) | `POST /update` (multipart raw `.bin`) | binary | size â‰¤ partition (6 MB) | Suspends streams, `esp_ota_begin/write/end/set_boot_partition`, **reboots**; `validate_ota_task` confirms health within 30 s or rolls back |
| C6 coprocessor OTA | `esp_hosted_slave_ota_*` (`ui.cpp:1288`) â€” **touch only today** | embedded image | â€” | Flashes C6 from embedded `c6_slave_fw.bin`, activates, restarts P4. No web route exists â€” candidate to add for parity, or keep touch-only |
| List directory | `SdManager::list_dir()` Â· `GET /files/list?path=` | path | mounted, `safe_path` | Returns JSON entries |
| Download file | `GET /files/download?path=` (web-only) | path | mounted, not a dir | Streamed 4 KB chunks |
| Delete entry | `SdManager::delete_entry()` Â· `POST /files/delete` {path} | path | mounted, `safe_path` | unlink (file) or rmdir (empty dir only) â€” see Â§3 |
| Rename / move | `SdManager::rename_entry()` Â· `POST /files/rename` {from,to} | from,to | both `safe_path` | `rename()` |
| Make directory | `mkdir()` Â· `POST /files/mkdir` {path} | path | mounted, `safe_path` | mkdir 0755 |
| RINEX bulk export (merge) | `POST /rinex/export` {start,end} (web-only) | datetime range | files exist in range | Merges hourly RINEX into one `export.rnx`, patching TIME OF LAST OBS |

**Web-only capabilities (acceptable divergence per the brief):** file download, P4 OTA upload, RINEX bulk export/merge, full console log viewer. **Touch-only today:** C6 OTA (data/action exists only on touch).

---

## 3. SD MANAGER API â€” DETAIL & BULK-DELETE GAP ANALYSIS

(`sd_manager.hpp`, `sd_manager.cpp`.) Mount point constant `kMountPoint = "/sdcard"`. SD runs over **SDSPI on SPI2** (native SDMMC is held by esp_hosted/WiFi), CS=42/CLK=43/MOSI=44/MISO=39.

### 3.1 Current signatures
- `esp_err_t mount()` / `void unmount()` / `bool is_mounted()`
- `DiskStats disk_stats() const` â€” `{total_bytes, used_bytes, valid}` via `esp_vfs_fat_info` (used = total âˆ’ free).
- `static const char *safe_path(const char *path)` â€” returns `path` if valid, else `nullptr`. Guards: non-empty; **must begin with `/sdcard`**; next char must be `/` or NUL (prevents `/sdcardevil`); rejects any occurrence of `".."`. **Returns the same pointer â€” purely a validator.**
- `char *list_dir(const char *path) const` â€” **heap-allocated JSON array** (`[{"name","path","is_dir","size"},â€¦]`), caller must `free()`. Uses `readdir` d_type (DT_DIR/DT_REG), `stat` only for size. Grows buffer via realloc. Returns `nullptr` on OOM/unopenable.
- `bool delete_entry(const char *path) const` â€” `stat`s the path; if dir â†’ `rmdir`, else `unlink`. Returns success.
- `bool rename_entry(const char *from, const char *to) const` â€” `rename()`.
- `void ensure_dirs() const` â€” creates `/sdcard/logs` and `/sdcard/rawdata` if missing.

### 3.2 Does `delete_entry` handle directories / recursion?
**No recursion.** It calls `rmdir()` for directories, which **fails (ENOTEMPTY) on any non-empty directory**. So today: deletes a single file, or an *empty* directory only. There is **no multi-file API and no recursive directory delete.** The web `/files/delete` handler deletes exactly one path per request (`web_server.cpp:1663`); the touchscreen file browser has **no delete at all** (navigation-only â€” `on_fb_item` explicitly comments "long-press could delete but for now tap does nothing for files", `ui.cpp:1175`).

### 3.3 What must be ADDED for safe bulk delete (both surfaces)
This is the core backend work the redesign implies. Recommended additions to `SdManager`:

1. **Recursive directory delete** â€” new `bool delete_recursive(const char *path)`: depth-first `opendir`â†’recurseâ†’`unlink` filesâ†’`rmdir` on the way up. Must re-validate `safe_path()` on the root and ideally re-check each child stays under `/sdcard` (defense in depth; FATFS has no symlinks so traversal risk is low, but keep the `..` guard).
2. **Multi-file delete** â€” either a batch accessor `int delete_many(const char* const* paths, size_t n)` returning success count, or have the web/UI layer loop `delete_entry` per path. A batch call is preferable so progress and partial-failure reporting are centralized.
3. **Protected-path guard (CRITICAL â€” does not exist today).** Nothing currently prevents deleting the mount root or the managed dirs. Add a deny-list enforced *inside* delete so neither surface can bypass it:
   - Never allow deleting `/sdcard` itself (the mount root).
   - Protect the managed directories themselves (`/sdcard/logs`, `/sdcard/rawdata`) from deletion *as directories* â€” allow deleting their *contents* but recreate-on-demand via `ensure_dirs()`. (Decide with product whether "delete entire rawdata" should wipe contents but keep the dir.)
   - Reject the active RINEX file: cross-check `rinex_status().current_file` and refuse to delete it (or any path) while `rinex_active` â€” deleting an open `FILE*` corrupts the logger. This check needs `BaseStation`/`RinexLogger` visibility, so enforce it at the handler/UI layer, not in `SdManager`.
4. **Path-traversal hardening for batch** â€” apply `safe_path()` to *every* element before acting; reject the whole batch if any element fails (atomic-ish validation), so one bad path can't slip through.
5. **Atomicity / partial failure** â€” filesystem delete is inherently non-atomic. Define semantics: best-effort, continue-on-error, return per-item results (success count + first error). Re-list the directory after the operation so the UI reflects reality regardless of partial failure.
6. **Progress for large dirs** â€” a full `rawdata` can hold hundreds of hourly files. Recursive delete on SDSPI is slow and **must not run on the LVGL or httpd task** (it would trip the watchdog and block rendering/serving). Run on a dedicated worker task; expose an atomic progress counter (deleted/total) the same way C6 OTA progress is surfaced (`std::atomic<int>` polled by refresh, `ui.cpp:1664`). Web can poll a status endpoint or stream.
7. **Confirmation/warning data** â€” for "delete entire directory," the UI needs a pre-count + total bytes to show in the confirmation ("Delete rawdata/ â€” 214 files, 1.7 GB?"). Add a helper `bool dir_stats(path, uint64_t& files, uint64_t& bytes)` or reuse `list_dir` counts.

### 3.4 Touch multi-select implications
The current touch browser builds an `lv_list` of buttons (`refresh_file_browser`, `ui.cpp:959`) with per-item click â†’ navigate. For finger-friendly multi-select you'll add: a per-row checkbox (or long-press to toggle selection state stored in a `std::vector<bool>`/set keyed to `fb_entries_`), a selection-count header, and Delete/Delete-All actions that route through the new batch API behind an `lv_msgbox` confirmation (the pattern already exists for survey restart and C6 OTA confirmations). Selection state must be mutated under `bsp_display_lock()`.

---

## 4. RESOURCE CONSTRAINTS BOUNDING UI COMPLEXITY

**Heap / memory**
- Tight internal SRAM: ~95 KB internal free before main task (per brief). PSRAM available and used for LVGL framebuffers (BSP).
- Live free-heap/min-free are observable (`heap_caps_*`). OTA upload deliberately suspends streams + waits 1.1 s to free TCP buffers before flashing (`web_server.cpp:1032`) â€” memory is genuinely contended during heavy ops.
- `list_dir` allocates the entire directory JSON on the heap (grows via realloc). A huge directory = large transient allocation; for the file browser, prefer paging or cap entries on the touch side.
- Avoid large static UI assets; the brief calls out flash/heap modesty. App partitions app0/app1 are 6 MB each (32 MB flash), OTA rollback enabled â€” flash headroom is fine, but keep CSS/HTML/asset growth modest since web HTML is built from C++ string literals streamed in chunks.

**Task / concurrency model**
- LVGL runs in the BSP's own task; **all object mutation must be under `bsp_display_lock()/unlock()`**.
- The base station runs one pinned task (core 1, prio 6, 7168-byte stack) draining a 4-deep action queue. UI/web actions are fire-and-forget enqueues â€” design for async (the UI won't get a synchronous result; poll status).
- **Blocking calls that must never run on the LVGL or httpd task:** anything that issues an SDIO RPC to the C6 (`esp_wifi_*` config, `esp_hosted_get_coprocessor_fwversion`, slave OTA), WiFi `scan_networks()`, and (newly) recursive SD delete. Existing code spawns short-lived tasks for these (`wifi_scan_task`, `c6_version_task`, `c6_ota_task`, `ap_apply_task`) â€” follow that pattern.
- WiFi `ssid()`/`rssi()` are deliberately **cached** (refreshed â‰¤ every 5 s by a recovery task) precisely because the live RPC blocks; a prior bug (IWDT WiFi-RPC crash, fixed ota95) came from UI/web calling the RPC directly. Treat these as cheap reads; do not add new direct `esp_wifi_sta_get_ap_info` calls.
- `Storage::load_position()` is an NVS read and is currently called on *every* UI refresh and every `/status` request. A higher-refresh marine-instrument readout should cache position in RAM rather than re-reading NVS each tick.

**Display**
- Fixed 720Ã—720, no mouse scrollbars; touch scrolling OK. Touch targets must be finger-sized. Existing buttons are ~40 px tall (e.g. C6 OTA button `LV_PCT(100) Ã— 40`) â€” a reasonable minimum.

**Networking**
- HTTPS on 443; large transfers (file download, RINEX export) are throttled to ~400 KB/s (4 KB chunks + 1-tick delay) to stay within esp_hosted SDIO bandwidth and avoid TCP drops. Any new bulk web operation should follow the same chunk+pace discipline.

**Key unification insight:** `BaseStation::status()` (+ `rinex_status()`, `satellites()`, the `WifiManager` cached getters, `disk_stats()`, `load_position()`, and `esp_app_get_description()`) already form a complete, snapshot-safe read model that **both** surfaces consume identically today. A redesign can define one logical "status struct â†’ view" mapping and render it on web and touch with shared section names/colors. The only genuine backend gap for the stated feature set is **bulk/recursive SD delete with a protected-path guard, off-task execution, progress, and active-file protection** â€” none of which exists yet.
