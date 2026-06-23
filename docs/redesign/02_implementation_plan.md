I now have complete grounding in the actual code. Every claim in the design spec checks out against the source: 5-tab `lv_tabview` at `ui.cpp:338-349`, single palette at `51-57`, `montserrat_14`-only, `c6_ota_progress_` atomic pattern, 29/32 URI handlers, shared `kAfterTitle` style block, `delete_entry` doing only `rmdir`/`unlink` at `sd_manager.cpp:226-230`, and `RinexLogger::Status{active, current_file}` for the active-file guard.

Here is the phased implementation plan.

---

# GPS RTK Base Station â€” Phased Implementation Plan

*Turns the Unified Design Spec into a reviewable, dispatchable build. DESIGN/PLANNING artifact only â€” no code is written here. All paths absolute.*

Grounded against the actual tree (`gps-base-station/main/`): `ui.cpp` 1704 lines / `ui.hpp` 217; `web_server.cpp` 1982 / `web_server.hpp` 75; `sd_manager.cpp` 256 / `sd_manager.hpp` 45. Verified anchors: 5-tab `lv_tabview` (`ui.cpp:338-349`), single palette (`ui.cpp:51-57`, `montserrat_14` only), `c6_ota_progress_` off-task atomic pattern (`ui.hpp:102`), 29 URI handlers vs `max_uri_handlers=32` (`web_server.cpp:269,331-362`), shared `kAfterTitle` style (`web_server.cpp:1316-1327`), `delete_entry` = bare `rmdir`/`unlink` (`sd_manager.cpp:226-230`), active-file guard data `RinexLogger::Status{active,current_file}` (`rinex_logger.hpp:16-21`, `base_station.hpp:61`).

---

## 1. Workstreams & phases (build order, parallelism, dependencies)

Six workstreams (WS). The hard dependency is the **shared constants header** (WS-A) and the **SD backend** (WS-B); everything visual fans out after A, and the two bulk-delete UIs depend on B.

```
Phase 0  WS-A  Shared tokens/constants header        [SEQUENTIAL â€” gates all UI]
              + touch Theme struct + apply_theme()
              + web :root/[data-theme] token block
                       â”‚
        â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
Phase 1 â”‚ WS-B SD backend (recursive/bulk delete,      â”‚  [PARALLEL with WS-C/D shells]
        â”‚      guards, off-task task, progress atomics) â”‚
        â”‚      sd_manager.* + base_station guard hook    â”‚
        â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                       â”‚ (B needed only by Phase 3 bulk-delete UIs, not by shells)
        â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
Phase 2 â”‚ WS-C Web shell + 5 sections (re-theme/re-IA) â”‚  â”
        â”‚ WS-D Touch shell + 5 sections (re-theme/re-IA)â”‚  â”œ PARALLEL (disjoint files)
        â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â”˜
                       â”‚ (shells must exist before delete UI hangs off them)
        â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
Phase 3 â”‚ WS-E Web bulk-delete UI   (needs B + C)       â”‚  â” PARALLEL
        â”‚ WS-F Touch bulk-delete UI (needs B + D)       â”‚  â”˜ (disjoint files)
        â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
Phase 4  WS-G  Polish, day/night persistence wiring, cross-surface
              consistency audit, flash/heap measurement  [SEQUENTIAL â€” integration]
```

**Sequencing rationale**
- **Phase 0 first, always.** Both UIs consume the token contract; building shells before it means a second re-theme pass. WS-A is small but blocking.
- **WS-B (SD backend) parallel with the shells.** The backend has *no* dependency on either UI. It must land before Phase 3 but can be built concurrently with Phase 2. Pull it early (Phase 1) so it is reviewed/merged before the delete UIs need it.
- **WS-C and WS-D fully parallel** â€” disjoint files (`web_server.*` vs `ui.*`),å…±äº« only the read-only WS-A header. This is the bulk of the work and the main parallelization win.
- **WS-E/WS-F parallel** but each gated on its shell (C/D) plus the backend (B).
- **WS-G last** â€” only after all sections exist can the consistency audit and the final flash/heap/OTA measurement be meaningful.

**Critical path:** A â†’ (C or D) â†’ (E or F) â†’ G. WS-B runs alongside and merges before E/F. Touch (Dâ†’F) is the longer leg (LVGL fonts, canvas sky-plot, off-task selection state), so dispatch D before C if a worker is scarce.

---

## 2. File-by-file change list (per phase)

### Phase 0 â€” WS-A: Shared design tokens & theme plumbing
| File | Created/Modified | Change |
|---|---|---|
| `main/ui_sections.hpp` | **CREATE** | The single source of truth both surfaces include. `enum class Section{Dashboard,Position,Links,Storage,System}`; `kSectionTouchLabel[]` (Dash/Position/Links/Storage/System), `kSectionWebName[]`, `kSectionTouchIcon[]` (LVGL symbols), per-section purpose strings; status enum `Status{Good,Warn,Crit,Idle}`; all F.7 bulk-delete strings verbatim (`kSelect`, `kDeleteN`, `kDeleteFolder`, `kEmptyFolder`, confirm bodies, `kFolderKept`, active-block message, `kDeletedNM`, `kDeletingProgress`); shared humanizer contract. Plain `constexpr`/`#define`, no LVGL/IDF types so web can include it cleanly. |
| `main/theme.hpp` | **CREATE** | Day/Night `lv_color_t` token table (C.1) as a `Theme` struct + two `constexpr` instances; `extern const Theme* g_theme;`. Hex tokens mirror the web `:root` so the two can be diffed. (LVGL-only; web does not include this.) |
| `main/ui.cpp` | **MODIFY** | Replace `kBgScreenâ€¦kDimCol` (`51-57`) with `Theme`-struct lookups; add `apply_theme(bool night)` + a `current_theme_` pointer; route all existing color usages through it. No layout change yet (keeps Phase 0 reviewable in isolation). |
| `main/lv_conf.h` (component config) | **MODIFY** | Enable `LV_FONT_MONTSERRAT_18/28/48` (14 already on). Flash `.rodata` cost only â€” measure in WS-G. |
| `main/web_server.cpp` | **MODIFY** | Replace `kAfterTitle` (`1316-1327`) terminal CSS with the marine `:root`+`[data-theme="night"]` token block + base component CSS (tiles/pills/arcs/nav). This single edit re-themes every `send_page` caller at once. Do **not** yet touch `/files`/`/rinex/export`/`/logs` inline themes (Phase 2/3). |

### Phase 1 â€” WS-B: SD backend (bulk/recursive delete)
| File | Created/Modified | Change |
|---|---|---|
| `main/sd_manager.hpp` (`45`) | **MODIFY** | Add POD types `DeleteResult`, `DeletePreview`, `enum class DeleteGuard`; methods `check_deletable()`, `preview_delete()/preview_delete_many()`, `delete_recursive()`, `delete_paths()`; constants `kInlineDeleteMax=16`, `kPreviewWalkCap=4096`. No exceptions/RTTI (already the house style). |
| `main/sd_manager.cpp` (`256`) | **MODIFY** | Implement recursive delete (currently `delete_entry` at `226-230` only `rmdir`/`unlink`s â€” fails on non-empty); deny-list guard (mount root `/sdcard`, managed shells `/sdcard/logs` + `/sdcard/rawdata` = empty-not-remove via `ensure_dirs` knowledge at `242-244`); atomic batch pre-validation (any bad path rejects whole batch); re-point existing `delete_entry` through `check_deletable`. Reuse `safe_path` (`146`). |
| `main/base_station.hpp/.cpp` | **MODIFY (small)** | Owner of `sd_delete_task` (prio 4, ~4096 stack) + atomics `sd_delete_state_/_progress_/_total_` (the Â§F.4 mirror of the C6 pattern), plus the caller-layer **active-RINEX-file guard** (it alone sees `rinex_status().active/current_file`, `rinex_logger.hpp:16-21`). Exposes a small "request bulk delete / poll progress" surface both UIs call. *(If the user prefers to keep `BaseStation` lean, this can instead live as a free helper in a new `sd_delete.cpp` that takes refs to `SdManager`+`RinexLogger` â€” Open Question Q4.)* |

### Phase 2 â€” WS-C (web shell+sections) â€– WS-D (touch shell+sections)
| File | Created/Modified | Change |
|---|---|---|
| `main/web_server.cpp` | **MODIFY (large)** | Add a shared `nav_html()` emitter (5-section segmented nav + global pill + day/night toggle) injected after the `<h1>` in `send_page`; rebuild `root_handler` (`433`) into the tile-grid Dashboard (read-only `<a>` deep-link tiles, hero RTCM, Start-Survey row); re-section existing handlers under the spine (Position/Links/Storage/System) â€” mostly re-grouping + re-theming existing markup, not new endpoints; bring `/files` (`files_page_handler` ~`1597`) and `/rinex/export` and `/logs` (`1251`,`1784`) onto the tokens (`/logs` keeps monospace `<pre>` only). Status JSON (`status_handler`) gains a worst-of-all field if not derivable client-side. |
| `main/web_server.hpp` | **MODIFY (small)** | Declare `nav_html()` / any new private helpers (no new route handlers in Phase 2). |
| `main/ui.cpp` | **MODIFY (large)** | Rename/retarget the 5 tabs to Dashboard/Position/Links/Storage/System (`338-349`), 80px bottom bar; build the Dashboard tile grid (hero + ranked tiles, read-only deep-links, Start-Survey button reusing `on_survey_*`); consolidate survey into Position (three-gate arcs replacing the time-only `bar_survey_`), add lazy `lv_canvas` sky-plot; move NTRIP into Links (reuse `ntrip_detail_group`/`open_ntrip_modal`); fold Debug tail into System console; antenna edit in System + echoes; apply hero/value fonts. |
| `main/ui.hpp` | **MODIFY (medium)** | Rename tab members; add new label/arc/canvas handles; add sky-plot + theme-toggle members. |

### Phase 3 â€” WS-E (web bulk delete) â€– WS-F (touch bulk delete)
| File | Created/Modified | Change |
|---|---|---|
| `main/web_server.cpp` | **MODIFY** | Selection-mode `/files` UI (checkboxes, Select-all tri-state, live counter, typed-"DELETE" friction for non-empty dirs, async progress poll); **3 new handlers**: `POST /files/preview`, `POST /files/delete-batch`, `GET /files/delete-status`; minimal JSON-array extractor (current `json_field`, `web_server.hpp:74`, is flat-only); register in `register_secure_handlers` (`331-362`). |
| `main/web_server.hpp` | **MODIFY** | Declare `files_preview_handler`, `files_delete_batch_handler`, `files_delete_status_handler`, plus the array extractor. |
| `main/ui.cpp` | **MODIFY** | Extend `build_file_browser/refresh_file_browser/on_fb_item`: long-press + Select mode, â‰¥44px checkboxes, slide-up action bar, confirm `lv_msgbox` with "I understand" toggle, progress view driven by the 1s refresh reading WS-B atomics; selection as `std::vector<bool>` keyed to `fb_entries_`, mutated only under `bsp_display_lock()`; suppress list-rebuild while in selection mode. |
| `main/ui.hpp` | **MODIFY** | Add selection-state members + new file-browser callbacks. |

### Phase 4 â€” WS-G: Polish / persistence / audit
| File | Modified | Change |
|---|---|---|
| `main/storage.hpp/.cpp` | **MODIFY (small)** | Persist `day_night` preference (NVS) so both surfaces agree (C.1/E.1). Add getter/setter; web toggle + touch header toggle both write it. *(Day/night persist may need 1 web route â€” see Â§E.2 / Q3.)* |
| `main/ui.cpp`, `main/web_server.cpp` | **MODIFY** | Wire toggles to `Storage`; final spacing/contrast tuning; consistency fixes from the audit. |
| (all touched) | review | Flash/heap measurement, OTA/rollback dry-run, screenshot diff web-vs-touch. |

---

## 3. Shared-consistency strategy

**One header, two includers.** `main/ui_sections.hpp` (WS-A) is the single source of section names, order, icons, status semantics, and *every* bulk-delete string from F.7. Both `ui.cpp` and `web_server.cpp` `#include` it. Rules to keep it includable by both:
- Only `constexpr`/`#define`/plain enums â€” **no `lv_*` or IDF types** in `ui_sections.hpp`. LVGL-specific bindings (the symbol glyphs, `Theme` `lv_color_t`) live in `theme.hpp`, included only by `ui.cpp`. The web maps the same logical icons to inline SVG by the same enum index.
- Section order is an `enum class Section` â€” iterate it to build both nav bars, guaranteeing identical ordering.
- **Color parity by construction:** the hex values in `theme.hpp` and the web `:root` block are authored from the same C.1 table. WS-G adds a tiny build-time check (a comment-table or a generated diff) so a change to one is caught against the other. (A future nicety: generate both from one `.def` â€” out of scope, noted in Q5.)
- **Wording parity:** all user-facing delete strings come from the header constants, referenced by name in both renderers â€” neither surface hand-types "This cannot be undone."
- **Status semantics** (good/warn/crit/idle thresholds â€” disk>80/95%, weak RSSI, crash reset) live as shared `constexpr` thresholds + a shared `classify_*()` contract so both surfaces color identically.

**Asymmetries are declared, not accidental** (B.3): web-only features (download, RINEX export, P4 OTA, full console, CA cert, `/setup`) and touch-only C6-OTA occupy the **same Section slot** on both; the header carries a per-feature `web_only`/`touch_only` note so reviewers can confirm nothing silently diverges.

---

## 4. Risks & mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| **Internal SRAM ceiling (~95KB free).** New labels/arcs/canvas inflate the LVGL object tree. | Med | Sky-plot `lv_canvas` is **lazy-built and only when Position is visible** (free it on tab-leave); modals stay dismissed-not-destroyed (existing pattern); fonts go to **flash `.rodata`, not SRAM** (the cheap win). Measure free internal heap before/after in WS-G; budget gate at merge. |
| **Flash budget (app partition 6MB).** Three new fonts + larger HTML strings. | Low-Med | Montserrat 18/28/48 are bounded; HTML is `std::string`-built then chunked (no extra static `.rodata` beyond the one shared CSS block, which *replaces* 3 divergent themes â€” likely net-neutral). Report `idf.py size` delta in WS-G. |
| **LVGL task locking / cross-thread state.** Recursive SD delete, WiFi scan, C6 OTA must never run on the LVGL or httpd task. | Med | All long FS/RPC work on worker tasks (`sd_delete_task`); LVGL mutates only under `bsp_display_lock()`; progress crosses threads via `std::atomic` polled by the 1s refresh â€” the proven `c6_ota_progress_` pattern. Selection `vector<bool>` mutated only under the lock; refresh must NOT rebuild the list mid-selection. |
| **Chunked HTML size / Dashboard poll cost.** Tile-grid + nav on every page grows each response. | Low | Keep 1024-byte chunking; poll `/status` (15s) rebuilds tiles client-side from JSON, not full-page reloads. `no-store` on data endpoints (already convention). |
| **URI-handler cap (29/32 used).** Phase 3 adds 3 (preview+batch POST, status GET) = exactly 32. Day/night-persist or web C6-OTA would overflow. | **Med-High** | Phase 3 fits exactly. **Decide before Phase 4** whether to raise `max_uri_handlers` (`web_server.cpp:269`) for the day/night-persist route, or persist via an existing endpoint (e.g. fold into `/config` POST). Flagged as Q3. |
| **OTA / rollback safety.** A redesign regression could brick the field unit (device on ota95). | Med | Every phase ships behind the bumpâ†’buildâ†’OTAâ†’verify discipline; keep the app rollback-capable; verify `esp_ota` health-check still marks valid; never merge a phase that fails on-device smoke test. Touch + web are independently testable, so a bad web change doesn't block touch verify. |
| **Functional regression (lose a current feature).** | Med | The B.2 placement table is the regression checklist â€” WS-G audits every row present on both surfaces. Keep existing single `/files/delete`, rename, mkdir working (now routed through the guard). |
| **SDSPI contention.** Bulk delete competes with RINEX writes + web file serving on the single SPI host. | Med | `vTaskDelay(1)` pacing in `sd_delete_task`; single-job state machine (2nd start = web 409 / touch disabled); active-RINEX-file path is `skipped`, never `failed`. |

---

## 5. Verification per phase

Standard loop every phase (the release discipline from memory: **bump version â†’ `idf.ps1` build â†’ OTA to `192.168.8.186` â†’ verify â†’ next ota number**, device currently ota95):

| Phase | Build/Static | On-device (OTA â†’ 192.168.8.186) | Surface checks |
|---|---|---|---|
| **0 (tokens/theme)** | `idf.ps1` builds clean; `idf.py size` baseline captured; confirm `montserrat_18/28/48` linked. | Boot OK, no visual regression beyond recolor; touch still navigable. | Web: every existing page renders in the new palette (Chrome / preview MCP). Diff CSS once vs 3-themes-before. |
| **1 (SD backend)** | Builds; unit-style sanity: `check_deletable` rejects `/sdcard`, `..`, managed shells; `delete_recursive` clears non-empty dir. | Create a throwaway `/sdcard/test/` tree, bulk-delete via a temporary debug call; confirm managed-shell "empty-not-remove"; confirm active-RINEX file refusal. | n/a (no UI yet) â€” exercise via a scratch endpoint or serial. |
| **2 (shells)** | Builds; SRAM/flash delta recorded. | **On-device screenshot** of all 5 touch sections; tab targets â‰¥120px; hero readouts legible; survey arcs animate at 1Hz only. | **Web Chrome/preview**: nav present on every page, 5 sections in order, identical icons; Dashboard reflows 1â†’2(â‰ˆ720)â†’4 col; deep-links work. Cross-check section names/order touch-vs-web. |
| **3 (bulk delete)** | Builds; URI count = 32 (not over). | Touch: long-press â†’ checkboxes â†’ Delete(N) â†’ progress â†’ partial-failure summary; "I understand" friction; active-file non-selectable. | Web: Select mode, Select-all tri-state, typed-"DELETE", async progress poll, 409 on concurrent. Verify same strings/byte-humanizer both surfaces. |
| **4 (polish)** | Final `idf.py size` + free-heap report vs Phase-0 baseline; confirm within budget. | Day/night persists across reboot and **agrees on both surfaces**; full B.2 regression walk on-device. | Side-by-side web/touch screenshot consistency audit; OTA rollback dry-run. |

Verification tooling available: `idf.ps1` (build/flash/OTA per memory windows-build-deploy), Chrome MCP / Claude Preview for the web UI, on-device screenshot for touch, and the monitoring Pi (192.168.8.101) for serial capture during OTA if a crash needs catching.

---

## 6. Suggested worker breakdown (parallel agents, file ownership)

File ownership is the conflict-avoidance contract â€” **no two concurrently-running workers write the same file.**

| Wave | Worker | Owns (writes) | Reads-only | Depends on |
|---|---|---|---|---|
| **W0** | **Agent-Tokens** | `ui_sections.hpp` (new), `theme.hpp` (new), `lv_conf.h`, the `kAfterTitle` block in `web_server.cpp`, the palette block in `ui.cpp` | spec | â€” (must finish & merge first) |
| **W1** | **Agent-SD** | `sd_manager.hpp/.cpp`, the delete-task + guard in `base_station.hpp/.cpp` (or new `sd_delete.cpp`) | `rinex_logger.hpp` | W0 merged (uses shared strings) |
| **W1** | **Agent-Web-Shell** | `web_server.cpp` + `web_server.hpp` (shell+sections, **no delete endpoints yet**) | `ui_sections.hpp`, `base_station.hpp` | W0 merged |
| **W1** | **Agent-Touch-Shell** | `ui.cpp` + `ui.hpp` (shell+sections, **no delete UI yet**) | `ui_sections.hpp`, `theme.hpp`, `base_station.hpp` | W0 merged |
| **W2** | **Agent-Web-Delete** | `web_server.cpp/.hpp` (delete endpoints + selection UI) | `sd_manager.hpp`, `base_station.hpp` | W1 Agent-SD **and** Agent-Web-Shell merged |
| **W2** | **Agent-Touch-Delete** | `ui.cpp/.hpp` (selection UI) | `sd_manager.hpp`, `base_station.hpp` | W1 Agent-SD **and** Agent-Touch-Shell merged |
| **W3** | **Agent-Polish** | `storage.hpp/.cpp`, small touch-ups across `ui.cpp`/`web_server.cpp` | all | all merged |

**Dispatch order & conflict notes**
1. Dispatch **Agent-Tokens alone** (W0). Merge before anything else â€” it touches `ui.cpp` and `web_server.cpp` color regions that W1 agents also edit.
2. Then dispatch **Agent-SD, Agent-Web-Shell, Agent-Touch-Shell in parallel** (W1). They own disjoint files. Agent-SD's `base_station` edits are additive (new task/atomics) and do not overlap the shells' read-only use of `base_station.hpp`.
3. **Serialize the same-file handoff:** Agent-Web-Shell must merge before Agent-Web-Delete starts (both own `web_server.cpp`); same for the touch pair on `ui.cpp`. The two W2 agents run in parallel with each other (different files).
4. **Agent-Polish last**, solo, since it reaches into already-settled files.

The only files with sequential same-file contention are `web_server.cpp` (Shellâ†’Delete) and `ui.cpp` (Shellâ†’Delete); everything else parallelizes cleanly. If you'd rather avoid even that handoff, the delete endpoints/UI could be carved into `web_files_delete.cpp` / a touch helper TU â€” but that fragments cohesive code (Q4).

---

## 7. Open questions for the user

1. **Day/night default & auto-switch.** Spec sets **Night as default**. Manual toggle only, or also auto by a schedule/ambient cue? (No light sensor confirmed â€” please confirm none, else we could automate.)
2. **Dashboard Start/Restart Survey button.** Kept as the one action on an otherwise read-only Dashboard. Confirm you want a deliberate action there, or move it to Position-only.
3. **URI-handler budget.** Phase 3 lands exactly at 32/32. For day/night persistence (Phase 4) and any future webâ†”touch OTA parity, do you approve **raising `max_uri_handlers`** (`web_server.cpp:269`), or should day/night persist piggyback an existing POST (e.g. `/config`)? This decides Phase 4's route shape.
4. **`BaseStation` vs standalone module for the delete task + active-file guard.** Put the `sd_delete_task`/atomics + RINEX-file guard **inside `BaseStation`** (it already owns `rinex_logger_`), or in a new `sd_delete.cpp` to keep `BaseStation` lean? Same question for whether the web/touch delete code stays inline in `web_server.cpp`/`ui.cpp` or moves to dedicated TUs.
5. **Single-source color generation.** Acceptable to keep web `:root` hexes and touch `Theme` hexes hand-synced from one C.1 table (with a WS-G diff check), or do you want a generated `colors.def` both consume? (More robust, slightly more build machinery.)
6. **Non-empty-dir delete friction.** Web = type "DELETE"; touch = "I understand this is permanent" toggle. Confirm these surface-appropriate variants are acceptable (same intent, different input), per F.6.
7. **Scope of webâ†”touch OTA parity.** Spec deliberately does **not** add web C6-OTA or touch P4-OTA (they stay in identical IA slots but single-surface). Confirm that asymmetry is acceptable for this pass, or should parity be added (adds routes â†’ ties into Q3).
8. **Version bump cadence for a multi-phase rollout.** Each phase OTAs separately (ota96, ota97, â€¦) per your release discipline â€” confirm you want a deployable OTA at each phase boundary (recommended for rollback safety), vs batching phases before a single OTA.

---

**Bottom line:** WS-A (shared header + theme) is the one true blocker â€” land and merge it first. Then run SD-backend + web-shell + touch-shell in parallel, hang the two bulk-delete UIs off their shells once the backend merges, and finish with a polish/persistence/consistency-audit pass. The only same-file serialization is Shellâ†’Delete on `web_server.cpp` and `ui.cpp`; all other workstreams own disjoint files and parallelize cleanly. Every phase is independently OTA-verifiable to `192.168.8.186` under your existing bumpâ†’buildâ†’OTAâ†’verify discipline.
