#include "net_health.hpp"

#include <cerrno>
#include <fcntl.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

namespace {
constexpr char kTag[] = "net_health";
}  // namespace

esp_err_t NetHealth::start() {
    stopping_ = false;
    return xTaskCreate(task_entry, "net_health", 3072, this, 3, &task_) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void NetHealth::stop() { stopping_ = true; }

void NetHealth::task_entry(void *argument) {
    static_cast<NetHealth *>(argument)->run();
    vTaskDelete(nullptr);
}

bool NetHealth::probe() const {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kProbePort);
    if (inet_pton(AF_INET, kProbeIp, &addr.sin_addr) != 1) return false;

    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int result = connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    bool ok = false;
    if (result == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);
        timeval timeout{kConnectTimeoutSec, 0};
        result = select(sock + 1, nullptr, &write_set, nullptr, &timeout);
        int error = 0;
        socklen_t error_length = sizeof(error);
        ok = result > 0 &&
             getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
             error == 0;
    }
    close(sock);
    return ok;
}

void NetHealth::run() {
    since_change_us_ = esp_timer_get_time();
    while (!stopping_) {
        const bool result = probe();
        const int64_t now = esp_timer_get_time();
        last_check_us_ = now;
        const bool was_up = up_.exchange(result);
        if (result != was_up) {
            const int64_t state_duration_us = now - since_change_us_.exchange(now);
            ESP_LOGW(kTag, "internet %s (was %s for %llds)",
                     result ? "UP" : "DOWN", was_up ? "up" : "down",
                     static_cast<long long>(state_duration_us / 1000000));
        }
        vTaskDelay(pdMS_TO_TICKS(kCheckIntervalMs));
    }
}

NetHealth::Status NetHealth::status() const {
    const int64_t now = esp_timer_get_time();
    Status s;
    s.up = up_;
    s.since_change_sec =
        static_cast<uint32_t>((now - since_change_us_.load()) / 1000000);
    s.checked_sec_ago =
        static_cast<uint32_t>((now - last_check_us_.load()) / 1000000);
    return s;
}
