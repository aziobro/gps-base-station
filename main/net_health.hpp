#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Periodic outbound-reachability probe, independent of WiFi association
// state. BaseStation::network_available_ only reflects whether the P4 is
// associated to an AP; during the 2026-07-03 storm that stayed true the
// whole time the WAN uplink itself was flaky, giving no way to tell
// "internet is down" from "our code is broken" short of after-the-fact
// forensic log analysis (TCP drop-counter slope, RST timing, backoff
// cadence). This probes a stable public IP directly on its own cadence so
// that state is visible live, in the log and in /status.
class NetHealth {
public:
    struct Status {
        bool up = false;
        uint32_t since_change_sec = 0;   // time spent in the current state
        uint32_t checked_sec_ago = 0;    // age of the last probe result
    };

    esp_err_t start();
    void stop();
    Status status() const;

private:
    static constexpr uint32_t kCheckIntervalMs = 20000;
    static constexpr int kConnectTimeoutSec = 4;
    // Google public DNS: stable, always-on, TCP/53 open. Probed by raw IP
    // (no getaddrinfo()) so a DNS outage can't masquerade as a WAN outage,
    // and so this can't be confused with the DNS-resolution caching already
    // used by NtripPushClient.
    static constexpr const char *kProbeIp = "8.8.8.8";
    static constexpr uint16_t kProbePort = 53;

    static void task_entry(void *argument);
    void run();
    bool probe() const;

    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> up_{false};
    std::atomic<int64_t> since_change_us_{0};
    std::atomic<int64_t> last_check_us_{0};
};
