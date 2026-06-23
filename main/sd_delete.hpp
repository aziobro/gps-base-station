#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sd_manager.hpp"

class BaseStation;

// ── Shared single-job bulk-delete executor for the SD card ───────────────────────
// Both UIs (touch + web) drive the SAME job through this module, so a delete can't
// run twice at once and the active-RINEX-file guard (which needs BaseStation
// visibility) lives in one place. Matches the log_buffer namespace-module style.
// The recursive delete itself is in SdManager; this owns the off-task worker, the
// progress atomics, and the guard. See docs/redesign/03_bulk_sd_management_design.md.
namespace sd_delete {

enum class State : int { kIdle = -1, kRunning = 0, kDoneOk = 1, kDoneFail = 2 };

struct Progress {
    State    state = State::kIdle;
    uint32_t done  = 0;
    uint32_t total = 0;
    SdManager::DeleteResult result;  // populated once state is kDoneOk / kDoneFail
};

// Wire up collaborators once at startup (app_main).
void bind(SdManager *sd, BaseStation *station);

// Recursive preview totals for the confirm dialog (delegates to SdManager).
SdManager::DeletePreview preview(const char *const *paths, size_t n);

// Active-RINEX-file guard: true if logging is active and the file being written is
// one of `paths` or lives inside a directory in `paths`. Fills `msg` with the shared
// refusal string (ui_sections::del::kActiveFile) when it returns true.
bool blocks_active_file(const char *const *paths, size_t n,
                        char *msg, size_t msg_len);

// Start a delete job (copies the path list). Returns false if a job is already
// running, SD is unmounted, the list is empty, or the active RINEX file is in scope
// (call blocks_active_file first if you want the precise message). Spawns the worker.
bool start(const char *const *paths, size_t n);

// Lock-free snapshot of the current job (reads atomics).
Progress poll();

// True while a job is running.
bool busy();

}  // namespace sd_delete
