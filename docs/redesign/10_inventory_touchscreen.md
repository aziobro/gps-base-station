I now have the complete file. Here is the exhaustive inventory.

---

# TOUCHSCREEN UI INVENTORY â€” GPS Base Station (LVGL 9.x, 720x720)

Source: `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\ui.cpp` and `ui.hpp`. All file paths absolute. Em-dash placeholder `\xe2\x80\x94` ("â€”") is the default "no data" value for every value label.

---

## 0. GLOBAL ARCHITECTURE & SHELL

### Top-level layout
- Single `lv_screen_active()` screen, background `kBgScreen = 0x0d1b2a` (very dark navy).
- One full-screen `lv_tabview` (720x720, pos 0,0), **tab bar at BOTTOM** (`LV_DIR_BOTTOM`), tab-bar height **52 px**.
- LVGL default theme initialized with primary `LV_PALETTE_BLUE`, secondary `LV_PALETTE_BLUE_GREY`, **dark mode = true**, default font.
- 5 tabs, in order, each with a built-in symbol glyph in its tab label:
  1. `LV_SYMBOL_HOME " Status"`
  2. `LV_SYMBOL_WIFI " NTRIP"`
  3. `LV_SYMBOL_GPS " Position"`
  4. `LV_SYMBOL_SETTINGS " System"`
  5. `LV_SYMBOL_LIST " Debug"`

### Color palette (hard-coded constants, ui.cpp lines 52â€“57)
| Constant | Hex | Use |
|---|---|---|
| `kBgScreen` | `0x0d1b2a` | screen / tab / modal background |
| `kBgGroup` | `0x152638` | card (group box) + title-bar + list background |
| `kBorderCol` | `0x1f3d5c` | card borders, dividers |
| `kKeyCol` | `0x5a8098` | row "key" (left label) text |
| `kTitleCol` | `0x3d6480` | group-title header text |
| `kDimCol` | `0x3a5570` | dimmed / inactive / placeholder text |

Semantic status colors come from LVGL palettes: `LV_PALETTE_GREEN` (good/connected/active), `LV_PALETTE_ORANGE` (survey/warn/reconnects), `LV_PALETTE_RED` (error/crash/fail), `LV_PALETTE_YELLOW` (in-progress / connecting). Debug log text is a unique green `0x7aaa7a`. NTRIP "Config" buttons and detail group titles use literal hexes `0x152638 / 0x1f3d5c / 0x3d6480 / 0x5a8098` (duplicated, not via the named constants).

### Fonts
Only `lv_font_montserrat_14` is referenced explicitly (group headers, NTRIP detail rows, debug label, sat-detail label, modal title bars, NTRIP config buttons). Everything else uses `LV_FONT_DEFAULT`. **There are NO large numeric-readout fonts today** â€” all values are default ~14px body text. (Major gap vs. the marine-instrument "large high-contrast numeric" target.)

---

## 1. REUSABLE LAYOUT HELPERS / PATTERNS

These are the building blocks a redesign must either keep or replace consistently.

### `make_group(parent, title)` â†’ card container (ui.cpp 149)
Titled "group box" card. `LV_PCT(100)` width, `LV_SIZE_CONTENT` height. Bg `kBgGroup`, 1px border `kBorderCol`, radius 8, `pad_all 12`, `pad_row 0`, column flex. Adds an uppercase title label (`kTitleCol`, montserrat_14, 6px bottom pad) + a 1px full-width divider line (`kBorderCol`, 6px bottom margin). Children (rows) are appended directly.

### `make_row(parent, &val_out, key)` â†’ row (ui.cpp 183)
Key/value row. Row flex (row direction, items center-aligned, 4px vertical pad). Left: fixed **148 px** key label (`kKeyCol`). Right: value label, `flex_grow 1`, default text "â€”". Returns the value label via `val_out`.

### `make_switch_row(parent, &sw_out, key)` â†’ row (ui.cpp 205)
Label-left / switch-right row. `LV_FLEX_ALIGN_SPACE_BETWEEN`, 6px vertical pad. Left: flex-grow key label (`kKeyCol`). Right: `lv_switch`. Returns the switch via `sw_out`.

### `ntrip_detail_group(...)` (file-static, ui.cpp 412)
Specialized card for NTRIP services (NOT using make_group). Same card styling as make_group but its header row is title (flex-grow) **+ an 80x32 "âš™ Config" button** (montserrat_14). Below the divider it builds 4 fixed rows via a local lambda `make_r` (148px key + flex value): **Status / Bytes sent / Dropped / Reconnects**. The Config button stores its service index in `lv_obj_set_user_data` and fires `Ui::on_ntrip_cfg_btn`.

### `make_modal_base(parent, title)` â†’ modal root (ui.cpp 226)
Full-screen modal on `lv_layer_top()`. Size = `Display::kWidth x Display::kHeight` (720x720), pos 0,0, bg `kBgScreen`. Column flex. **Created HIDDEN** (`LV_OBJ_FLAG_HIDDEN`). Builds a **52px title bar** (`kBgGroup`, space-between row): title label (montserrat_14, flex-grow) + a **40x40 close button** showing `LV_SYMBOL_CLOSE`. Then a 1px divider. Returns the modal root; callers fetch the title bar as `child(0)` and the close button as the last child of the bar.

### Keyboard handling pattern (ui.cpp 295â€“312)
- One `lv_keyboard` per modal (`kb_wifi_`, `kb_ntrip_`), sized `kWidth x (kHeight*40/100)` (full width, 40% height â‰ˆ 288px), created hidden.
- `on_ta_focused`: binds keyboard to the focused textarea (`lv_keyboard_set_textarea`) and unhides it. Wired via `LV_EVENT_FOCUSED`, user_data = the keyboard.
- `on_ta_defocused`: hides the keyboard (`LV_EVENT_DEFOCUSED`).
- `on_kb_ready`: on `LV_EVENT_READY` (checkmark/Enter) or `LV_EVENT_CANCEL` (âœ•), defocuses the textarea and hides the keyboard.
- Keyboard is the last flex child of the modal so it naturally sits at the bottom (comment notes this is a layout "workaround," lines 646â€“648).

### `style_tab(tab)` (file-static, ui.cpp 316)
Applies to every tab page: column flex, top-start align, `pad_all 14`, `pad_row 10`, **vertical-only scroll** (`LV_DIR_VER`), bg `kBgScreen`. Tabs scroll by touch when content overflows.

### Formatting helpers
- `fmt_bytes_str(buf,n,bytes)` (137): humanizes bytes â†’ `GB / MB / KB / B` (1 decimal for GB/MB, 0 for KB).
- `fmt_ntrip_label(lbl,ns,buf,n)` (272): renders an `NtripStatus` into a colored status string â€” see Â§2/Â§3.
- `ipv4_to_text` / `local_client_ips_to_text` (file-static, 69 / 83): format caster client IPs; empty list â†’ "â€”".
- `reset_reason_str` / `reset_is_crash` (30 / 46): map `esp_reset_reason_t` to text and to a crash flag (panic, INT/TASK/generic WDT, brownout = crash â†’ red).

---

## 2. TAB: STATUS (`build_status_tab`, ui.cpp 367)
Purpose: at-a-glance operational dashboard.

### Group "BASE OPERATION"
| Label (key) | Value shown | Source | Notes / color |
|---|---|---|---|
| Mode | `SURVEY` or `BASE TX` | `station_->status().mode` (`BaseMode::kSurvey`/`kTransmit`) | SURVEY = orange, BASE TX = green |
| RTCM output | `"<n> B/s  (total: <bytes>)"` or "â€”" | `st.rtcm_bytes_per_second`, `st.rtcm_bytes_total` | only shown when mode=Transmit AND bps>0 |
| Satellites | `"G:n  R:n  E:n  C:n  (n tracked)"` | `st.survey.gps/glonass/galileo/beidou/satellites_tracked` | |

**Control â€” Survey button** (`btn_survey_start_`): full-width, 40px, orange (`darken(ORANGE,2)`), 8px top margin. Label (`lbl_survey_btn_`) is dynamic:
- Initial: `LV_SYMBOL_REFRESH "  Start Survey"`
- In SURVEY mode â†’ `"âŸ³ Restart Survey"`
- In BASE TX mode â†’ `"âŸ³ Start New Survey"`
- Action: `on_survey_start` â†’ see Survey flow in Â§7.

### Group "POSITION"
| Label | Value | Source |
|---|---|---|
| Latitude | `"%.7fÂ°"` or "â€”" | `storage_->load_position()` (`BasePosition.lat`, only if `.valid`) |
| Longitude | `"%.7fÂ°"` or "â€”" | `.lon` |
| Height | `"%.4f m"` or "No position stored" | `.height` |

Plus a **survey progress block** (hidden unless mode=SURVEY):
- `lbl_survey_` (orange): `"Survey  <n>s  blocks:<n>  Â±<x.xx>m"` from `sv.elapsed_sec`, `sv.blocks`, `sv.stability`.
- `bar_survey_` (`lv_bar`, 10px, range 0â€“300): value = `min(elapsed_sec, 300)`. (300s â‰ˆ the survey target.)

### Group "NTRIP CASTERS" (compact)
| Label | Value | Source | Color logic |
|---|---|---|---|
| RTK2go | via `fmt_ntrip_label` | `st.rtk2go` | Disabled=dim / Connected ("Connected  up <Xh/m/s>")=green / else last_error or "Connectingâ€¦"=red |
| Onocoy | same | `st.onocoy` | same |
| RTKdata | same | `st.rtkdata` | same |
| Local :2101 | `"<n> client(s)"` | `st.local_clients` | green if >0 else dim |
| Client IPs | comma-sep IP list or "â€”" | `st.local_client_ips` (`LocalCaster::ClientSnapshot`) | green if clients>0 else dim |

---

## 3. TAB: NTRIP (`build_ntrip_tab`, ui.cpp 486)
Purpose: detailed per-service NTRIP push management + config entry points.

### Group "ALL NTRIP SERVICES"
- One inline switch row (NOT make_switch_row): label "Enable all services" + `sw_ntrip_all_`. **Default checked (on).**
- Action `on_ntrip_all_toggle` â†’ `station_->set_streams_enabled(checked)` â€” **persists across power cycles** (comment line 1199).
- Kept in sync each refresh from `station_->streams_enabled()` without retriggering callback (refresh lines 1392â€“1399).

### Three detail cards: "RTK2go" (idx 0), "Onocoy" (idx 1), "RTKdata" (idx 2)
Each card (built by `ntrip_detail_group`) has a **"âš™ Config" button** (â†’ `on_ntrip_cfg_btn` â†’ opens NTRIP config modal for that index) and 4 rows:
| Row | Value | Source (`NtripStatus ns`) |
|---|---|---|
| Status | `fmt_ntrip_label` text/color | `ns.enabled/connected/connected_sec/message/last_error` |
| Bytes sent | `"<bytes>  (last <n>s ago)"` if ever_sent, else just bytes; "â€”" if disabled | `ns.bytes_sent`, `ns.ever_sent`, `ns.last_send_age_sec` |
| Dropped | `"<n>"` or "â€”" | `ns.dropped_batches` |
| Reconnects | `"<n>"` or "â€”"; orange if >0 else dim | `ns.reconnects` |

### Group "LOCAL CASTER :2101"
| Label | Value | Source | Color |
|---|---|---|---|
| Clients | `"<n> client(s)"` | `st.local_clients` | green if >0 else dim |
| Client IPs | comma-sep list / "â€”" | `st.local_client_ips` | green if >0 else dim |

---

## 4. TAB: POSITION (`build_position_tab`, ui.cpp 520)
Purpose: fixed-base coordinates + full survey quality + satellite breakdown.

### Group "FIXED BASE POSITION"
| Label | Value | Source |
|---|---|---|
| Latitude | `"%.7fÂ°"` / "â€”" | `BasePosition.lat` (`storage_->load_position()`) |
| Longitude | `"%.7fÂ°"` / "â€”" | `.lon` |
| Height | `"%.4f m"` / "No position stored" | `.height` |
- Plus duplicate survey block: `lbl_d_survey_` (orange text) + `bar_d_survey_` (same range 0â€“300), both hidden unless SURVEY. Mirror of the Status-tab survey widgets â€” both updated together in `refresh()`.

### Group "SURVEY QUALITY"
| Label | Value | Source (`SurveySnapshot sv`) |
|---|---|---|
| Elapsed | `"<n>s"` | `sv.elapsed_sec` |
| Blocks | `"<n>"` | `sv.blocks` |
| Samples | `"<n>"` | `sv.samples` |
| Stability Â± | `"%.3f m"` | `sv.stability` |
| Inst. sigma | `"%.3f m"` | `sv.instantaneous_sigma` |

### Group "SATELLITES"
| Label | Value | Source |
|---|---|---|
| Used / tracked | `"<used> / <tracked>"` | `sv.satellites_used`, `sv.satellites_tracked` |
| GPS | `"<n>"` | `sv.gps` |
| GLONASS | `"<n>"` | `sv.glonass` |
| Galileo | `"<n>"` | `sv.galileo` |
| BeiDou | `"<n>"` | `sv.beidou` |

- `lbl_sv_detail_` (wrapping multi-line, montserrat_14, dim color): per-satellite SNR summary. Pulls up to 32 sats via `station_->satellites()`, sorts by SNR descending, prints **top 16** as `"<S><PRN%100>:<SNR>dB  "` where system letter map = `{G,R,?,E,C,J}`. Fallback "No satellite data".

---

## 5. TAB: SYSTEM (`build_system_tab`, ui.cpp 565)
Purpose: device health, network, storage, firmware + the major control buttons.

### Group "SYSTEM"
| Label | Value | Source | Color |
|---|---|---|---|
| Uptime | `"<d>d HH:MM:SS"` or `"HH:MM:SS"` | `esp_timer_get_time()` | |
| Last reset | reset-reason text | `esp_reset_reason()` via `reset_reason_str` | RED if crash, else dim |

### Group "NETWORK"
| Label | Value | Source | Color |
|---|---|---|---|
| Station | `"<ssid>  <rssi> dBm"` / `"Connecting to <ssid>â€¦"` / `"Not configured"` | `wifi_->connected()`, `wifi_->ssid()`, `wifi_->rssi()`, else `storage_->load_wifi()` | green / yellow / dim |
| Station IP | IP or "â€”" | `wifi_->ip_address()` | |
| Hotspot | AP SSID or "off" | `wifi_->access_point_ssid()` | green if up, dim if off |
| Hotspot IP | IP or "â€”" | `wifi_->access_point_ip()` | |

**Control â€” "ðŸ“¶ Configure WiFi" button** (full-width 40px) â†’ `on_wifi_btn` â†’ opens WiFi modal (Â§7).

### Group "STORAGE"
| Label | Value | Source |
|---|---|---|
| SD card | `"<used> / <total>"` / "Mounted" / "Not mounted" | `sd_->is_mounted()`, `sd_->disk_stats()` |
| RINEX collection (switch row) | switch state | `sw_rinex_`, synced from `rinex_status().active` |
| Current file | `"<filename>  ep:<epochs>"` / "Active" / "â€”" | `station_->rinex_status()` (`current_file`, `epochs`) |

- **Control â€” RINEX switch** (`sw_rinex_`) â†’ `on_rinex_toggle` â†’ `station_->request_raw_collection(enabled)`. Switch is force-synced to real state each refresh so it never fights the actual logger.
- **Control â€” "ðŸ“ Browse SD Card" button** (full-width 40px) â†’ `on_files_btn` â†’ file browser modal at `SdManager::kMountPoint` (Â§7).

### Group "FIRMWARE"
| Label | Value | Source |
|---|---|---|
| Firmware | version string | `esp_app_get_description()->version` |
| Built | `"<date>  <time>"` | `desc->date`, `desc->time` |
| C6 running | RPC fw version `"M.m.p"` / "queryingâ€¦" / "unknown" | `c6_version_task` via `esp_hosted_get_coprocessor_fwversion` |
| C6 available | embedded-image version `"<ver>  (<date>)"`; doubles as OTA status: "Flashingâ€¦ N%" / "Update failed" | `load_c6_available_version()` parses embedded image app-desc; `c6_ota_progress_` |

- **Control â€” "â¬† Update C6 Firmware" button** (`btn_c6_ota_`, full-width 40px) â†’ `on_c6_ota_btn` â†’ confirm msgbox â†’ C6 OTA flow (Â§7). Disabled while flashing.

---

## 6. TAB: DEBUG (`build_debug_tab`, ui.cpp 615)
Purpose: live console log tail (read-only).
- One card-ish container (`kBgGroup`, no border, pad 8) with `lbl_debug_`: wrapping multi-line label, montserrat_14, green `0x7aaa7a`, initial "Log output will appear here...".
- Source: `log_buffer::snapshot()` (the shared console buffer also served by web `/logs`). Renders only the **tail (last `kDebugTailChars = 4000` chars)**, trimmed to start at a line boundary. Empty â†’ "(no log output yet)".
- Auto-scrolls the **tab page** (not inner container) to bottom each update via `lv_obj_get_scroll_y + lv_obj_get_scroll_bottom`.
- Updated **every OTHER refresh tick** (every 2s) to cut overhead (`debug_tick` counter, lines 1679â€“1683).
- No interactive controls. (Web has the full log view; this is the touchscreen-degraded equivalent.)

---

## 7. MODAL FLOWS

All modals are lazily built on first open, live on `lv_layer_top()`, are full-screen 720x720, start hidden, and are dismissed by hide (not destroyed). Each has a 52px title bar with a 40x40 âœ• close button.

### 7a. WiFi Setup modal (`build_wifi_modal` 633 / `open_wifi_modal` 755)
Title "WiFi Setup". On open, pre-fills from `storage_->load_wifi()` (SSID+password) and AP password from `storage_->ap_password()` (fallback `config::kDefaultApPassword`); clears both message lines.

Content (topâ†’bottom), with shared keyboard `kb_wifi_` at bottom:
1. **"Network SSID:"** label + `ta_wifi_ssid_` one-line textarea (placeholder "Network name").
2. **"Password:"** label + `ta_wifi_pass_` one-line, **password-masked** textarea (placeholder "Password").
3. **Button row (space-between):**
   - **"âŸ³  Scan"** (160x40) â†’ `on_wifi_scan`: guards re-entry via `scan_running_` atomic; clears list, shows "âŸ³  Scanning..."; spawns `wifi_scan_task` (separate FreeRTOS task, prio 5, 4096 stack) which calls `wifi_->scan_networks()`, then locks display and calls `populate_wifi_scan_list`.
   - **"ðŸ“¶  Connect"** (160x40) â†’ `on_wifi_connect`: validates SSID non-empty (else red "SSID cannot be empty"); saves `WifiCredentials{ssid,pass,valid=true}` via `storage_->save_wifi`; shows yellow "Saved. Connecting..."; hides modal+keyboard; spawns `wifi_connect_task` (prio 5) calling `wifi_->update_credentials` â€” **off the LVGL task because it does SDIO RPC to the C6**.
4. `lbl_wifi_msg_` status line (dim by default; turns red/yellow per outcome).
5. **Hotspot section:** label `"Hotspot \"<AP SSID>\" password (WPA2):"` (`config::kAccessPointSsid`) + `ta_ap_pass_` one-line masked textarea (placeholder "8-63 characters") + **"ðŸ’¾  Save Hotspot Password"** button (220x40) â†’ `on_ap_save`: validates length 8â€“63 (else red error); saves via `storage_->save_ap_password`; shows yellow "Saved. Applying â€” reconnect AP clientsâ€¦"; re-masks field; spawns `ap_apply_task` (prio 5) â†’ `wifi_->apply_ap_settings()` (again off-LVGL due to SDIO). `lbl_ap_msg_` status line.
6. **"Available networks:"** label + `list_wifi_scan_` (`lv_list`, 160px tall, `kBgGroup`). Initial item "Tap Scan to find networks".
   - `populate_wifi_scan_list`: each network â†’ button labeled `"<lock> <ssid>  (<rssi> dBm)"` where lock = `LV_SYMBOL_CLOSE` if secured else spaces; empty â†’ "No networks found".
   - **Tapping a network** â†’ `on_wifi_list_click`: parses the SSID back out of the button label (skips UTF-8 lock glyph + spaces, cuts at "  (") and writes it into `ta_wifi_ssid_`. (No password auto-fill.)

### 7b. NTRIP Config modal (`build_ntrip_modal` 786 / `open_ntrip_modal` 872 / `save_ntrip_config` 895)
Single **shared** modal repopulated per service via `ntrip_cfg_idx_` (0=rtk2go,1=onocoy,2=rtkdata). Title set dynamically to "RTK2go Config" / "Onocoy Config" / "RTKdata Config".

On open: loads `storage_->load_service(key)` (mountpoint+password) and `storage_->service_enabled(key)`; sets the enable switch; fills fields; hides keyboard.

Content:
1. **"Service enabled"** label + `sw_ntrip_en_` switch (inline row).
2. **"Mountpoint:"** label + `ta_ntrip_mp_` one-line textarea (placeholder "MOUNTPOINT").
3. **"Password:"** label + `ta_ntrip_pw_` one-line masked textarea (placeholder "Password").
4. Shared keyboard `kb_ntrip_` (all textareas wired focus/defocus).
5. **Button row:** **"Cancel"** (160x44) â†’ `on_ntrip_close` (hide modal+kb, no save); **"ðŸ’¾  Save"** (160x44) â†’ `on_ntrip_save` â†’ `save_ntrip_config`.

Save action: reads switch + both fields â†’ `storage_->set_service_enabled(key,en)` + `storage_->save_service(key,creds)` â†’ **`station_->reload_services()`** (applies live) â†’ hides modal + keyboard.

### 7c. SD Card file browser modal (`build_file_browser` 915 / `open_file_browser` 952 / `refresh_file_browser` 959)
Title "SD Card". Opens rooted at `SdManager::kMountPoint` ("/sdcard").

Layout:
- **Path bar:** `lbl_fb_path_` (current path, flex-grow) + **"â¬†  Up"** button (80x36) â†’ `on_fb_up` (goes to parent dir; no-op at mount root).
- **File list** `list_fb_` (`lv_list`), height = `kHeight - 52 - 48 - 2` (fills remaining space below title+path bars), `kBgGroup`, square corners.

`refresh_file_browser`: `opendir` â†’ skips dotfiles â†’ collects `(is_dir,name)` â†’ **sorts directories first, then alphabetical** â†’ for each entry:
- Dir â†’ label `"ðŸ“ <name>/"`.
- File â†’ `stat` for size â†’ label `"ðŸ“„ <name(<=40 wide)>  <human size>"`.
- Each button stores its index (`fb_entries_` vector) and fires `on_fb_item`.
- Empty dir â†’ "(empty)"; `opendir` failure â†’ "Cannot open directory".

`on_fb_item`: if entry is a directory â†’ navigate into it (`open_file_browser`). **Files: tap does nothing.** Comment at line 1175 explicitly notes "long-press could delete but for now tap does nothing for files" â€” i.e. **NO delete capability of any kind exists on the touchscreen today** (no single-file delete, no multi-select, no directory delete). This is the biggest functional gap vs. the required "bulk SD management on BOTH surfaces."

### 7d. Confirmation message boxes (`lv_msgbox`, not full modals)
Both created on `lv_layer_top()`, width 480, centered, with footer buttons tagged with the `Ui*` pointer:
- **Start New Survey confirm** (`on_survey_start` when mode=Transmit): title "Start New Survey?", text "This will reset the current fixed position and begin a new accuracy survey.", footer **"Yes" / "Cancel"**. `on_survey_confirm` â†’ only on "Yes" calls `station_->request_survey()`; always closes. (In SURVEY mode, no confirm â€” survey is re-requested immediately.)
- **C6 OTA confirm** (`on_c6_ota_btn`): title "Update C6 Firmware?", text "This flashes new firmware to the WiFi coprocessor. The device will restart when complete.", footer **"Update" / "Cancel"**. `on_c6_ota_confirm` â†’ on "Update": sets `c6_ota_progress_=0`, disables the OTA button, spawns `c6_ota_task` (prio 5, 8192 stack).
  - `c6_ota_task`: reads embedded `_binary_c6_slave_fw_bin_*`; `esp_hosted_slave_ota_begin` â†’ write in **1500-byte chunks** (updates progress %), â†’ `esp_hosted_slave_ota_end` â†’ `esp_hosted_slave_ota_activate` â†’ delay 3s â†’ **`esp_restart()`** (whole P4 reboots). Failures set progress=-2 ("Update failed", red).

---

## 8. REFRESH / UPDATE MECHANICS

- **One `lv_timer`** created in `init()` at **1000 ms** interval (`refresh_timer_cb` â†’ `refresh()`), running on the LVGL/BSP task (display lock already held by LVGL during timer callbacks).
- `refresh()` (1372) re-reads all sources every second and updates **every** value label across Status, NTRIP, Position, System tabs (regardless of which tab is visible). One stack `char buf[192]` reused throughout.
- Snapshots taken once per tick: `station_->status()` (`BaseStationStatus`, includes embedded `SurveySnapshot survey`), `station_->rinex_status()`, `storage_->load_position()`. Satellites copied into a stack `SatelliteInfo sats[32]`.
- Debug log rendered every **2nd** tick only.
- **Switch resync guards:** both `sw_ntrip_all_` and `sw_rinex_` are reconciled to backend state each tick using add/clear-state (which does not refire `VALUE_CHANGED`), so periodic refresh never fights the user or loops.

### Threading / locking (CRITICAL for redesign)
- All LVGL object mutation outside the timer callback MUST hold the BSP display lock: `bsp_display_lock(0)` / `bsp_display_unlock()`. `init()` does this around `build_screens` + timer creation.
- **Any operation that does SDIO RPC to the C6 coprocessor MUST run on a separate FreeRTOS task, never inline in an LVGL callback** (the LVGL task holds the display lock; blocking it on RPC caused the IWDT WiFi-RPC crash noted in memory). This applies to: WiFi scan (`wifi_scan_task`), WiFi connect (`wifi_connect_task`), AP apply (`ap_apply_task`), C6 OTA (`c6_ota_task`), and the running-C6-version query (`c6_version_task`, spawned at init prio 3). Worker tasks re-acquire `bsp_display_lock` before touching LVGL (see `wifi_scan_task`).
- Cross-thread state uses `std::atomic`: `c6_ota_progress_` (-1 idle / 0â€“100 / -2 fail), `c6_running_ready_`, `scan_running_`. The C6 version strings are filled by a worker and read in refresh once `c6_running_ready_` is set.

### Heap / performance notes
- Modals built lazily on first open (comment line 362) to keep init fast and reduce resident SRAM.
- Debug label capped at 4000 chars to keep the LVGL label light; full history offloaded to web `/logs`.
- Sat detail capped to top 16 of 32, 256-byte buffer.
- Each keyboard is ~40% of screen height; only two exist (wifi, ntrip), built on demand.

---

## 9. HARD CONSTRAINTS A REDESIGN MUST RESPECT
1. **LVGL 9.x API** (`lv_event_get_target_obj`, `lv_msgbox_add_footer_button`, `lv_tabview_add_tab`, `lv_obj_get_child(..., int32_t)`, etc.). Object-based, flex layout, style-per-part.
2. **BSP-owned LVGL task** â€” mutate objects only under `bsp_display_lock()/unlock()`; the 1s timer runs already-locked.
3. **No SDIO/RPC work on the LVGL task** â€” offload WiFi/C6 operations to worker tasks; marshal results back under the lock; pass cross-thread state via atomics. (Root cause of a prior watchdog crash.)
4. **Fixed 720x720, capacitive touch only** â€” no mouse, no hover, no right-click. Vertical touch-scroll within tabs is the only scroll. Touch targets are finger-sized today (buttons 36â€“44px tall, 40x40 close, 80px+ wide). Tab bar fixed 52px at bottom.
5. **C++17, no exceptions/RTTI**; app partition 6MB; tight internal SRAM (~95KB free) so avoid large fonts/bitmaps in internal RAM, lazy-build heavy UI, keep PSRAM in mind.
6. **Data dependencies are via injected managers** â€” `BaseStation`, `SdManager`, `WifiManager`, `Storage` (set in `Ui::init`). All values originate from `BaseStationStatus`/`SurveySnapshot`/`NtripStatus`/`RinexLogger::Status`/`SdManager::DiskStats`/`WifiManager`/`BasePosition` + ESP-IDF system calls. A redesign reuses these same getters.

---

## 10. KNOWN GAPS THE REDESIGN MUST CLOSE (observed in code)
- **No large numeric readouts** â€” everything is ~14px body text; the marine-instrument aesthetic requires new large fonts/gauges.
- **No file delete at all on touch** â€” `on_fb_item` ignores file taps; no single delete, no multi-select, no directory delete (the spec mandates finger-friendly multi-select delete + whole-directory delete with confirmations).
- **Terminology/IA is ad-hoc per tab** â€” group titles are SHOUTING-CAPS strings, tab names mix concerns (Status vs Position both show position+survey; NTRIP split across Status compact + NTRIP detail). A unified IA must reconcile these.
- **Antenna model/radome/height and sky plot** are NOT surfaced on the touchscreen today (only raw sat SNR list); these exist on web and are candidates for the unified structure.
- **Day/night palette** not implemented â€” single dark palette only.
