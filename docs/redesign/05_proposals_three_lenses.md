--- P1 (Task-flow) ---
PROPOSAL P1 â€” Task-flow

## 1. Organizing principle

A GPS RTK base station is not a thing you monitor continuously â€” it is a thing you *commission once, leave running, and occasionally service*. The operator's mental model is therefore a lifecycle, not a feature inventory: you **Set up** the box (WiFi, admin, hotspot, antenna metadata), you run a **Survey-in** to pin down where the antenna actually is, you switch to **Broadcast** the corrections out to NTRIP casters and local clients, you optionally **Log** raw observations for post-processing, and forever after you do occasional **Maintenance** (firmware, storage cleanup, diagnostics). This IA makes those five life-stages the top-level sections, ordered exactly as the operator progresses through them. The payoff is that the navigation *teaches* the workflow â€” a first-time user reads the tab bar left-to-right as a checklist, and an experienced user jumps straight to the stage they came to touch. Every screen answers "what am I trying to accomplish right now?" rather than "which subsystem do I want to inspect?" A persistent global health pill sits above all five stages so the box's overall status is always glanceable regardless of which stage you're in â€” the marine "shared alarm strip."

## 2. Unified section map

The spine is **five lifecycle stages** plus a persistent **Dashboard** home that is not a stage but the always-on instrument face. Same names, order, icons, and status semantics on both surfaces.

| # | Section | Icon idea | Contains | Current features mapped in | Web-only sub-features |
|---|---------|-----------|----------|----------------------------|----------------------|
| 0 | **Dashboard** (Home) | Crosshair-in-ring (instrument bezel) | The instrument face: global health pill, current lifecycle stage indicator, hero RTCM rate, mode (SURVEY / BASE TX), sat count, NTRIP push summary, stored position. One-tap into whichever stage is "active now." | Touch Status tab (BASE OPERATION, compact NTRIP CASTERS, POSITION summary); Web `/` dashboard table | â€” |
| 1 | **Setup** | Wrench-over-gear (commissioning) | One-time/occasional commissioning: WiFi station creds + scan, hotspot (AP) password, admin password, antenna model/radome/ARP height. "Where am I broadcasting from / how do I reach the box." | Touch: WiFi modal (Â§7a), AP password, NETWORK group (station/AP read-back); Web: `/setup`, `/config/wifi`, `/config/ap`, `/config/antenna`, `/wifi/scan`, WiFi section of `/config` | First-run password gate (`/setup`); CA cert download (`/ca.crt`) surfaced here |
| 2 | **Survey-in** | Surveyor's tripod / converging crosshairs | Establishing the fixed coordinate: live lat/lon/alt estimate, the three completion gates (â‰¥300 s, â‰¥5 blocks, Ïƒ â‰¤ 0.50 m) as arcs, sat-by-constellation, sky plot, Start/Restart Survey, **manual position entry** (skip survey if coords known). | Touch: Position tab (FIXED BASE POSITION, SURVEY QUALITY, SATELLITES, sat-SNR), survey progress block + bar, Survey button + confirm; Web: survey rows on `/`, `/survey`, `/config/position`, `/skyplot`, `/skyplot/data` | Sky plot canvas is richer on web but present on both |
| 3 | **Broadcast** (was NTRIP) | Cloud-up over antenna (transmit) | Pushing corrections live: master enable, per-service tiles (RTK2go / Onocoy / RTKdata) with status/bytes/bitrate/drops/reconnects + config, local caster :2101 + connected client IPs, RTCM throughput hero. | Touch: NTRIP tab (ALL NTRIP SERVICES, 3 detail cards + Config, LOCAL CASTER), Status RTCM/NTRIP rows; Web: NTRIP rows on `/`, `/ntrip/toggle`, service section of `/config` | â€” |
| 4 | **Logging** | Cassette / waveform-to-disk | Recording raw obs for post-processing: RINEX logging toggle + state (file, epochs, files-closed), SD mount + disk-usage arc, current-file detail, antenna metadata read-back (drives header). | Touch: System STORAGE group (SD card, RINEX switch, current file); Web: RINEX rows on `/`, `/rinex/toggle` | Bulk **RINEX export / merge** (`/rinex/export`) lives here |
| 5 | **Maintenance** | Shield-check / toolbox | Servicing the box: uptime, reset reason, free heap, firmware version + build, C6 coprocessor version + OTA, day/night toggle, **SD file browser with bulk multi-select + directory delete**, and diagnostics. | Touch: System tab (SYSTEM, FIRMWARE groups, C6 OTA button + confirm), Debug tab tail, file browser modal (Â§7c); Web: System rows on `/`, `/update`, `/logs`, `/files`, `/files/*` | File **download**, P4 firmware **OTA upload** (`/update`), full **console log** viewer (`/logs`, monospace), CA cert if not in Setup |

**Coverage check â€” every current capability is placed:**
- Survey start/restart + confirm â†’ Survey-in. Manual position â†’ Survey-in. Auto surveyâ†’TX transition surfaces as a stage change on Dashboard + Survey-in.
- All 3 NTRIP push services + config + master toggle + local caster + client IPs â†’ Broadcast.
- RINEX toggle/state + SD disk usage + antenna read-back â†’ Logging; RINEX export (web) â†’ Logging.
- WiFi connect/scan, AP password, admin password, antenna config â†’ Setup.
- Uptime, reset, heap, firmware, C6 version + OTA, day/night, file browser (bulk delete), debug log â†’ Maintenance; web OTA upload + console + download â†’ Maintenance.
- Sky plot + sat SNR + constellation breakdown â†’ Survey-in (and sat count summary on Dashboard).

**Items that do NOT fit cleanly (called out honestly):**
- **Antenna metadata** is double-homed: it is *entered* during Setup (it's commissioning data) but is *consumed* by Logging (RINEX header) and *read* during Survey-in. Resolution: entry form lives in **Setup**; a read-only echo appears in Logging and Survey-in. Acceptable, but it's the one piece of config that spans stages.
- **Day/night toggle** is a global preference, not maintenance â€” but it has no natural lifecycle home, so it lives in the header (always reachable) with a duplicate switch in Maintenance. Header is canonical.
- **C6 OTA** exists only on touch today and **P4 OTA** only on web. Both go in Maintenance; this proposal does *not* force parity (adding C6 OTA to web or P4 OTA to touch is out of scope), so Maintenance is the one section whose action set legitimately differs per surface beyond the sanctioned web-only list.
- **Mode (SURVEY vs BASE TX)** is a state that cuts across Survey-in and Broadcast. It's shown on Dashboard as the headline and echoed in both stages; no single section "owns" it.

## 3. Home/landing screen â€” the instrument view

**Shared concept:** Dashboard is the always-on instrument face. Its defining element is a **lifecycle ribbon** under the header â€” five small stage chips (Setup Â· Survey Â· Broadcast Â· Logging Â· Maint) with the *current* stage lit in `--accent` and completed prerequisites in `--good`. This is the task-flow angle made literal: the operator always sees where the box is in its life, and the ribbon doubles as quick-nav.

**Touch (720Ã—720):**
- 64px header: "BASE STATION" + global health pill (`â— LIVE` / `â— SURVEYING` / `â— ALARM`) + day/night toggle.
- Lifecycle ribbon (~40px): 5 chips, current stage highlighted.
- 2Ã—3 tile grid in the ~536px middle band:
  - **Hero tile (spans 2 cols):** RTCM output â€” `1247` `B/s` in 48px hero, value tinted `--good` when transmitting, with `total: 4.2 MB` sub-line. In SURVEY mode this hero swaps to **survey stability** `Â±0.18 m` with a 270Â° progress arc, because that's the number that matters in that stage.
  - Mode tile (`SURVEY`/`BASE TX`, color-coded).
  - Satellites tile (`24` used, `G:9 R:6 E:6 C:3` sub-line).
  - NTRIP push tile (`2/3 LIVE` pill + local `3 clients`).
  - Position tile (lat/lon condensed, tap â†’ Survey-in).
- Bottom tab bar (80px): the six sections (Dashboard + 5 stages). Tapping a tile deep-links into its stage.

**Web (responsive):**
- Same header + lifecycle ribbon as a horizontal stepper.
- Tiles via `grid: repeat(auto-fit, minmax(220px, 1fr))` â€” identical tile content, reflowing 1â†’4 columns. At ~720px it lands on 2-up and mirrors the touch face exactly (the unification trick). Hero RTCM tile spans 2 columns at wide breakpoints.
- Top segmented nav for the six sections (collapses to bottom bar / drawer on phones). Footer keeps the raw `/status` JSON link for power users.
- Polls `/status` at the established 15 s (Dashboard); no behavior change beyond retheme.

## 4. Visual system applied

**Tokens & theme:** Adopt the two-palette token contract verbatim. Night is default. LVGL receives a `Theme` struct of `lv_color_t` + font pointers selected by day/night and applied via a shared `apply_theme()`; web emits the `:root` / `[data-theme="night"]` CSS-variable block **once** in `kAfterTitle`, replacing the three divergent inline themes. Day/night toggle persists in `Storage` so both surfaces agree.

**Tiles:** The "big readout tile" (Â§4.1) is the atom of Dashboard, Broadcast (per-service), Survey-in (the three gates), and Logging (disk arc). Value carries status color; border neutral except a 2px `--crit` left-edge bar in alarm. Tappable tiles show a faint `>` chevron.

**Readouts:** Enable Montserrat 18/28/48 on LVGL (the current build only compiles 14 â€” this closes the "no large numeric" gap). Web uses the system sans stack with `font-variant-numeric: tabular-nums` for live-changing numbers. Exactly one `hero` value per screen.

**Status colors:** The four-color semantics apply identically â€” good=transmitting/connected/logging-healthy/fix; warn=surveying/reconnecting/disk>80%/weak RSSI; crit=disconnected/SD error/disk>95%/survey failed; dim=off/idle (never red). Every status is **redundantly encoded** (color + word + glyph) so it survives sunlight and color-blindness.

**Gauges/arcs:** Survey-in uses three 270Â° arcs for its three completion gates (time, blocks, stability) â€” the marine arc made literal as a progress instrument; this fixes the current time-only progress bar that understates completion. Logging uses a disk-usage arc; Setup uses an RSSI arc.

**Nav pattern:**
- **Touch:** bottom tab bar, 6 icon+label tabs, 80px tall (â‰¥120px wide each), active tab = `--accent` icon + 3px top accent rule. This keeps the existing bottom-`lv_tabview` pattern, just re-sectioned and retitled.
- **Web:** top segmented nav (desktop) showing the same six in the same order; collapses to a bottom tab bar / hamburger drawer on narrow widths. Identical icons and labels. This *adds the shared nav the web has never had* (today it's ad-hoc footer `<a>` tags).

**Day/night:** single header control on both surfaces; touch swaps the `Theme` struct + restyles, web toggles `data-theme`. Persisted.

## 5. Per-surface notes

**Touch (720Ã—720 LVGL):**
- Six tabs vs today's five: the bottom tab bar gets one more entry. At 720px / 6 tabs that's 120px per tab â€” still finger-sized, but icons must carry most of the weight (short labels: "Setup/Survey/Cast/Log/Maint" + Dashboard). This is the tightest spot in the design and is a real tradeoff (see Â§6).
- Modals stay full-screen on `lv_layer_top()`, lazily built, dismissed-not-destroyed â€” reuse the existing `make_modal_base` + keyboard pattern for Setup forms and per-service Broadcast config.
- All the threading rules hold: WiFi scan/connect/AP-apply, C6 version query/OTA, and the **new recursive SD delete** run on worker tasks; LVGL mutation only under `bsp_display_lock()`; cross-thread progress via `std::atomic` polled by the 1 s refresh (same pattern as `c6_ota_progress_`).
- Bulk SD delete (Maintenance): per-row 36px checkbox, long-press to enter select mode, selection-count header, slide-up `Delete (N)` bar in `--crit`, routed through an `lv_msgbox` confirm showing file count + total bytes. Selection state mutated under the display lock. Active RINEX file is non-selectable.
- Cache `load_position()` in RAM instead of re-reading NVS each tick (the higher-cadence hero readout demands it).
- Survey-in's three arcs and sky plot are net-new LVGL widgets (`lv_arc` Ã—3; sky plot via `lv_canvas` or drawn points) â€” the heaviest new touch work.

**Web (responsive):**
- One shared CSS block, one `:root`/`[data-theme]` token set, one component vocabulary (tile/pill/list-row/arc/bar/confirm) reused across all pages â€” kills the three-theme divergence and the per-page inline styles on `/files` and `/rinex/export`.
- Six-section top nav rendered into `kAfterTitle` so every page shares it; current ad-hoc footer links retire.
- Dashboard tiles reflow 1â†’4 cols; the 2-col breakpoint mirrors touch.
- Bulk delete: add multi-select checkboxes + a "Delete selected (N)" + "Delete folder" action to `/files`, backed by a **new batch/recursive SD API** with a protected-path guard (mount root, managed dirs, active RINEX file) and off-task execution â€” this is the one genuine backend addition the brief implies. New endpoints register in `register_secure_handlers()` (watch the â‰¤32 handler cap; ~3 slots free, raise if needed).
- Web-only capabilities stay where the lifecycle puts them: RINEX export in **Logging**; OTA upload, console log, file download, CA cert in **Maintenance/Setup**. Console keeps monospace.
- Keep established polling cadences and `Cache-Control: no-store`; arcs as inline `<svg>` `stroke-dasharray` themed by `currentColor` (no canvas, no libraries).

## 6. Tradeoffs

- **Six tabs on a 720px touch bar is tight.** Adding a stage over today's five squeezes each tab to ~120px; labels must be terse and lean on icons. If it feels cramped in testing, fold Dashboard into a "home" affordance in the header and run five stage tabs â€” but that weakens the always-visible instrument face.
- **Lifecycle ordering fights random-access use.** A task-flow IA is great the first time and for periodic servicing, but a power user who just wants "is RTK2go up?" must know that lives under **Broadcast**, not a subsystem named "NTRIP." The stage metaphor adds a translation step for experienced operators who think in subsystems. The lifecycle ribbon + deep-linking tiles mitigate but don't eliminate this.
- **Stages aren't strictly sequential in reality.** You can be Broadcasting *and* Logging simultaneously, and you re-enter Setup at any time. The five-chip ribbon implies linear progression it doesn't truly have; we lean on "current stage lit, prerequisites green" rather than a hard wizard, which is softer but slightly less crisp than the metaphor promises.
- **Cross-cutting state gets split.** Mode (SURVEY/TX) and antenna metadata legitimately span stages, forcing duplication (echoes/read-backs). Duplication risks drift if not driven from the single `status()` read-model â€” mitigated by one data contract, but it's real surface area.
- **Two sections carry most of the density.** Survey-in (live estimate + 3 gates + sat breakdown + sky plot + manual entry) and Maintenance (health + firmware + C6 + file browser + logs) are heavy; Setup and Logging are comparatively thin. The lifecycle split produces uneven section weight rather than the even subsystem split a "by-subsystem" IA would give.
- **Naming risk.** "Broadcast" / "Logging" / "Survey-in" are operator-correct but the device's existing vocabulary (and casters' own docs) say "NTRIP" and "RINEX." We keep those as tile/sub-labels under the stage names, accepting a one-time learning cost for clearer top-level IA.
- **Net-new backend is required regardless of IA.** Recursive/bulk SD delete with protected-path guard, off-task execution, progress, and active-file protection does not exist today; this proposal depends on it for the Maintenance file browser on both surfaces. It's the single largest engineering risk and is shared by any of the competing IA proposals.

--- P2 (Live-instrument MFD) ---
PROPOSAL P2 â€” Live-instrument MFD

## 1. Organizing principle

This IA treats the base station the way a marine multi-function display treats a vessel: one always-on **instrument home** answers the only question that matters at a glance â€” "is my base healthy and transmitting?" â€” and everything else is a drill-down behind it. The home screen is a grid of big-readout tiles showing the live operational vitals (mode, satellites, RTCM rate, fix/position quality, NTRIP push health), topped by a persistent global status pill that summarizes the worst current condition (the marine "shared alarm strip"). Each tile is both a readout and a door: tapping/clicking it drills into the section that owns that data. The five sections form one shared spine â€” same names, order, icons, and status-color semantics on both surfaces â€” so the touchscreen is literally "the web app at its 720px breakpoint." Glanceability wins every tradeoff: one hero value per screen, color used only for status and always paired with a word and an icon, large bright numerics over dim tracked labels.

## 2. Unified section map

The home screen ("Helm") is section 0 â€” the instrument view. Sections 1â€“5 are the drill-downs. All six use the same icons/labels/colors on web and touch.

| # | Section (label) | Icon idea | What it contains | Current features that map in | Web-only sub-features |
|---|---|---|---|---|---|
| 0 | **Helm** (home) | compass rose / MFD grid | Live tile grid: global health pill, Mode tile (SURVEY/BASE TX), RTCM-rate hero tile, Satellites tile, Position-fix tile, NTRIP-push summary tile, Storage/RINEX mini-tile. Survey start/confirm action lives here. | Status tab "BASE OPERATION" (mode, RTCM, sats) + Survey button + survey progress; Status compact NTRIP summary; web `/` dashboard top rows | â€” (tiles are read + drill-down only) |
| 1 | **Position** | crosshair / target | Lat/lon/alt hero readouts; survey-in detail (elapsed, samples, Ïƒ, blocks, stability) shown against the three completion gates with a stability arc; satellites-by-constellation (G/R/E/C/used/tracked) + SNR list; antenna model/radome/height; **sky plot**; Set-manual-position; Start/Restart survey + confirm | Touch Position tab (FIXED BASE POSITION, SURVEY QUALITY, SATELLITES, SNR list); Status/Position survey blocks; web `/skyplot`; web `/config` Manual Position + Antenna; `POST /survey`, `POST /config/position` | Sky plot is shown on both (data exists for both); no web-only here |
| 2 | **Links** (was NTRIP) | cloud-up / broadcast | Master enable switch; three per-service tiles (RTK2go/Onocoy/RTKdata) each with status pill + bytes/bitrate + dropped + reconnects + last-error + per-service config (mountpoint/password/enable); local caster :2101 client count + client IPs | Touch NTRIP tab (ALL services switch, 3 detail cards + Config modals, LOCAL CASTER); Status compact casters; web `/config` NTRIP Services; `POST /ntrip/toggle`, `POST /config` | â€” |
| 3 | **Storage** | SD card / folder | SD mount + disk-usage arc; RINEX logging toggle + current file + epochs + files; **file browser with finger-friendly multi-select delete + whole-directory delete** (both surfaces); breadcrumb/up nav; new-folder; rename | Touch System STORAGE group + RINEX switch + file browser modal (navigation-only today); web `/files`, `/files/list|delete|rename|mkdir`, `/rinex/toggle` | File **download** (`/files/download`); bulk **RINEX export/merge** (`/rinex/export`) |
| 4 | **System** | gear | Uptime, last reset (crash-colored), free heap arc; WiFi station (SSID/RSSI/IP) + hotspot (SSID/IP) + WiFi scan/connect + AP password; firmware version + build; C6 running vs available version + **C6 OTA**; day/night toggle; admin password | Touch System tab (SYSTEM, NETWORK, FIRMWARE groups; Configure WiFi modal; C6 OTA button/flow); web `/config` WiFi + AP, `/status` system fields, `/setup`; `POST /config/wifi|ap`, setup | P4 firmware **OTA upload** (`/update`); CA cert download (`/ca.crt`) |
| 5 | **Console** (was Debug) | waveform / list | Live log tail with status-colored severity; auto-scroll | Touch Debug tab (4000-char tail); web link to logs | Full **console log viewer** with cursor/incremental fetch + "jump to end" (`/logs`, `/logs/data`) â€” stays monospace |

**Functionality audit â€” 100% placement, with call-outs:**
- Everything from both inventories lands in exactly one section above. Cross-references on Helm (RTCM, sats, NTRIP summary) are read-only mirrors that deep-link to their owning section, not duplicate owners.
- **Survey controls intentionally appear twice** (Helm action + Position): the brief's "instrument home with start/confirm" plus Position's detailed survey ownership. Both call the same `request_survey()`; the confirm dialog is shared. This is a deliberate convenience duplication, not an IA ambiguity.
- **Does-not-fit-cleanly call-outs:** (a) **C6 OTA** is touch-only today and **firmware (P4) OTA** is web-only today â€” both live in **System** but on opposite surfaces; for true parity I recommend adding a web C6-OTA action (a route exists nowhere yet) OR explicitly documenting C6 OTA as a touch-only divergence. I place both in System and flag the asymmetry rather than hide it. (b) **CA cert download** and **first-run `/setup` password gate** are web-only mechanics with no touch equivalent â€” they sit in System but render only on web. (c) **Day/night toggle** is net-new and lives in the header on both surfaces but is "owned" by System for its persisted setting.

## 3. Home/landing screen (the instrument view)

**Shared concept:** a persistent header (section title left, **global status pill** right â€” green `LIVE` / amber `SURVEYING` / red `ALARM`, computed as worst-of-all-subsystems, plus day/night toggle) over a grid of big-readout tiles. Exactly one hero value per screen.

**Touch (720Ã—720), "Helm":** Under the 64px header and above the 80px bottom tab bar (~576px usable), a 2-column tile grid, touch-scroll for overflow:
- **Hero tile (spans 2 cols): RTCM OUTPUT** â€” `1247` in 48px hero + `B/s` unit, value tinted by status (green transmitting, dim if survey/idle), sub-line "total 4.2 MB". This is the heartbeat of a working base.
- **Mode tile** â€” big `BASE TX` (green) / `SURVEY` (amber) with the survey progress bar + "elapsed/blocks/Â±Ïƒ" sub-line when surveying.
- **Satellites tile** â€” big total `28`, sub-line `G12 R7 E6 C3`.
- **Position tile** â€” "FIX" pill + compact lat/lon, taps into Position.
- **NTRIP tile** â€” pill `3/3 UP` or `1 DOWN` (worst-of services) + local-client count.
- **Storage mini-tile** â€” disk-% arc + RINEX dot (logging green / off dim).
- Tapping any tile drills into its section. A **Start Survey** button sits at the bottom of the grid (full-width, amber), routed through the shared confirm dialog when leaving Base TX.

**Web, "Helm" (`/`):** identical tiles in a CSS grid `repeat(auto-fit, minmax(220px, 1fr))` â€” collapses to 1 column on a phone, expands to 4 on desktop; the ~720px 2-column breakpoint is visually the touch screen. Same hero RTCM tile, same pills, same global status pill in the top nav. Tiles are `<a>`-wrapped to deep-link into sections. Live values refresh from the existing `GET /status` poll (15s), rebuilding each tile rather than table rows.

## 4. Visual system applied

Tokens are the design-system's two-palette contract (`--bg/--surface/--text/--good/--warn/--crit/--accent` etc.), night default, persisted in `Storage` so both surfaces agree.

- **Tiles:** the atom on both surfaces â€” surface bg, 12px radius, 1px border, label (uppercase/dim/tracked) top, big value + dim unit center, optional caption. The **value** carries status color (B&G pattern), not the whole tile; `crit` adds a 2px red left-edge bar (redundant cue). Tappable tiles show a faint `>` chevron.
- **Readouts:** type ramp `hero 48 / value 28 / value-sm 18 / label 14 / unit 14`. Touch enables `montserrat_18/28/48` (closing the "no large numerics" gap); web uses a system sans stack with `tabular-nums` for live-changing numbers. Console alone keeps monospace.
- **Status colors:** four only (good/warn/crit + dim-for-off), each always paired with a **word + icon** in a capsule pill (`â— LIVE`, `â— SURVEYING`, `â— OFFLINE`, `â—‹ OFF`). "Off" is dim, never red. Identical semantics across surfaces (transmitting/connected/mounted = green; survey/reconnect/disk>80%/weak RSSI = amber; dropped/unmounted/disk>95%/failed = red).
- **Gauges/bars:** `lv_arc` (touch) / SVG `stroke-dasharray` (web) for bounded quantities â€” survey stability, disk %, RSSI, free heap. Survey progress shown against all three gates (â‰¥300s, â‰¥5 blocks, â‰¤0.50m), not time-only as today.
- **Nav pattern â€” touch:** **bottom tab bar**, 80px tall, 6 icon+label destinations (Helm, Position, Links, Storage, System, Console), active = accent icon + 3px top accent rule. (Replaces today's 52px bottom tabview â€” same edge, bigger finger target.)
- **Nav pattern â€” web:** **top segmented nav** carrying the same 6 sections (icons+labels+order), with the global status pill and day/night toggle; collapses to a bottom tab bar / drawer on narrow screens so phone web mirrors the touch layout.
- **Day/night:** single header toggle. Touch swaps an `lv_color_t` Theme struct via a shared `apply_theme()`; web toggles `data-theme="night"` on `<html>` over `var(--token)`s. One CSS variable block emitted once as a shared literal (replaces today's three divergent per-page styles).

## 5. Per-surface notes

**Touch (720Ã—720 LVGL):**
- Helm is a flex/grid of the reusable tile component (generalizes today's `make_group`); the existing `make_row/make_switch_row` survive inside drill-downs. Tab bar grows 52â†’80px; six tabs at â‰¥120px width each fit 720px.
- Per the threading rules, all WiFi/C6/recursive-delete work stays on worker tasks (`wifi_scan_task`, `c6_ota_task`, new `sd_delete_task`) with results marshalled back under `bsp_display_lock()`; progress via `std::atomic<int>` polled by the 1s refresh â€” same pattern as C6 OTA today. Position should be cached in RAM to feed faster numeric readouts instead of NVS-reading every tick.
- **Multi-select delete:** long-press a file row enters select mode (per-row 36px checkbox, selection in a `std::vector<bool>` keyed to `fb_entries_`), a slide-up action bar shows `Delete (N)` in red; directory rows offer "Delete folderâ€¦"; everything routes through the shared `lv_msgbox` confirm with file count + total bytes.
- **Acceptable touch omissions** (web-only, no finger value): file download, P4 OTA upload, RINEX export/merge, full console viewer (touch shows the tail), CA cert, first-run setup.
- Heap discipline: only four Montserrat sizes enabled (~3â€“10KB each); modals/file lists stay lazy-built; sat list still capped.

**Web (responsive):**
- One shared CSS variable theme block + one inline-SVG icon set colored by `currentColor`, replacing the three divergent themes (`send_page`, `/files`, `/rinex/export` unify). Every page wraps in `send_page` with the new top nav.
- Same six sections render as full pages; tiles use `auto-fit minmax` so 720px â‰ˆ touch, desktop = 4-col, phone = 1-col. Existing `GET /status` (15s), `/skyplot/data` (10s), `/logs/data` (5s) polling cadence preserved; JS rebuilds tiles. Long-term, fold the duplicated JS/C++ formatting toward one contract.
- **Multi-select delete:** checkboxes per row + a sticky bulk action bar (`Delete N`, `Delete folderâ€¦`), routed through the same confirm dialog and a new batched/recursive `SdManager` API with a protected-path guard (never delete `/sdcard`, `/sdcard/logs`, `/sdcard/rawdata` as dirs, never the active RINEX file) and off-task execution with progress â€” this backend is net-new for both surfaces.
- Web-only sections/actions (download, OTA upload, RINEX export, full logs, CA cert, setup) appear inline where they belong (Storage/System/Console) and simply have no touch counterpart.

## 6. Tradeoffs

- **Duplication risk on Helm.** Mirroring RTCM/sats/NTRIP/position on the home grid and again in their sections means two render paths for the same values; if they diverge, the operator sees conflicting numbers. Mitigation: Helm tiles must read the same `status()` snapshot and be strictly read-only deep-links, never independent owners.
- **Glanceability vs density.** "One hero per screen, big numbers" is great at distance but spends vertical space; the 720px touch grid can show ~4â€“6 tiles before scrolling, so some Helm content scrolls â€” slightly at odds with "no scroll" instincts. We accept touch-scroll (already the norm) and rank tiles so the top 4 (RTCM, Mode, Sats, NTRIP) never require scrolling.
- **The OTA asymmetry is real and unresolved by IA alone.** Placing C6 OTA (touch-only) and P4 OTA (web-only) both in System exposes that the two surfaces aren't truly feature-identical there. The IA flags it but the fix is engineering (add a web C6-OTA route or formally bless the divergence), not layout.
- **Tile-as-button discoverability.** Marine MFD tiles double as nav; new operators may not realize a readout is tappable. The chevron + ripple helps but isn't foolproof on a glance-first screen.
- **Per-service NTRIP density.** Three services Ã— (status/bytes/drops/reconnects/last-error/config) is a lot to express as tiles without becoming a wall; "Links" tiles may need a compact list-row variant inside, slightly breaking the pure-tile grammar.
- **New large fonts + arcs cost flash/SRAM.** Four Montserrat sizes plus arc widgets add to a tight internal-SRAM budget; must verify the ~95KB free margin holds and keep heavy UI lazy-built.
- **Backend is the long pole.** The bulk/recursive delete with protected-path guard, active-file protection, off-task execution and progress does not exist yet and is shared-critical; the prettiest UI can't ship until that lands safely on both surfaces.
- **Two render targets, one contract â€” but no enforcement.** Nothing mechanically prevents web and touch from drifting (token values, icon choices, wording); keeping them unified is a discipline/process cost, not something this IA guarantees.

Relevant files (absolute): `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\ui.cpp`, `ui.hpp`, `web_server.cpp`, `web_server.hpp`, `sd_manager.hpp`, `sd_manager.cpp`, `base_station.hpp`.

--- P3 (Status-dashboard) ---
PROPOSAL P3 â€” Status-dashboard

## 1. Organizing principle

The whole interface answers one question first â€” "Is my base healthy, and what is it doing right now?" â€” before it offers any depth. We promote a single comprehensive **Dashboard** to the front of both surfaces: a grid of marine-instrument tiles that summarizes every subsystem (mode, RTCM output, satellites, NTRIP push, position fix, SD/RINEX, link health) with redundantly-coded status pills, plus inline toggles for the two things an operator touches most (start survey, master NTRIP on/off, RINEX on/off). Everything else collapses into four focused sections reached by one tap, where each Dashboard tile is also a shortcut into its own detail section. The design optimizes for *taps-to-glance* (zero â€” health is the landing screen) and *taps-to-toggle* (one â€” common controls live on the Dashboard), pushing configuration and rarely-used depth one level down. The same five-section spine, the same words, icons, and status-color semantics drive both the LVGL tab bar and the web nav, so the touchscreen is literally "the web product at its 2-column breakpoint."

## 2. Unified section map

| # | Section (shared name) | Icon idea (LVGL glyph / web SVG) | What it contains | Current features that map in | Web-only sub-features |
|---|---|---|---|---|---|
| 1 | **Dashboard** | Home / gauge cluster (`LV_SYMBOL_HOME`) | Global health pill (worst-of-all). Tile grid: **Mode** (SURVEY/BASE TX) with hero, **RTCM output** (B/s + total), **Satellites** (used/tracked + G/R/E/C), **NTRIP push** rollup (n of 3 live + local clients), **Position fix** (lat/lon/alt compact + valid pill), **SD/RINEX** rollup (mount + logging + disk arc), **Link** (WiFi SSID/RSSI or AP). Inline controls: **Start/Restart/New Survey** button (+confirm), **NTRIP master** toggle, **RINEX** toggle. Each tile taps through to its owning section. | Touch Status tab (BASE OPERATION, POSITION compact, NTRIP CASTERS compact, survey progress bar/label); web `/` status table + `toggleNtrip`/`toggleRinex` buttons + survey | Live JSON poll (`/status`); raw-JSON link |
| 2 | **Position** | Crosshair (`LV_SYMBOL_GPS`) | Lat/lon/alt **hero readouts**; survey-in detail (state, elapsed, samples, Ïƒ, instantaneous sigma, **3-gate completion**: â‰¥300 s / â‰¥5 blocks / stability â‰¤0.50 m as a stability arc); satellites by constellation (used/tracked, G/R/E/C); per-sat SNR list; **sky plot**; **antenna model/radome/height**; manual-position entry. | Touch Position tab (FIXED BASE POSITION, SURVEY QUALITY, SATELLITES + SNR list) + survey block; web `/skyplot`, `/config` position + antenna forms, `/config/position`, `/survey` | â€” (sky plot now on both; antenna editable on both) |
| 3 | **Links** | Cloud-up / broadcast (`LV_SYMBOL_WIFI`) | Per-service tiles RTK2go / Onocoy / RTKdata â€” status pill + bytes sent / bitrate / dropped / reconnects / last-send age / last error, each with **Config** (mountpoint + password + enable); **master enable**; **local caster :2101** clients + client IPs. | Touch NTRIP tab (ALL NTRIP SERVICES toggle, 3 detail cards + Config modals, LOCAL CASTER) + Status NTRIP compact; web `/` service rows + `/config` NTRIP services + `/ntrip/toggle` | â€” |
| 4 | **Storage** | SD card / folder (`LV_SYMBOL_SD_CARD`) | SD mount + **disk-usage arc** + used/total; **RINEX logging toggle** + current file + epochs + files; **file browser** with **finger-friendly multi-select delete + whole-directory delete** (confirm w/ count+size); breadcrumb nav, new folder, rename. | Touch System STORAGE group (SD, RINEX switch, Browse SD modal) + Status; web `/files` (list/delete/rename/mkdir), `/rinex/toggle`, storage bar | **File download**, **bulk RINEX export/merge** (`/files/download`, `/rinex/export`) |
| 5 | **System** | Gear (`LV_SYMBOL_SETTINGS`) | Uptime, last reset (crash-red), free heap arc, firmware version + build, **C6 running/available version + C6 OTA**, WiFi station (SSID/RSSI/IP) + hotspot (SSID/IP) + **Configure WiFi** + **hotspot password**, **day/night toggle**, admin password. | Touch System tab (SYSTEM, NETWORK + Configure WiFi modal, FIRMWARE + C6 OTA) + WiFi modal hotspot section; web `/config` WiFi/AP, `/setup` | **P4 firmware OTA upload** (`/update`), **full console log viewer** (`/logs`, monospace), **CA cert download** (`/ca.crt`) |

**Coverage check â€” every current feature placed:** Mode/RTCM/sats/survey-button â†’ Dashboard+Position. Stored position + survey quality + SNR + sky plot + antenna â†’ Position. All 3 NTRIP services + local caster + master toggle â†’ Links. SD/disk/RINEX/file browser â†’ Storage. Uptime/reset/heap/firmware/C6/WiFi/AP/admin/day-night â†’ System. Web-only download, OTA upload, RINEX export, full logs, CA cert â†’ noted in their sections. **Nothing is dropped.**

**Does-not-fit-cleanly callouts:**
- **RINEX toggle lives in two mental homes** â€” it is a *Storage* concern (writes SD files) but is gated by *Base TX mode* (a Position/operation concern). I place the control in **Storage** and **mirror it as a Dashboard tile toggle**, with a disabled+hint state ("Requires Base TX") when in survey mode.
- **C6 OTA** exists only on touch today; the unified spine puts it under **System** on **both** surfaces (add a web route for parity). If web parity is deferred, it stays touch-only as an accepted asymmetry â€” but the IA slot is identical.
- **Day/night toggle** is global chrome (lives in the header on both), but its *persistence* setting is surfaced in **System** so it has a discoverable home.
- **Master NTRIP toggle** appears both on the Dashboard (quick) and in Links (canonical). One backend call (`set_streams_enabled`), two entry points kept in sync by the refresh resync guard pattern.

## 3. Home/landing screen â€” the instrument view

**Shared concept:** a persistent header (section title + **global status pill** = worst-of-all-systems + day/night toggle) sits above a grid of big-readout tiles. The global pill is the marine "shared alarm strip": green **LIVE**, amber **SURVEYING**, red **ALARM**. One hero value dominates; everything else steps down.

**Touch (720Ã—720):** Under the 64px header and above the 80px bottom tab bar (~576px usable), a **2-column tile grid**, touch-scroll for overflow:
- Row 1: **Mode** tile (hero â€” "BASE TX" green / "SURVEYING" amber) spanning the survey progress arc when in survey; **RTCM output** hero tile ("1247 B/s", dim total beneath).
- Row 2: **Satellites** ("23 used / 31 tracked", sub-line "G9 R6 E5 C3"); **NTRIP push** ("2 / 3 LIVE" + "4 local clients").
- Row 3: **Position** ("47.3712Â°, 8.5402Â°", valid pill); **SD / RINEX** (disk arc + "LOGGING â— / file ep:412").
- Row 4: **Link** tile (SSID + RSSI, or "AP: GPS-BaseStation"); spare for alarms.
- Pinned action row (or in-tile buttons): **Start/Restart Survey** (amber, confirm), **NTRIP** master switch, **RINEX** switch. Each non-action tile taps into its owning section.

**Web:** Same header (with hamburger/segmented nav). Tiles in CSS grid `repeat(auto-fit, minmax(220px,1fr))` â€” 1 col on phone, 2 col at ~720px (mirrors touch exactly), up to 4 col on desktop. Same tiles, same order, same pills. Inline NTRIP/RINEX toggle buttons and the Start-Survey button live in their tiles. Footer keeps the raw-JSON and (now nav-linked) section links. Polls `/status` every 15 s as today.

First-run web still gates to `/setup` (admin password) before the Dashboard is reachable.

## 4. Visual system applied

**Tiles & readouts:** every datum is a *big-readout tile* (Â§4.1 of the design system): surface bg, 12px radius, 1px border, uppercase dimmed `label`, large bright `value` + dim `unit`, optional dim caption. The **value** carries status color (B&G pattern), not the whole tile; a `crit` tile gains a 2px red left-edge bar as a redundant cue. Tappable tiles show a faint `>` chevron. Exactly one `hero` (48px touch / clamp web) per screen.

**Status colors (identical both surfaces, redundantly encoded):** good=green (base TX, NTRIP connected, SD mounted, RINEX healthy), warn=amber (survey-in, reconnecting, disk >80%, weak RSSI), crit=red (disconnected, SD error, disk >95%, survey failed, crash reset), dim=off/idle (disabled service â€” never red). Every pill pairs color + word + dot/icon (`â— LIVE`, `â— SURVEYING`, `â— OFFLINE`, `â—‹ OFF`).

**Gauges/bars:** survey progress, disk %, free heap, RSSI render as 270Â° arcs (LVGL `lv_arc` / web inline-SVG `stroke-dasharray`, themeable via `currentColor`). Survey-in and OTA use the retheme'd progress bar (track `--surface-hi`, fill goodâ†’crit).

**Nav pattern:**
- **Touch:** bottom tab bar, **5 icon+label tabs** (Dashboard / Position / Links / Storage / System), 80px tall, each â‰¥120px wide; active tab = `--accent` icon + 3px accent top-rule. Reuses the existing `lv_tabview` with `LV_DIR_BOTTOM`.
- **Web:** top segmented nav on desktop (same 5, same icons/labels/order) that collapses to a drawer/bottom bar on narrow widths. One shared CSS `:root`/`[data-theme]` block emitted once (replaces the three divergent per-page styles).

**Day/night:** single header toggle. LVGL swaps an `lv_color_t` Theme struct via a shared `apply_theme()`; web toggles `data-theme="night"` on `<html>` (all colors are `var(--token)`). Night is default. Choice persisted in `Storage` so both surfaces agree.

## 5. Per-surface notes

**Touch (720Ã—720 LVGL):**
- Tab bar stays bottom-anchored (existing pattern); five tabs. New numeric fonts (`montserrat_18/28/48`) enabled in `lv_conf.h` for the hero/value ramp â€” the biggest visible upgrade over today's flat 14px.
- All tiles built lazily per tab (keep init fast, respect ~95KB internal SRAM); arcs/bars are LVGL-native (no bitmaps). Position cache in RAM instead of NVS-read-per-tick to support snappier readouts.
- **Multi-select SD delete** added: long-press a file/dir row to enter select mode â†’ per-row 36px checkbox, selection-count header, bottom **Delete (N)** bar in red, routed through an `lv_msgbox` confirm (count + bytes). Whole-directory delete offered on dir rows. Selection state mutated under `bsp_display_lock()`; the recursive/batch delete runs on a **worker task** with an atomic progress counter (mirrors C6 OTA pattern) â€” never on the LVGL task.
- All SDIO/RPC work (WiFi scan/connect, AP apply, C6 OTA/version) stays on worker tasks; the 1s refresh timer re-reads `status()` and updates every tile under the existing lock; switch resync guards keep Dashboard/Links toggles from fighting backend.
- Sky plot is new on touch: render as `lv_canvas`/arc primitives in Position (data already available via `satellites()`).

**Web (responsive):**
- Same five sections as nav routes; Dashboard = retheme'd `/`. One shared theme block; `/files` and `/rinex/export` stop carrying private styles and adopt the tokens (export page keeps its richer controls but in-theme; logs view keeps **monospace** â€” correct there).
- Tiles reflow 1â†’2â†’4 col; the 720px breakpoint is intentionally the touch layout. Existing 15s/10s/5s poll cadences and `Cache-Control: no-store` preserved.
- **Bulk SD delete** added to `/files`: row checkboxes + "Select all" + a `Delete (N)` action and "Delete folderâ€¦", hitting a new batch/recursive endpoint with protected-path guard (never `/sdcard`, `/logs`, `/rawdata` as dirs; refuse the active RINEX file). Keeps the 4KB-chunk + 1-tick pacing discipline for any streamed responses.
- Web-only depth (download, OTA upload, RINEX export, full logs, CA cert) lives inside its section, clearly marked as web-only â€” no touchscreen equivalent expected.
- New secure handlers must fit `max_uri_handlers` (â‰ˆ3 free of 32; raise cap if adding C6-OTA + batch-delete + day/night-persist).

## 6. Tradeoffs

- **Dashboard density vs. glanceability.** Seven rollup tiles plus three controls on one 720px screen risks the "everything is big, so nothing reads" failure. Mitigation: strict hero discipline (one dominant value), rollups (e.g. "2/3 LIVE") not raw triplets, and touch-scroll for overflow â€” but it remains the tightest screen and needs careful sizing/QA.
- **Duplicated controls (NTRIP master, RINEX) in two places** can drift or confuse ("I toggled it on the Dashboard, why is Links still showing old state?"). Mitigation is the resync-guard pattern, but two entry points is more surface area to test than one.
- **Rollups hide detail.** A green "NTRIP 2/3 LIVE" tile masks *which* service is down until you tap in; an operator triaging a specific caster pays a one-tap tax versus today's always-expanded NTRIP list. This is the explicit cost of status-first.
- **Survey/RINEX live in adjacent sections** (Position vs Storage) while being operationally linked (RINEX needs Base TX, survey resets position). A user mid-survey-setup may bounce between Position and Storage/Dashboard. The Dashboard mirror softens this but doesn't eliminate it.
- **More numeric fonts = more flash/SRAM.** Four Montserrat sizes add ~15â€“30KB flash and the larger glyphs press on tight internal SRAM if cached; must verify against the ~95KB headroom and keep heroes PSRAM-friendly.
- **New backend dependency:** the bulk/recursive SD delete (protected-path guard, off-task worker, progress, active-file protection) does not exist today and is the single largest net-new engineering item this IA implies â€” a design that *reads* clean but carries real implementation risk on SDSPI.
- **C6-OTA web parity and day/night persistence** each consume scarce URI-handler slots and add routes; if deferred, the IA shows an intentional asymmetry that slightly dents the "identical on both surfaces" promise.

Files referenced (read-only, unchanged): `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\ui.cpp`, `ui.hpp`, `web_server.cpp`, `web_server.hpp`, `base_station.hpp`, `sd_manager.hpp/.cpp`, `storage.hpp`, `survey.hpp`, `rinex_logger.hpp`, `wifi_manager.hpp`, `app_config.hpp`.
