#include "sd_delete.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "base_station.hpp"
#include "ui_sections.hpp"

namespace sd_delete {
namespace {

SdManager   *g_sd      = nullptr;
BaseStation *g_station = nullptr;

std::atomic<int>        g_state{static_cast<int>(State::kIdle)};
std::atomic<uint32_t>   g_done{0};
std::atomic<uint32_t>   g_total{0};
SdManager::DeleteResult g_result;     // published BEFORE the release-store of g_state
std::vector<std::string> g_paths;     // owned copy of the active job's path list

void delete_task(void *) {
    std::vector<const char *> ptrs;
    ptrs.reserve(g_paths.size());
    for (const std::string &p : g_paths) ptrs.push_back(p.c_str());

    SdManager::DeleteResult res =
        g_sd->delete_paths(ptrs.data(), ptrs.size(), &g_done);

    g_result = res;
    g_state.store(res.failed > 0 ? static_cast<int>(State::kDoneFail)
                                 : static_cast<int>(State::kDoneOk),
                  std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

void bind(SdManager *sd, BaseStation *station) {
    g_sd = sd;
    g_station = station;
}

SdManager::DeletePreview preview(const char *const *paths, size_t n) {
    if (!g_sd) return {};
    return g_sd->preview_delete_many(paths, n);
}

bool blocks_active_file(const char *const *paths, size_t n,
                        char *msg, size_t msg_len) {
    if (!g_station) return false;
    const RinexLogger::Status rs = g_station->rinex_status();
    if (!rs.active || rs.current_file.empty()) return false;
    const std::string &af = rs.current_file;
    for (size_t i = 0; i < n; ++i) {
        if (!paths[i]) continue;
        const std::string p = paths[i];
        const bool same   = (af == p);
        const bool inside = (af.size() > p.size() &&
                             af.compare(0, p.size(), p) == 0 &&
                             af[p.size()] == '/');
        if (same || inside) {
            if (msg && msg_len) {
                snprintf(msg, msg_len, "%s", ui_sections::del::kActiveFile);
            }
            return true;
        }
    }
    return false;
}

bool busy() {
    return g_state.load(std::memory_order_acquire) ==
           static_cast<int>(State::kRunning);
}

bool start(const char *const *paths, size_t n) {
    if (!g_sd || !g_sd->is_mounted()) return false;
    if (busy()) return false;
    char msg[160];
    if (blocks_active_file(paths, n, msg, sizeof(msg))) return false;

    g_paths.clear();
    g_paths.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (paths[i]) g_paths.emplace_back(paths[i]);
    }
    if (g_paths.empty()) return false;

    const SdManager::DeletePreview pv = g_sd->preview_delete_many(paths, n);
    g_total.store(pv.files + pv.dirs, std::memory_order_relaxed);
    g_done.store(0, std::memory_order_relaxed);
    g_result = SdManager::DeleteResult{};
    g_state.store(static_cast<int>(State::kRunning), std::memory_order_release);

    // ~8 KB stack: recursive delete + std::string/vector scratch on a shallow tree.
    if (xTaskCreate(delete_task, "sd_del", 8192, nullptr, 4, nullptr) != pdPASS) {
        g_state.store(static_cast<int>(State::kIdle), std::memory_order_release);
        return false;
    }
    return true;
}

Progress poll() {
    Progress p;
    p.state = static_cast<State>(g_state.load(std::memory_order_acquire));
    p.done  = g_done.load(std::memory_order_relaxed);
    p.total = g_total.load(std::memory_order_relaxed);
    if (p.state == State::kDoneOk || p.state == State::kDoneFail) {
        p.result = g_result;  // safe: published before the release-store of g_state
    }
    return p;
}

}  // namespace sd_delete
