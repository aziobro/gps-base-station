#include "log_buffer.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "esp_log.h"

namespace log_buffer {
namespace {

// ~16 KB of recent console output. Large enough to span the boot sequence plus
// a few minutes of steady-state logging; old bytes are overwritten in place.
constexpr size_t kCapacity = 16384;

char g_ring[kCapacity];
size_t g_head = 0;    // index of the next byte to write
size_t g_count = 0;   // bytes currently stored (<= kCapacity)
uint64_t g_total = 0; // absolute byte cursor for incremental readers
std::mutex g_mutex;
vprintf_like_t g_previous = nullptr;

void append(const char *data, size_t length) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < length; ++i) {
        g_ring[g_head] = data[i];
        g_head = (g_head + 1) % kCapacity;
        if (g_count < kCapacity) ++g_count;
        ++g_total;
    }
}

int hook(const char *format, va_list args) {
    char line[256];
    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (written > 0) {
        append(line, std::min(static_cast<size_t>(written), sizeof(line) - 1));
    }
    // Forward to the original logger so the serial console is unaffected.
    return g_previous ? g_previous(format, args) : vprintf(format, args);
}

}  // namespace

void init() {
    g_previous = esp_log_set_vprintf(hook);
}

std::string snapshot() {
    return snapshot_since(0).text;
}

Snapshot snapshot_since(uint64_t since) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Snapshot snap;
    snap.next = g_total;
    if (g_count == 0 || since == g_total) return snap;

    const uint64_t oldest = g_total - g_count;
    uint64_t start = since;
    if (start == 0 || start < oldest) {
        snap.truncated = since != 0;
        start = oldest;
    } else if (start > g_total) {
        start = g_total;
    }

    const size_t bytes = static_cast<size_t>(g_total - start);
    if (bytes == 0) return snap;

    const size_t ring_start = (g_count == kCapacity) ? g_head : 0;
    const size_t offset = static_cast<size_t>(start - oldest);
    snap.text.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        snap.text.push_back(g_ring[(ring_start + offset + i) % kCapacity]);
    }
    return snap;
}

}  // namespace log_buffer
