Both facts confirmed: `safe_path` is a validator-only (returns the same pointer), `delete_entry` does `rmdir`/`unlink` with no recursion, the RINEX active file is at `RinexLogger::Status::current_file`, and the mount root is `/sdcard` with managed dirs `/sdcard/logs` and `/sdcard/rawdata`. Here is the design.

---

# Bulk SD-Card Management â€” End-to-End Design

Marine-instrument aesthetic, unified IA, identical safety semantics on web and touch. This is a design pass only; no files were modified.

---

## 0. Design summary (the shape of the feature)

A single backend capability â€” **validated, off-task, recursive, protected, progress-reporting bulk delete** â€” exposed identically through two renderers. Both surfaces share:

- **One selection model**: enter "selection mode", multi-select rows (checkboxes), select-all, with a count + size readout.
- **Two destructive actions**: **Delete selected** (N paths) and **Delete folderâ€¦** (recursive, extra friction).
- **One confirm dialog contract**: names exactly what is affected, shows **count + total size + irreversibility**, requires explicit confirm, adds extra friction for non-empty directories.
- **One progress model**: long deletes run on a worker, report `deleted/total` via an atomic, both surfaces poll the same counter.
- **One protected-path guard** enforced *inside* `SdManager` so neither surface can bypass it, plus an **active-RINEX-file guard** enforced at the caller layer (where `BaseStation` visibility exists).

---

## 1. Backend API plan (`SdManager` + caller-layer guard)

### 1.1 New / changed data types

```cpp
// sd_manager.hpp â€” additions

// Result of a delete batch. POD, no exceptions.
struct DeleteResult {
    uint32_t requested   = 0;   // paths/entries the caller asked to delete
    uint32_t deleted     = 0;   // files+dirs actually removed
    uint32_t failed      = 0;   // entries that could not be removed
    uint32_t skipped     = 0;   // protected / rejected-by-guard entries
    char     first_error[96] = {0}; // first failing path + errno text; "" if none
};

// Preview totals for a confirmation dialog (recursive over a dir, or summed over a set).
struct DeletePreview {
    bool     ok       = false;  // false => could not stat / not mounted / bad path
    uint32_t files    = 0;      // regular files that would be deleted
    uint32_t dirs     = 0;      // subdirectories that would be removed
    uint64_t bytes    = 0;      // total payload bytes
    bool     truncated = false; // walk stopped at kPreviewWalkCap (very large tree)
};

// Reason a path is refused, surfaced to UIs for precise messaging.
enum class DeleteGuard : uint8_t {
    kAllowed = 0,
    kBadPath,        // safe_path() failed (empty / not under /sdcard / contains "..")
    kMountRoot,      // path == /sdcard
    kManagedDir,     // path is /sdcard/logs or /sdcard/rawdata (the dir itself)
    kNotMounted,
};
```

### 1.2 New / changed methods

```cpp
// ---- Protected-path guard (NEW, the security core) ----
// Pure validator. Re-runs safe_path(), then checks the deny-list.
// Static + const-correct; callable from any layer for pre-flight checks.
static DeleteGuard check_deletable(const char *path);

// Returns true only if check_deletable(path)==kAllowed AND the entry exists.
bool is_deletable(const char *path) const;

// ---- Preview (NEW, feeds the confirm dialog) ----
// Recursively walks `path` (file or dir) WITHOUT deleting. Bounded by
// kPreviewWalkCap entries so it can run inline on the web/UI thread for
// normal dirs; sets truncated=true and returns partial counts past the cap.
DeletePreview preview_delete(const char *path) const;

// Sum a preview across many paths (multi-select). Stops at the cap in aggregate.
DeletePreview preview_delete_many(const char *const *paths, size_t n) const;

// ---- Recursive single-path delete (NEW) ----
// Depth-first: unlink files, rmdir on the way up. Re-validates check_deletable()
// on root AND on every descendant directory before recursing into it.
// Optional progress: increments *progress once per removed entry (atomic, may be null).
// Continue-on-error: a failed child does NOT abort siblings.
DeleteResult delete_recursive(const char *path,
                              std::atomic<uint32_t> *progress = nullptr) const;

// ---- Batch multi-path delete (NEW, the primary bulk entry point) ----
// PRE-VALIDATION IS ATOMIC: if ANY path fails check_deletable(), the whole batch
// is rejected (DeleteResult.skipped=n, deleted=0) and nothing is touched.
// After validation passes, deletes each entry best-effort (files via unlink,
// dirs via delete_recursive), continue-on-error, accumulating counts.
// progress increments per removed entry across the whole batch.
DeleteResult delete_paths(const char *const *paths, size_t n,
                          std::atomic<uint32_t> *progress = nullptr) const;

// ---- Constants (NEW) ----
static constexpr size_t kPreviewWalkCap = 4096;  // entries; cap inline walk cost
```

`delete_entry()` (existing single-path, file-or-empty-dir) **stays** for the existing web single-item path and is internally re-pointed to call `check_deletable()` first so even the legacy path can't delete a protected entry.

### 1.3 Protected paths (deny-list, enforced inside `check_deletable`)

Hard-coded, never deletable by either surface:

| Path | Rule | Rationale |
|---|---|---|
| `/sdcard` | **Never** (mount root) | Deleting the mount root is meaningless/destructive to the FS handle. |
| `/sdcard/logs` | The directory itself is **never** removable | Managed by `ensure_dirs()`; app expects it to exist. Contents *are* deletable. |
| `/sdcard/rawdata` | The directory itself is **never** removable | Holds RINEX; recreated by `ensure_dirs()`. Contents *are* deletable. |
| anything failing `safe_path()` | Rejected (`kBadPath`) | empty, not under `/sdcard`, or contains `".."`. |

**Decision (locked with the "delete entire directory" requirement):** "Delete folder" on `logs/` or `rawdata/` means **empty the directory** (delete all contents recursively) but **keep the directory shell**. This satisfies "delete an entire directory" while keeping the app's invariants. The UI presents this as "Empty rawdata/ â€” 214 files, 1.7 GB" for those two managed dirs, and "Delete folder X â€” â€¦" for any user-created subdirectory (which *is* fully removed). User-created subdirectories under `/sdcard` (e.g. `/sdcard/myfolder`) are fully deletable, shell included.

Implementation note: `delete_recursive("/sdcard/rawdata")` detects the managed-dir case, recurses to empty contents, and skips the final `rmdir` of the protected root (returns `skipped` for the root, `deleted` for contents). For non-managed dirs it removes the shell too.

### 1.4 Active-RINEX-file guard (caller layer, NOT in `SdManager`)

`SdManager` has no visibility into `BaseStation`/`RinexLogger`, so the open-file guard lives in the web handler and the touch UI (both already hold `BaseStation*`):

- Before any delete (single, batch, or folder), the caller fetches `station_->rinex_status()`.
- If `active == true`, compute `current_file` (full path under `/sdcard/rawdata/`). **Reject** the operation if the batch/folder *contains* that path. Specifically:
  - Multi-select batch containing the active file â†’ reject the whole batch with a precise message ("Cannot delete â€” file is being written by RINEX logging. Stop logging first.").
  - "Empty rawdata/" while logging active â†’ reject with the same message and offer "Stop logging" inline (touch: opens System; web: links to the RINEX toggle).
- This check is **pre-flight** (before enqueuing the worker) and **re-checked** by the worker immediately before it touches each path (logging could start mid-delete); a path matching the now-active file is `skipped`, not `failed`.

### 1.5 Error handling, atomicity, partial failure

- **No exceptions / no RTTI** â€” all paths return POD result structs and bools; errno captured into `first_error`.
- **Validation is atomic; deletion is not.** `delete_paths` validates *all* inputs first and rejects the entire batch on any bad/protected path (so a single crafted path can't slip a partial destructive run). Once validation passes, deletion is **best-effort continue-on-error**: one unremovable file (e.g. read error) does not stop the rest. Per-item outcome is summarized in `DeleteResult` (`deleted`/`failed`/`skipped` + `first_error`).
- **Path-traversal hardening for batch**: every element runs through `check_deletable()` (which re-runs `safe_path()`); `delete_recursive` re-validates each descendant directory path before recursing (defense in depth â€” FATFS has no symlinks, but the `..` guard and the `/sdcard` prefix check are re-applied per level).
- **Post-op truth**: after any delete the caller **re-lists the directory** and **re-reads `disk_stats()`** so the UI reflects reality regardless of partial failure.

### 1.6 Off-task execution + progress

Recursive delete on SDSPI is slow and **must not run on the LVGL or httpd task** (watchdog + blocks rendering/serving).

- A single dedicated worker task `sd_delete_task` (prio 4, ~4096 stack) owns the active delete job. A small job struct holds: the path list (copied), an `std::atomic<int> sd_delete_state_` (`-1` idle / `0` running / `1` done-ok / `2` done-with-failures), `std::atomic<uint32_t> sd_delete_progress_` (entries removed), and `std::atomic<uint32_t> sd_delete_total_` (set from the preview count before start).
- This mirrors the existing C6-OTA progress pattern (`std::atomic<int>` polled by `refresh()` / a status endpoint).
- **Threshold:** small operations (preview `files+dirs â‰¤ kInlineDeleteMax`, e.g. 16) delete inline (single-shot, no worker) for snappy UX; larger ones spawn the worker and show progress.
- Only one delete job at a time (state machine refuses a second start while running).

---

## 2. Web UX

### 2.1 New IA placement

File browser moves under the unified **Storage** section (`/files` retained as the route). Top of page keeps the **disk-usage arc/bar** (retheme to marine tokens). The browser gains a selection toolbar.

### 2.2 Browser states

**Normal mode** (default): rows as today (icon Â· name Â· size Â· per-row actions dl / rn / del), plus a new **"Select"** button in the toolbar.

**Selection mode** (toggled by "Select", or by clicking a row's new leading checkbox):
- Each row shows a **leading checkbox** (44px hit target). Directory rows are selectable too.
- Toolbar shows: **Select all** (header checkbox, tri-state), a live **"N selected Â· 1.7 GB"** counter, **Delete (N)** button (`--crit`, disabled when N=0), and **Cancel** (exits selection mode).
- Each directory row additionally has a **"Delete folderâ€¦"** affordance (always available, even outside selection mode) for whole-directory recursive delete.
- The selected-size counter is computed via a debounced call to `/files/preview` (so very large/dir selections don't block the UI), falling back to summing known file sizes for file-only selections.

### 2.3 Confirm dialog (web)

A themed modal (component 4.8) over a 60% scrim. Two variants, identical wording to touch:

**Delete selected (files / mixed):**
> **Delete 7 items?**
> 7 files Â· 24.3 MB
> This permanently deletes them from the SD card. **This cannot be undone.**
> [ Cancel ]  [ Delete 7 ]

**Delete folder / non-empty directory (extra friction):**
> **Delete folder "2024-09"?**
> 214 files Â· 3 subfolders Â· 1.7 GB
> Everything inside this folder will be permanently erased. **This cannot be undone.**
> Type **DELETE** to confirm: [ _______ ]
> [ Cancel ]  [ Delete folder ]  â† disabled until the text matches

- For managed dirs the title reads **"Empty folder "rawdata"?"** with a note "The folder itself is kept."
- The **Delete** button is `--crit` filled and is **never** the default focus; Enter does not trigger it. The typed-confirm gate applies whenever the target (folder or selection) contains a non-empty directory.
- If the active RINEX file is in scope, the dialog is replaced by a **blocking notice** ("RINEX logging is writing rawdata/â€¦. Stop logging in System before deleting.") with a link, not a confirm.

### 2.4 New / changed endpoints (request/response contract)

All require `authorize()`, `Cache-Control: no-store`, register in `register_secure_handlers()` (raise `max_uri_handlers` from 32 since only ~3 slots remain and we add 3 routes).

| Route | Method | Body (JSON) | Response | Notes |
|---|---|---|---|---|
| `/files/preview` | POST | `{"paths":["/sdcard/.."]}` or `{"path":"/sdcard/dir"}` | `{"ok":bool,"files":n,"dirs":n,"bytes":n,"truncated":bool}` | Feeds the confirm dialog count+size. Inline, bounded by `kPreviewWalkCap`. Body cap 4096 (multi-path). |
| `/files/delete-batch` | POST | `{"paths":["/sdcard/a","/sdcard/b/"]}` | small set â†’ `{"ok":bool,"deleted":n,"failed":n,"skipped":n,"error":"â€¦","async":false}`; large set â†’ `{"ok":true,"async":true,"total":n}` | Pre-validates atomically; rejects whole batch with `{"ok":false,"skipped":n,"error":"â€¦"}` on any protected/bad path. Active-RINEX pre-check here. Body cap 4096. |
| `/files/delete-status` | GET | â€” | `{"state":"idle\|running\|done\|failed","progress":n,"total":n,"deleted":n,"failed":n,"skipped":n,"error":"â€¦"}` | Polled at 1 s while a job is async. |

- The existing single-item `POST /files/delete` is **kept** for the per-row "del" button (now also guarded server-side).
- **JSON note:** the current `json_field` only handles flat `"k":"v"` strings and cannot parse a `paths` array. The handler needs a minimal array extractor (split on `","` between `[`â€¦`]`, url/JSON-unescape each, validate each with `safe_path`) â€” kept small and defensive. Reject if element count exceeds a sane cap (e.g. 256) or any element > 256 chars.
- **Pacing:** the worker honors the same SDSPI discipline (yield/`vTaskDelay(1)` periodically) so it can't starve the server.
- After a successful delete (sync or after polling reports `done`), the JS re-runs `loadDir(path)` and refetches the storage-usage bar (re-reads `disk_stats()` server-side).

### 2.5 Web JS behavior

- `enterSelect()/exitSelect()` toggle a body class; `toggleRow`, `toggleAll` maintain a `Set` of selected paths and the live counter.
- `confirmAndDelete(paths|folder)` â†’ POST `/files/preview` â†’ render dialog with returned count/size â†’ on confirm POST `/files/delete-batch` â†’ if `async`, poll `/files/delete-status` every 1 s, drive the progress bar (component 4.7), then refresh.
- All vanilla JS, no libraries; reuses the shared themed dialog markup.

---

## 3. Touchscreen UX (LVGL 9.x, 720Ã—720)

Built into the existing **SD Card file browser modal** (`build_file_browser` / `refresh_file_browser`). All object mutation under `bsp_display_lock()`; the recursive delete runs on `sd_delete_task`, never inline.

### 3.1 Entering selection mode

Two finger-friendly entry points (both retained for discoverability):
- **Long-press any row** (`LV_EVENT_LONG_PRESSED` on the list button) â†’ enters selection mode and selects that row. (LVGL fires long-press natively; no SDIO/locking concern.)
- A **"Select" button** in the path bar (next to "Up") â†’ enters selection mode with nothing selected.

In selection mode each list row shows a **leading checkbox** (an `lv_checkbox`/styled state, â‰¥44px hit area inside the 64px row). Tap toggles selection (no navigation while in selection mode). Selected rows get `--surface-hi` background + `--accent` check (redundant cue: bg + check glyph).

### 3.2 Selection action bar

A bar slides up above the bottom tab area (within the modal), 64px tall:
- Left: **"N selected Â· 1.7 GB"** (`value-sm` + dim size; size from inline `preview_delete_many` when bounded, else "â€¦").
- **"Select all"** toggle (selects every entry in the current dir).
- **"Delete (N)"** button, `--crit` filled, â‰¥64px, disabled at N=0.
- **"Cancel"** (neutral) exits selection mode and clears the set.

Directory rows also expose **"Delete folderâ€¦"** via a trailing trash glyph on the row (works outside selection mode too), so whole-directory recursive delete is one tap â†’ confirm.

Selection state is a `std::vector<bool>` (or `std::set<int>`) keyed to `fb_entries_`, mutated only under the display lock and rebuilt whenever the directory is re-listed.

### 3.3 LVGL confirm dialog

Use the existing `lv_msgbox` pattern (as survey-restart / C6-OTA confirms), width 480, centered, on `lv_layer_top()`, with the marine theme applied. Same wording as web:

- **Title:** "Delete 7 items?" / "Delete folder "2024-09"?" / "Empty folder "rawdata"?"
- **Body:** "7 files Â· 24.3 MB â€” This cannot be undone." (folder variant adds subfolder count + bytes and "Everything inside is permanently erased.")
- **Footer buttons:** **Cancel** (neutral, default) Â· **Delete** (`--crit`). Delete is **not** the default; first focus is Cancel.
- **Extra friction for non-empty dirs:** since the on-screen keyboard pattern already exists (`kb_*`), the folder-delete confirm requires the operator to **toggle a "I understand this is permanent" switch** before the Delete button enables (a switch is more finger-appropriate than typing "DELETE" on a 720px panel; web uses typed-confirm, touch uses the explicit toggle â€” *same intent, surface-appropriate friction*, and the wording is identical).
- **Active RINEX file in scope:** the confirm is replaced by an info msgbox: "Cannot delete â€” RINEX logging is writing this file. Stop logging in System first." with an "OK" button (no destructive option).

### 3.4 Progress + execution

- On confirm: compute total via `preview_*` (already have it from the dialog), set `sd_delete_total_`, `sd_delete_state_=0`, spawn `sd_delete_task` (prio 4) with a copied path list.
- The msgbox transforms into a **progress view**: progress bar (component 4.7) + "Deleting 137 / 214â€¦" driven by the 1 s `refresh()` tick reading `sd_delete_progress_`/`sd_delete_total_` (atomics), exactly like the C6-OTA progress readout. The Delete/Cancel buttons are replaced by a disabled state until done.
- On completion (`state==1/2`): show "Deleted N files" (or "Deleted N Â· M failed" in `--warn`), then `refresh_file_browser()` re-lists the dir and the System/Storage disk readout refreshes from `disk_stats()` on the next tick.
- **No SDIO/RPC, no FS recursion on the LVGL task** â€” the worker does all FS work; the LVGL task only reads atomics under the lock it already holds.

### 3.5 Existing-pattern reuse

- Modal/title-bar/close-button: `make_modal_base` conventions.
- Confirm: `lv_msgbox` + footer buttons tagged with `Ui*` (as survey/OTA confirms).
- Cross-thread state: `std::atomic` (as `c6_ota_progress_`, `scan_running_`).
- Worker spawn: same pattern as `wifi_scan_task` / `c6_ota_task` (re-acquire `bsp_display_lock` before touching LVGL on completion).

---

## 4. Consistency (identical across both surfaces)

### 4.1 Wording (verbatim, shared string constants)

| Context | Exact text |
|---|---|
| Enter selection | **Select** |
| Counter | **N selected Â· {size}** (e.g. "7 selected Â· 24.3 MB") |
| Primary destructive (batch) | **Delete (N)** |
| Folder action | **Delete folderâ€¦** |
| Managed-dir action | **Empty folderâ€¦** |
| Confirm title (items) | **Delete N items?** |
| Confirm title (folder) | **Delete folder "{name}"?** |
| Confirm title (managed) | **Empty folder "{name}"?** |
| Confirm body (items) | **{n} files Â· {size} â€” This cannot be undone.** |
| Confirm body (folder) | **{f} files Â· {d} subfolders Â· {size} â€” Everything inside is permanently erased. This cannot be undone.** |
| Managed-dir note | **The folder itself is kept.** |
| Confirm buttons | **Cancel** / **Delete** (folder: **Delete folder**) |
| Active-file block | **Cannot delete â€” RINEX logging is writing this file. Stop logging in System first.** |
| Partial failure | **Deleted {n}, {m} failed.** |
| Progress | **Deleting {done} / {total}â€¦** |

Size formatting uses the *same* humanizer logic on both (`fmt_bytes_str` on touch / `human_bytes` on web â€” both render `GB/MB/KB/B`, 1 decimal for GB/MB). Counts use "file/files", "subfolder/subfolders" with correct singular/plural.

### 4.2 Iconography (shared semantic set)

| Meaning | Touch (LVGL symbol) | Web (inline SVG, `currentColor`) |
|---|---|---|
| Folder | `LV_SYMBOL_DIRECTORY` (ðŸ“) | folder glyph |
| File | `LV_SYMBOL_FILE` (ðŸ“„) | file glyph |
| Delete | `LV_SYMBOL_TRASH` | trash glyph |
| Selected | check in box (`--accent`) | check in box (`--accent`) |
| Warning (in confirm) | `LV_SYMBOL_WARNING` (`--crit`) | warning triangle (`--crit`) |

### 4.3 Safety semantics (identical)

1. Same protected paths (`/sdcard`, `/sdcard/logs`, `/sdcard/rawdata` shells) â€” enforced in `SdManager`, so both surfaces are guarded by the same code.
2. Same active-RINEX-file refusal, same message.
3. Confirm always shows **count + total size + "This cannot be undone."**
4. **Extra friction for non-empty directories**: web = type "DELETE"; touch = "I understand" toggle. Same gate, surface-appropriate input.
5. Destructive button is `--crit`, never default-focused, on both.
6. Same continue-on-error semantics and same partial-failure summary text.

---

## 5. Edge cases

| Case | Behavior |
|---|---|
| **Deleting the active RINEX log** | Pre-flight + per-path re-check refuses any batch/folder containing `rinex_status().current_file` while `active`. Surfaced as a block notice with "Stop logging in System" path; that file is `skipped` (not `failed`) if logging starts mid-job. |
| **Large directory (responsiveness)** | Operations above `kInlineDeleteMax` run on `sd_delete_task` with `vTaskDelay(1)` pacing; both surfaces poll an atomic progress counter and show a progress bar. LVGL/httpd threads never recurse the FS. Preview walk is bounded by `kPreviewWalkCap` (4096) and reports `truncated:true` â†’ dialog shows "â‰¥4096 files" so a giant tree doesn't hang the preview. |
| **Read errors mid-delete** | Continue-on-error: failed entry increments `failed`, captured in `first_error`; siblings proceed. Final summary: "Deleted N, M failed." (`--warn`). Dir re-listed so the UI shows what actually remains. |
| **SD not mounted** | All endpoints/handlers check `is_mounted()` first. Web: `503`/`{"ok":false,"error":"SD not mounted"}`. Touch: action bar disabled, browser shows "Not mounted"; `check_deletable` returns `kNotMounted`. |
| **Free-space refresh after delete** | On completion both surfaces re-read `disk_stats()` and re-list the current directory. Web refetches the storage-usage bar; touch updates the Storage disk arc on the next 1 s `refresh()` tick. Position read is *not* re-triggered (avoid extra NVS reads). |
| **Path-traversal / crafted batch** | Every element validated via `check_deletable()` (re-runs `safe_path()`); any bad/protected element rejects the **entire** batch (atomic validation) â€” nothing deleted, `skipped` reported, precise error. |
| **Selection invalidated by refresh** | While in selection mode on touch, periodic `refresh()` does **not** rebuild the file list (only the browser modal's own refresh does); selection set is rebuilt on explicit navigation/`refresh_file_browser`. Web only re-lists after a delete or manual navigation, so selections aren't silently dropped. |
| **Concurrent delete attempts** | Single-job state machine: a second start while `state==running` is refused (web: `409`/`{"ok":false,"error":"delete in progress"}`; touch: Delete button disabled during progress). |
| **Delete folder == empty SD root contents?** | The mount root `/sdcard` is never deletable; there is no "delete everything" shortcut â€” operators must select children or empty managed dirs. This is intentional friction. |
| **Empty directory delete** | A user-created empty subdir deletes via `rmdir` (fast, inline). Managed empty dirs are still protected (kept). |

---

## Implementer anchors (no files changed)

- **Backend:** add the methods/structs in Â§1 to `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\sd_manager.hpp` / `sd_manager.cpp`. Re-point existing `delete_entry()` through `check_deletable()`. Protected roots: `kMountPoint` (`/sdcard`) + the two dirs created by `ensure_dirs()` (`/sdcard/logs`, `/sdcard/rawdata`).
- **Active-file guard:** at caller layer using `BaseStation::rinex_status().current_file` / `.active` (`base_station.hpp:61`, `rinex_logger.hpp:20`).
- **Web:** add `/files/preview`, `/files/delete-batch`, `/files/delete-status` handlers + declarations in `web_server.hpp`, register in `register_secure_handlers()` (raise `max_uri_handlers`), add a small JSON-array extractor (current `json_field` is flat-string only). Retheme `/files` per the marine token block.
- **Touch:** extend `build_file_browser` / `refresh_file_browser` / `on_fb_item` in `ui.cpp` with selection mode, action bar, `lv_msgbox` confirm, and `sd_delete_task` (mirror `c6_ota_task` + `c6_ota_progress_` atomics). Mutate LVGL only under `bsp_display_lock()`.
