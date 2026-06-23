I'll synthesize the definitive unified design spec now. I have all three proposals, the three judge verdicts, the SD design, both UI inventories, the data model, and the marine design system research. Let me produce the authoritative spec.

# GPS RTK Base Station â€” Unified Design Specification (Authoritative)

*Marine-instrument redesign for both surfaces: 720Ã—720 LVGL touchscreen + responsive HTTPS web UI. Design pass only â€” no files modified. All paths absolute.*

---

## A. Design rationale â€” backbone, grafts, conflict resolution

### A.1 The winning backbone: P3 (Status-dashboard), with one decisive correction from the consistency judge

**P3 wins the spine.** The cross-surface-consistency lens â€” the lens that is explicitly weighted highest in the brief ("UNIFIED IAâ€¦ same section names, ordering, terminology") â€” ranked P3 first (9) precisely because its **five sections map 1:1 onto today's five touch tabs** (`ui.cpp:345-349`) using the operator's existing subsystem vocabulary (Position, Links/NTRIP, Storage, System). This is the single most important decision in the whole design, and the judge stated it bluntly: **adopt FIVE top-level sections, not six.** P1's six-stage lifecycle ribbon and P2's six-section "Helm" both squeeze the 720px touch tab bar to ~120px/tab and force operators to translate "is RTK2go up?" into a lifecycle stage. P3's five sections do not.

**Why not P1 (Task-flow):** It lost all three lenses (6/6/6). The lifecycle metaphor (Setupâ†’Surveyâ†’Broadcastâ†’Loggingâ†’Maint) is pedagogically elegant but (a) needs six tabs, (b) renames operator/device terms (NTRIPâ†’"Broadcast", RINEXâ†’"Logging") fighting the casters' own docs and the device's vocabulary, (c) splits cross-cutting state (mode, antenna) across stages forcing duplication, and (d) the feasibility judge flagged its overloaded Survey-in screen (three live arcs + canvas + sat lists on one tab) as the least-feasible redraw surface against the 720Ã—50 partial draw buffer and the ~95KB SRAM ceiling. We keep exactly **one** idea from P1 (see A.2).

**Why P3 over P2 (Live-instrument MFD):** P2 won feasibility (narrowly, 8 vs 8) and aesthetics (9 vs 8), and it is genuinely excellent â€” but it proposed **six** sections (Helm/Position/Links/Storage/System/Console), promoting the Debug/Console tail to a top-level tab. That reintroduces the six-tab problem the consistency judge told us to avoid. P3 folds the console into System (web gets the full viewer; touch keeps the tail), keeping five. P3 and P2 are otherwise nearly identical (both express "the touchscreen is the web app at its 720px 2-column breakpoint"), so adopting P3's spine costs us almost nothing of P2.

### A.2 What was grafted in (per the judges' "must keep")

The backbone is P3's five-section IA. Onto it we graft:

| Grafted element | From | Why (judge mandate) |
|---|---|---|
| **Lighter Dashboard with read-only deep-link tiles** | P2 | Feasibility judge: P3's Dashboard "packs seven rollups plus three live inline togglesâ€¦ raising per-tick redraw work and drift risk." We **demote the inline NTRIP/RINEX toggles off the Dashboard tiles** and make every Dashboard tile a strictly read-only deep-link (P2's single-render-path discipline). The one exception kept from P3: a single **Start/Restart Survey** action button on the Dashboard, because it routes through the same shared confirm + `request_survey()` and is the operator's most common deliberate action. |
| **Tile-ranking / above-the-fold discipline** | P2 | Aesthetics judge mustKeep #6: top-N watched tiles (RTCM, Mode, Sats, NTRIP) never scroll out of glance range. |
| **RTCM rate as the status-tinted heartbeat hero** | P2 | Aesthetics judge mustKeep #5. Exactly one hero per screen. |
| **Sky-plot isolated inside the Position drill-down (lazy-built, redrawn only when visible)** | P2/P3 | Feasibility judge mustKeep #4: never stack the `lv_canvas` with live arcs. |
| **Three-gate survey arc (â‰¥300s, â‰¥5 blocks, Ïƒâ‰¤0.50m)** replacing today's time-only bar | all three | Aesthetics judge mustKeep #7; data model confirms today's bar is time-only and understates completion. |
| **Antenna metadata read-back echo** | P1's honest call-out | P1 correctly flagged antenna data is *entered* in config but *consumed* by RINEX. We resolve: editable in **System** (commissioning), read-only echo in **Storage** (drives RINEX header) and **Position**. |
| **Global worst-of-all status pill + day/night header toggle, persisted in `Storage`** | all three | All three lenses mustKeep. Net-new â€” no theme system exists today. |
| **One shared `:root`/`[data-theme]` CSS token block in `kAfterTitle`** | all three | Feasibility mustKeep #7; web inventory confirms 3 divergent themes (`send_page`, `/files`, `/rinex/export`). |
| **The full bulk-SD-delete backend** (validated, off-task, recursive, protected, progress) | dedicated SD design | All three lenses mustKeep #7/#10; the single largest shared engineering item. Folded in Â§F verbatim. |

**Net result:** P3's five-section subsystem spine + P2's lighter/glanceable rendering discipline + P1's one honest cross-cutting resolution (antenna double-home) + the shared SD backend.

---

## B. Unified Information Architecture

### B.1 The five-section spine (identical names, icons, order, status semantics on BOTH surfaces)

| # | Section | Touch icon (LVGL symbol) | Web icon (inline SVG, `currentColor`) | One-line purpose |
|---|---------|--------------------------|----------------------------------------|------------------|
| 1 | **Dashboard** | `LV_SYMBOL_HOME` | gauge-cluster / home | At-a-glance instrument face: is the base healthy and transmitting? |
| 2 | **Position** | `LV_SYMBOL_GPS` | crosshair | Fixed coordinate, survey-in quality, satellites, sky plot, antenna. |
| 3 | **Links** | `LV_SYMBOL_WIFI` | cloud-up / broadcast | NTRIP push services + local caster + master enable. |
| 4 | **Storage** | `LV_SYMBOL_SD_CARD` | SD card / folder | SD usage, RINEX logging, file browser + bulk delete. |
| 5 | **System** | `LV_SYMBOL_SETTINGS` | gear | Health, network, firmware/C6, console, day/night, admin. |

Touch labels (terse, fit â‰¥120px tab at 5 tabs / 720px): **Dash Â· Position Â· Links Â· Storage Â· System**. Web uses full names. Bottom tab bar on touch; top segmented nav (collapses to drawer/bottom bar on narrow) on web â€” same five, same order, same icons.

Naming decision (resolving P1's "Broadcast/Logging" vs P3's "Links/Storage"): we keep **operator-subsystem terms** per the consistency judge. "Links" is the section name; "NTRIP" appears as sub-labels/tile titles inside it (RTK2go/Onocoy/RTKdata). "RINEX" stays as the logging label inside Storage. This matches the operator's mental model and the casters' own vocabulary, and minimizes cross-section duplication.

### B.2 Feature-placement mapping table (proves every current feature is placed)

Legend: **[W]** = web-only (acceptable divergence, rendered only on web); **[T]** = touch-only today (asymmetry, honestly placed in identical IA slot); **[D]** = also mirrored read-only as a Dashboard deep-link tile.

| Current feature | Source accessor | Section | Notes |
|---|---|---|---|
| Mode (SURVEY / BASE TX) | `status().mode` | Dashboard **[D]** + Position | Color-coded constant: SURVEY=warn amber, BASE TX=good green. |
| Healthy / global state | `BaseStation::healthy()` + worst-of-all | Dashboard (global pill) | Pill in header of every screen. |
| RTCM B/s + total | `status().rtcm_bytes_per_second/_total` | Dashboard (hero) **[D]** | Status-tinted heartbeat hero. |
| Stored lat / lon / height | `Storage::load_position()` | Position **[D]** | Cached in RAM (not NVS-per-tick). |
| Survey state/elapsed/samples/Ïƒ/blocks/inst-Ïƒ | `status().survey` | Position | Three-gate arc + detail readouts. |
| Satellites used/tracked + G/R/E/C | `status().survey.*` | Position **[D]** (count only on Dash) | |
| Per-sat SNR list | `satellites()` | Position | Top-16 SNR list (as today). |
| Sky plot | `satellites()` (el/az/snr/sys) | Position | **Net-new on touch** (`lv_canvas`); existing on web. Lazy-built, visible-only redraw. |
| Antenna model/radome/height | `Storage::antenna_*()` | **System** (edit) â†’ echo in Position + Storage | Resolves the cross-stage double-home (graft from P1). |
| NTRIP master enable | `set_streams_enabled()` | Links (canonical) | Persisted. **Removed from Dashboard tiles** (P2 graft â€” read-only Dashboard). |
| Per-service status/bytes/dropped/reconnects/last-error | `status().rtk2go/.onocoy/.rtkdata` | Links **[D]** (rollup "n/3 LIVE" on Dash) | Compact list-row variant inside Links. |
| Per-service config (mountpoint/pw/enable) | `Storage::save_service` + `reload_services()` | Links | Modal (touch) / form (web). |
| Local caster :2101 clients + IPs | `status().local_clients/_client_ips` | Links | |
| SD mount + disk usage | `SdManager::disk_stats()` | Storage **[D]** (disk arc on Dash) | |
| RINEX logging toggle + file/epochs/files | `rinex_status()` + `request_raw_collection()` | Storage | Toggle lives here (canonical); disabled+hint "Requires Base TX" when in survey. **[D]** as read-only status dot only. |
| File browser (list/navigate) | `SdManager::list_dir()` | Storage | |
| Bulk multi-select delete + directory delete | **net-new** `SdManager` (Â§F) | Storage | Both surfaces. |
| File **download** | `/files/download` | Storage **[W]** | |
| Bulk **RINEX export/merge** | `/rinex/export` | Storage **[W]** | |
| Single-file rename / mkdir | `rename_entry` / mkdir | Storage | Kept as-is. |
| Uptime / reset reason | `esp_timer` / `esp_reset_reason()` | System | Reset reason red if crash. |
| Free heap (+ min, total) | `heap_caps_*` | System | Heap arc. |
| WiFi station (SSID/RSSI/IP) | `WifiManager` cached getters | System | RSSI arc. |
| Hotspot (AP SSID/IP) + AP password | `WifiManager` + `Storage::ap_password` | System | |
| WiFi scan / connect | `scan_networks()` / `save_wifi()` | System | Off-task worker (touch). |
| Firmware version + build | `esp_app_get_description()` | System | |
| C6 running vs available version | `esp_hosted_get_coprocessor_fwversion` / embedded | System | |
| **C6 OTA** | `esp_hosted_slave_ota_*` | System **[T]** | Touch-only today; same IA slot on both. See B.3. |
| **P4 firmware OTA upload** | `/update` | System **[W]** | Web-only today; same IA slot. See B.3. |
| Full console log viewer | `/logs` + `/logs/data` | System **[W]** | Keeps monospace. |
| Console tail | `log_buffer::snapshot()` | System | Touch-degraded equivalent (replaces standalone Debug tab). |
| CA cert download | `/ca.crt` | System **[W]** | |
| First-run admin password gate | `/setup` | System **[W]** | Web provisioning mechanic. |
| Admin password change | `Storage::save_admin_password` | System | |
| Day/night toggle | net-new, persisted in `Storage` | header (canonical) + System (persistence setting) | |

**Coverage check: every feature from both inventories is placed in exactly one owning section.** Nothing dropped. Dashboard tiles are read-only mirrors `[D]`, never independent owners.

### B.3 The two real asymmetries â€” placed honestly in identical IA slots

The consistency judge (mustKeep #4) demands these be named, never hidden:

- **C6 OTA** is touch-only today; **P4 `/update` OTA** is web-only today. **Both occupy the identical "Firmware" group inside System on both surfaces.** This design does *not* force parity (adding a web C6-OTA route or a touch P4-OTA flow is out of scope), but the IA slot is identical, so if parity is later added it drops in with zero IA change. System is the one section whose *action set* legitimately differs per surface beyond the sanctioned web-only list â€” and that difference is documented, not accidental.
- **Web-only capabilities** (file download, RINEX export, P4 OTA upload, full console viewer, CA cert, first-run `/setup`) are each rendered inline in their owning section (Storage/System) and simply have no touch counterpart. They are labeled web-only in this spec and never appear as broken/empty affordances on touch.

---

## C. Visual design system

### C.1 Final color token table (day + night, identical token names)

Night is the default. LVGL receives a `Theme` struct of `lv_color_t` + font pointers selected by day/night and applied via a shared `apply_theme()`; web emits the block below once in `kAfterTitle`.

**Day palette (sunlight / bright ambient):**

| Token | Hex | Role |
|---|---|---|
| `--bg` | `#0B1016` | Screen background |
| `--surface` | `#16202B` | Tile / card surface |
| `--surface-hi` | `#1E2C3A` | Raised / selected surface |
| `--border` | `#2C4053` | Hairlines, dividers |
| `--text` | `#F2F6FA` | Primary numeric / text (15.6:1) |
| `--text-dim` | `#9DB0C2` | Labels, units (6.2:1) |
| `--text-faint` | `#5E7488` | Disabled, captions |
| `--accent` | `#2FA4FF` | Interactive / links / focus |
| `--good` | `#27D17C` | OK / connected / transmitting / fix |
| `--warn` | `#FFB02E` | Surveying / reconnecting / disk>80% / weak RSSI |
| `--crit` | `#FF5A52` | Disconnected / SD error / disk>95% / survey failed |

**Night palette (dark-adapted):**

| Token | Hex |
|---|---|
| `--bg` | `#05080B` |
| `--surface` | `#0D141B` |
| `--surface-hi` | `#142029` |
| `--border` | `#203040` |
| `--text` | `#C9D6E0` |
| `--text-dim` | `#6E8294` |
| `--text-faint` | `#3F5364` |
| `--accent` | `#1C7FCC` |
| `--good` | `#1FAE66` |
| `--warn` | `#D98A1F` |
| `--crit` | `#E0473F` |

**Status semantics (identical, redundantly encoded â€” color + word + glyph, mandatory for sunlight + color-blindness):**
- **good (green):** base TX, NTRIP connected, SD mounted, RINEX healthy, RTK fix.
- **warn (amber):** survey-in, reconnecting, disk >80%, weak RSSI, degraded sat count.
- **crit (red):** disconnected/dropped, SD unmounted/error, disk >95%, survey failed, crash reset.
- **dim (`--text-faint`):** off / idle / disabled service / N/A â€” **never red.** "Off" â‰  "broken."

### C.2 Typography

**Touch (LVGL):** enable in `lv_conf.h` â€” `MONTSERRAT_14` (already on), **`18`, `28`, `48`** (net-new). These cost flash `.rodata`, NOT scarce internal SRAM (feasibility mustKeep #1), so they are affordable and they close the no-large-numeric gap.

**Web:** `--font: "Segoe UI", system-ui, -apple-system, "Roboto", "Helvetica Neue", Arial, sans-serif;` with `font-variant-numeric: tabular-nums` on live-changing values so they don't jitter. Monospace retained **only** in the `/logs` console.

**Shared type ramp:**

| Token | Touch | Web | Weight | Use |
|---|---|---|---|---|
| `hero` | 48px | `clamp(40px,9vw,64px)` | 700 | The one dominant value per screen (RTCM rate / survey Ïƒ) |
| `value` | 28px | `clamp(24px,5vw,36px)` | 600 | Standard tile value |
| `value-sm` | 18px | 18â€“20px | 600 | Dense tiles, sat-by-constellation |
| `label` | 14px | 13px | 600 **uppercase +0.06em** | Tile label |
| `unit` | 14px | 14px | 500 dim | Unit suffix |
| `body` | 14px | 14â€“15px | 400 | List rows |
| `caption` | 14px | 12px | 500 dim | Timestamps, hints |

Rule: labels uppercase+dimmed+tracked, values large+bright+tight. Exactly one `hero` per screen.

### C.3 Sizing & spacing scale

8px base unit. Gaps 16px (touch) / 12â€“16px (web). Tile radius 12px, pills full-radius, buttons 8px. Touch targets â‰¥64px; web â‰¥44px. Header 64px (touch) / 56px (web). Bottom tab bar 80px (touch, replaces today's 52px â€” bigger finger target, same bottom edge).

### C.4 Core component set

1. **Readout tile (the atom):** `--surface` bg, 12px radius, 1px `--border`. Label (uppercase/dim/tracked) top, large `value` + dim `unit` center, optional dim caption. **The value carries status color, not the whole tile** (B&G pattern). `crit` adds a 2px `--crit` left-edge bar (third redundant cue). Tappable tiles show a faint `>` chevron + `--accent` ripple/hover. Touch min-height 150px / web 110px, pad 16px.
2. **Status pill:** color-at-12%-opacity bg + solid-color text + 8px dot + word. `â— LIVE` / `â— SURVEYING` / `â— OFFLINE` / `â—‹ OFF` (off = outline only, dim). Touch 32px / web 22px, capsule radius. Used in tile corners, list rows, header.
3. **Header (persistent):** section title left Â· **global status pill** (worst-of-all-subsystems = the marine shared-alarm strip; green LIVE / amber SURVEYING / red ALARM) Â· day/night toggle + (web) hamburger right.
4. **Nav bar:** touch = bottom tab bar, 5 icon+label tabs, 80px, active = `--accent` icon + 3px top accent rule (reuses existing `lv_tabview` `LV_DIR_BOTTOM`). Web = top segmented nav, same 5, collapses to drawer/bottom bar on narrow.
5. **List row:** 64px touch / 48px web. Leading icon Â· primary + dim sub-text Â· trailing pill/value/chevron. **Multi-select variant** (bulk SD): leading 36â€“44px checkbox, selected rows get `--surface-hi` + `--accent` check, slide-up action bar `Delete (N)` in `--crit`.
6. **Gauge/arc:** 270Â°, 12px stroke, `--border` track + status-colored fill, centered value. Bounded 0â€“100 quantities: survey stability, disk %, RSSI, free heap. LVGL `lv_arc`; web inline `<svg>` `stroke-dasharray` themed via `currentColor` (no canvas/libs). **No per-tick `LV_ANIM`; refresh at 1Hz only** (feasibility mustKeep #5).
7. **Progress bar:** 10px touch / 8px web track, full-radius, status-colored fill. Survey-in, OTA, delete progress. Retheme existing OTA bar markup (track `--surface-hi`, fill `--good`â†’`--crit`).
8. **Confirm dialog:** centered modal over 60% scrim. Title in `--text`, body names exactly what's affected + count + total size + "This cannot be undone." Buttons: **Cancel** (neutral `--surface-hi`, default focus) and destructive (`--crit` filled, **never** default focus). Touch buttons â‰¥64px.

---

## D. Touchscreen layout (720Ã—720 LVGL)

Shell: 64px header (title + global pill + day/night toggle) Â· content area Â· 80px bottom tab bar (5 tabs). All object mutation under `bsp_display_lock()`. Single 1000ms `lv_timer` re-reads the `status()` snapshot and updates all value labels/arcs (existing pattern). Position cached in RAM (feasibility mustKeep #6). Switch resync guards on RINEX toggle so refresh never fights the user.

### D.1 Dashboard (Section 1) â€” the instrument face

Under header, above tab bar (~576px usable), a **2-column tile grid**, touch-scroll for overflow. Tile ranking so the top 4 (RTCM, Mode, Sats, NTRIP) never scroll out of glance range:

- **Row 1 â€” Hero tile (spans 2 cols): RTCM OUTPUT** â€” `1247` in 48px `hero`, `B/s` unit, status-tinted (green transmitting / dim idle), sub-line "total 4.2 MB". In SURVEY mode this hero **swaps to survey stability** `Â±0.18 m` with a 270Â° progress arc (the number that matters in that stage).
- **Row 2 â€” Mode tile** (`BASE TX` green / `SURVEY` amber) Â· **Satellites tile** (`24` used, sub `G9 R6 E6 C3`).
- **Row 3 â€” NTRIP push tile** (`2/3 LIVE` pill + `3 clients` rollup) Â· **Position tile** (compact lat/lon, FIX pill).
- **Row 4 â€” SD/RINEX tile** (disk arc + RINEX dot: logging green / off dim) Â· **Link tile** (SSID + RSSI, or "AP: GPS-BaseStation").
- **Pinned action:** full-width **Start/Restart Survey** button (amber, `--crit`-gated confirm when leaving Base TX), routed through the shared `request_survey()` + confirm msgbox. (Per P2 graft, the NTRIP/RINEX toggles are NOT on the Dashboard â€” they live canonically in Links/Storage; Dashboard tiles are read-only deep-links into their owning section.)

Every non-action tile taps â†’ its owning section.

### D.2 Position (Section 2)

Vertical touch-scroll tab (`style_tab` pattern). Groups via `make_group`:
- **FIXED BASE POSITION** â€” lat/lon as `value` (28px), height. Big readouts replace today's 14px.
- **SURVEY-IN** â€” three 270Â° arcs (time / blocks / stability gates) replacing today's single time-only bar; sub-readouts elapsed/samples/inst-Ïƒ. Hidden/dim when in Base TX.
- **SATELLITES** â€” used/tracked + G/R/E/C as `value-sm` tiles; top-16 SNR list below (as today).
- **SKY PLOT** â€” `lv_canvas` (or drawn points), **lazy-built, redrawn only when this tab is visible** (feasibility mustKeep #4 â€” never stack with the arcs' redraw). Net-new on touch.
- **ANTENNA (read-only echo)** â€” model/radome/height; "Edit in System" chevron.

### D.3 Links (Section 3)

- **ALL NTRIP SERVICES** â€” master enable switch (canonical home; `set_streams_enabled`, persisted; resync-guarded).
- **Three service cards** (RTK2go/Onocoy/RTKdata) via the `ntrip_detail_group` pattern: status pill + bytes/dropped/reconnects rows + "âš™ Config" button â†’ shared NTRIP config modal (`open_ntrip_modal`, repopulated per index).
- **LOCAL CASTER :2101** â€” clients + client IPs.

### D.4 Storage (Section 4)

- **SD CARD** â€” disk-usage arc + used/total.
- **RINEX COLLECTION** â€” toggle (canonical; `request_raw_collection`; disabled+hint "Requires Base TX" in survey) + current file/epochs + antenna echo.
- **FILE BROWSER** â€” the existing `build_file_browser` modal extended with bulk multi-select delete + directory delete (Â§F).

### D.5 System (Section 5)

- **SYSTEM** â€” uptime, reset reason (red if crash), free heap arc.
- **NETWORK** â€” station (SSID/RSSI arc/IP) + hotspot + "Configure WiFi" (modal, off-task workers) + AP password.
- **FIRMWARE** â€” FW version/build, C6 running/available, **"Update C6 Firmware"** button + confirm (touch-only `[T]`, `c6_ota_task`).
- **CONSOLE** â€” log tail (`log_buffer::snapshot()`, last 4000 chars, green, every-2nd-tick), replacing today's standalone Debug tab. (Full viewer is web-only.)
- **DISPLAY** â€” day/night toggle (duplicate of header; header is canonical), admin password.
- **ANTENNA (edit)** â€” model/radome/height form (canonical entry).

### D.6 Nav pattern + modals

Bottom `lv_tabview` (`LV_DIR_BOTTOM`), 5 tabs, 80px, â‰¥120px/tab â€” a **smaller change to the existing tabview than P1/P2's six tabs** (feasibility judge's narrow nav point for P3). Modals stay full-screen on `lv_layer_top()`, lazily built, dismissed-not-destroyed (reuse `make_modal_base` + `kb_*` keyboard pattern). All SDIO/RPC work (WiFi scan/connect, AP apply, C6 OTA/version, recursive SD delete) runs on worker tasks; LVGL mutation only under the lock; cross-thread state via `std::atomic` polled by the 1s refresh (the proven `c6_ota_progress_` pattern â€” feasibility mustKeep #2).

---

## E. Web layout (responsive HTML)

### E.1 Shared CSS theme approach

Emit **one** `:root` + `[data-theme="night"]` CSS-variable block (the C.1 tokens) exactly once inside the existing shared `<style>` in `send_page` (`kAfterTitle`). Retire the three divergent inline themes: `/files` and `/rinex/export` adopt the tokens (export keeps its richer controls but in-theme); `/logs` keeps monospace only for the `<pre>`. All colors become `var(--token)`; day/night toggles `data-theme` on `<html>`, persisted in `Storage` so both surfaces agree. One inline-SVG icon set colored by `currentColor` so status color flows through. No client libraries; vanilla `fetch`/XHR only.

### E.2 Shared nav (net-new â€” web has none today)

Render a top segmented nav into `kAfterTitle` so **every page** shares it: the five sections in order with identical icons/labels, the global status pill, and the day/night toggle. Collapses to a drawer/bottom bar on narrow widths so phone-web mirrors the touch layout. Retire today's ad-hoc footer `<a>` links (keep the raw `/status` JSON link in a footer for power users).

### E.3 Per-section structure

- **Dashboard (`/`)** â€” tile grid `repeat(auto-fit, minmax(220px,1fr))`: 1 col phone â†’ **2 col at ~720px (visually identical to the touch face â€” the unification trick)** â†’ 4 col desktop. Same hero RTCM tile (spans 2 at wide), same pills, same global pill in nav. Tiles are `<a>`-wrapped read-only deep-links. Start-Survey button in its own row. Polls `/status` at 15s, rebuilding tiles (not table rows). First-run still gates to `/setup`.
- **Position** â€” lat/lon/alt hero readouts; three-gate survey arcs (inline SVG); sat-by-constellation; sky-plot `<canvas>` (richer on web, polls `/skyplot/data` 10s); antenna form echo.
- **Links** â€” per-service tiles/list-rows (status/bytes/drops/reconnects/last-error) + Config forms (`/config`); master enable; local caster + IPs.
- **Storage** â€” disk arc; RINEX toggle + file/epochs; file browser (`/files`) with bulk multi-select + directory delete (Â§F); **web-only** download + RINEX export (`/rinex/export`).
- **System** â€” health/uptime/reset/heap; WiFi station+AP + scan/connect; firmware + C6; **web-only** P4 OTA upload (`/update`), full console viewer (`/logs`, monospace), CA cert (`/ca.crt`); admin password; day/night persistence.

### E.4 Transport discipline (preserved as-is)

Build pages as `std::string`, emit via 1024-byte `httpd_resp_send_chunk`; binary downloads 4KB-chunked + `vTaskDelay(1)` paced (~400KB/s). `Cache-Control: no-store` on data endpoints. Poll cadences 15s/10s/5s. New handlers register in `register_secure_handlers()` against the hard URI-handler cap: **29 of 32 used, ~3 free; each GET+POST pair burns 2.** Budget Â§F's `/files/preview` + `/files/delete-batch` + `/files/delete-status` against those 3 (preview+batch are POST, status is GET = 3 slots â€” exactly fits; raise `max_uri_handlers` deliberately if C6-OTA-web-parity or day/night-persist routes are later added).

---

## F. Bulk SD management (folded in, consistent with the rest)

One backend capability â€” **validated, off-task, recursive, protected, progress-reporting bulk delete** â€” exposed identically through both renderers. This is the single largest net-new engineering item and is shared-critical (all three lenses mustKeep). Today `delete_entry` only `rmdir`/`unlink`s (fails on non-empty dirs) and `safe_path` is validator-only â€” recursive delete is genuinely net-new.

### F.1 Backend (`SdManager`, in `sd_manager.hpp/.cpp`)

New POD types (no exceptions/RTTI): `DeleteResult{requested,deleted,failed,skipped,first_error[96]}`, `DeletePreview{ok,files,dirs,bytes,truncated}`, `enum class DeleteGuard{kAllowed,kBadPath,kMountRoot,kManagedDir,kNotMounted}`. New methods: `check_deletable(path)` (static validator re-running `safe_path` + deny-list), `preview_delete`/`preview_delete_many` (bounded by `kPreviewWalkCap=4096`), `delete_recursive(path, atomic* progress)`, `delete_paths(paths, n, atomic* progress)` (**atomic pre-validation** â€” any bad/protected path rejects the whole batch, nothing touched; then best-effort continue-on-error). Re-point existing `delete_entry` through `check_deletable`. Constant `kInlineDeleteMax=16` (small ops inline, larger spawn the worker).

### F.2 Protected-path guard (deny-list, enforced inside `SdManager` so neither surface can bypass)

| Path | Rule |
|---|---|
| `/sdcard` (mount root) | Never deletable. No "delete everything" shortcut exists (intentional friction). |
| `/sdcard/logs`, `/sdcard/rawdata` (the dir shells) | Never removable; **contents are**. "Delete folder" on these = **empty the dir, keep the shell** ("Empty rawdata/ â€” 214 files, 1.7 GB", note "The folder itself is kept"). Recreated by `ensure_dirs()`. |
| anything failing `safe_path()` | `kBadPath` (empty / not under `/sdcard` / contains `..`). |
| user-created subdirs (e.g. `/sdcard/myfolder`) | Fully deletable, shell included. |

### F.3 Active-RINEX-file guard (caller layer â€” `SdManager` has no `BaseStation` visibility)

Before any delete, caller fetches `rinex_status()`; if `active`, reject any batch/folder containing `current_file` with the exact message **"Cannot delete â€” RINEX logging is writing this file. Stop logging in System first."** Pre-flight + re-checked per-path by the worker (logging could start mid-delete â†’ that path is `skipped`, not `failed`).

### F.4 Off-task execution + progress (mirrors the `c6_ota_progress_` pattern)

One `sd_delete_task` (prio 4, ~4096 stack) owns the active job: `std::atomic<int> sd_delete_state_` (-1 idle / 0 running / 1 done-ok / 2 done-with-failures), `sd_delete_progress_`, `sd_delete_total_`. Single-job state machine (second start refused: web 409, touch button disabled). `vTaskDelay(1)` pacing so it can't starve the contended SDSPI/server. Never recurse the FS on the LVGL or httpd task.

### F.5 Web UX (`/files`)

Normal mode adds a **"Select"** button. Selection mode: leading 44px checkboxes (dirs selectable too), "Select all" tri-state, live **"N selected Â· 1.7 GB"** counter (debounced `/files/preview`), **Delete (N)** (`--crit`, disabled at 0), Cancel. Each dir row also has **"Delete folderâ€¦"**. Confirm dialog (themed modal): names count + size + "This cannot be undone."; **non-empty-dir extra friction = type "DELETE"**; managed dirs read "Empty folderâ€¦"; active-file in scope â†’ blocking notice with link, not a confirm. New endpoints (all `authorize()` + `no-store`): `POST /files/preview`, `POST /files/delete-batch` (smallâ†’sync JSON, largeâ†’`{async:true,total}`), `GET /files/delete-status` (1s poll). Needs a minimal JSON-array extractor (current `json_field` is flat-string only; cap 256 elements, 256 chars each). Existing single `/files/delete` kept (now guarded). After delete: re-`loadDir` + refetch the storage bar.

### F.6 Touch UX (extend `build_file_browser`/`refresh_file_browser`/`on_fb_item`)

Two entry points: **long-press a row** (`LV_EVENT_LONG_PRESSED`) or a **"Select"** button. Selection mode: leading checkbox (â‰¥44px in the 64px row), tap toggles, selected = `--surface-hi` + `--accent` check. Slide-up 64px action bar: **"N selected Â· 1.7 GB"** + "Select all" + **Delete (N)** (`--crit`, â‰¥64px) + Cancel. Dir rows expose trailing-trash **"Delete folderâ€¦"**. Confirm via `lv_msgbox` on `lv_layer_top()` (survey/OTA pattern): same wording; **non-empty-dir friction = an "I understand this is permanent" toggle** (finger-appropriate vs web's typed "DELETE" â€” *same intent, surface-appropriate input*); active-file â†’ info msgbox with no destructive option. On confirm: set total from preview, spawn `sd_delete_task`, msgbox becomes a progress view ("Deleting 137/214â€¦") driven by the 1s refresh reading atomics. Selection state is a `std::vector<bool>` keyed to `fb_entries_`, mutated only under `bsp_display_lock()`; the periodic refresh does NOT rebuild the list while in selection mode (so selections aren't dropped). Active RINEX file is non-selectable.

### F.7 Consistency (identical across surfaces)

Shared string constants (verbatim): **Select** Â· **N selected Â· {size}** Â· **Delete (N)** Â· **Delete folderâ€¦** Â· **Empty folderâ€¦** Â· **Delete N items?** Â· **Delete folder "{name}"?** Â· **Empty folder "{name}"?** Â· body **"{n} files Â· {size} â€” This cannot be undone."** / **"{f} files Â· {d} subfolders Â· {size} â€” Everything inside is permanently erased. This cannot be undone."** Â· **The folder itself is kept.** Â· active-block message above Â· **Deleted {n}, {m} failed.** Â· **Deleting {done} / {total}â€¦**. Same byte humanizer (`fmt_bytes_str` touch / `human_bytes` web â†’ GB/MB/KB/B, 1 decimal GB/MB). Same icon semantics (folder/file/trash/check/warning). Same safety: same protected paths (enforced in one place), same active-file refusal, confirm always shows count+size+"cannot be undone", destructive button `--crit` and never default-focused, same continue-on-error + partial-failure summary.

### F.8 Edge cases (summary)

Active-file â†’ refuse with "Stop logging in System". Large dir â†’ worker + 1s progress poll, preview capped at 4096 (shows "â‰¥4096 files"). Read errors â†’ continue-on-error, "Deleted N, M failed" in `--warn`, dir re-listed. SD unmounted â†’ 503/disabled. Path-traversal/crafted batch â†’ atomic validation rejects whole batch. Concurrent delete â†’ single-job 409/disabled. Post-op â†’ re-read `disk_stats()` + re-list (position NOT re-read â€” avoid NVS churn).

---

## G. What changes vs today â€” before/after per surface

### G.1 Touchscreen

| Aspect | Today | After |
|---|---|---|
| Tabs | 5: Status / NTRIP / Position / System / Debug (52px bottom bar) | 5: **Dashboard / Position / Links / Storage / System** (80px bottom bar, icon+label, accent active rule). Debug folded into System console tail. |
| Section vocabulary | Ad-hoc SHOUTING-CAPS group titles; position+survey split across Status & Position; NTRIP split Status-compact + NTRIP-detail | One subsystem spine matching web exactly; survey consolidated in Position; NTRIP consolidated in Links. |
| Numerics | Everything ~14px (`montserrat_14` only) | Hero 48 / value 28 / value-sm 18 fonts (net-new `montserrat_18/28/48`, flash-only cost); RTCM rate as status-tinted hero. |
| Theme | Single dark navy palette (`kBgScreen 0x0d1b2a` etc.) | Two-palette token contract via `Theme` struct + `apply_theme()`; day/night header toggle, persisted. |
| Survey progress | Single time-only bar (0â€“300s) | Three 270Â° gate arcs (time/blocks/Ïƒ) â€” accurate completion. |
| Sky plot | Absent (raw SNR list only) | Net-new `lv_canvas` in Position, lazy-built, visible-only redraw. |
| Antenna | Not surfaced | Editable in System; read-only echo in Position + Storage. |
| Status pill | None | Global worst-of-all pill in header on every screen. |
| SD files | Navigate-only, **no delete at all** | Bulk multi-select delete + directory delete (long-press/Select, checkboxes, confirm with "I understand" toggle, off-task progress). |

### G.2 Web

| Aspect | Today | After |
|---|---|---|
| Nav | None â€” ad-hoc footer `<a>` links per page | Shared top segmented nav (5 sections, icons, global pill, day/night), collapses to drawer/bottom bar; mirrors touch. |
| Theme | 3 divergent inline themes (terminal `#0f0`-on-`#111`; `/files` blue/amber; `/rinex/export` blue), 42 scattered color occurrences | One `:root`/`[data-theme]` token block in `kAfterTitle`; marine palette; monospace only in `/logs`. |
| Pages | Separate ad-hoc pages (`/`, setup, config, skyplot, update, logs, files, rinex/export) | Same routes, reorganized under the 5-section spine; `/` becomes a tile-grid Dashboard reflowing 1â†’2(â‰ˆ720px=touch)â†’4 col. |
| Numerics | Monospace label/value tables | System-sans tiles, `tabular-nums`, status-tinted hero RTCM. |
| Survey | Time-only bar | Three-gate inline-SVG arcs. |
| Status summary | Per-row classes only | Global worst-of-all pill in nav. |
| SD files | Single-item delete/rename only | Bulk multi-select + directory delete (checkboxes, Select-all, typed-"DELETE" friction, async progress) via 3 new endpoints; web-only download + RINEX export retained in Storage. |
| Day/night | None | Header toggle, persisted in `Storage`, agreeing with touch. |

### G.3 Shared backend (both surfaces)

Net-new and shared-critical: **recursive/bulk SD delete in `SdManager`** with protected-path guard (mount root + managed dir shells), caller-layer active-RINEX-file guard, atomic batch pre-validation, off-task `sd_delete_task` with atomic progress, continue-on-error partial-failure reporting, and the `/files/preview` + `/files/delete-batch` + `/files/delete-status` endpoints. This is the one genuine engineering dependency the design carries; everything else is a re-theme/re-section over the existing `BaseStation::status()` read-model that both surfaces already consume identically.

---

**Implementer anchors (read-only, unchanged):** `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\ui.cpp`, `ui.hpp`, `web_server.cpp`, `web_server.hpp`, `sd_manager.hpp`, `sd_manager.cpp`, `base_station.hpp`, `storage.hpp`, `survey.hpp`, `rinex_logger.hpp`, `wifi_manager.hpp`, `app_config.hpp`. Touch palette to replace: `ui.cpp:51-57` (only `montserrat_14`). Web theme to replace: `web_server.cpp:~1317-1326` (`kAfterTitle`, repeated per page). SD backend additions: `sd_manager.cpp:230` (single-entry `delete_entry`). Active-file guard data: `base_station.hpp:61`, `rinex_logger.hpp:20`.
