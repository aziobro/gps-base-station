#include "ntrip_push.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <vector>

#include "esp_log.h"
#include "lwip/tcp.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

namespace {

constexpr char kTag[] = "ntrip_push";
constexpr int kBaseRetryMs = 10000;
constexpr int kMaxRetryMs = 120000;

}  // namespace

NtripPushClient::NtripPushClient(
    const char *label, const char *host, uint16_t port,
    NtripProtocol protocol)
    : label_(label), host_(host), port_(port), protocol_(protocol) {}

NtripPushClient::~NtripPushClient() {
    stop();
}

esp_err_t NtripPushClient::start(
    bool enabled, const std::string &mountpoint,
    const std::string &password) {
    configure(enabled, mountpoint, password);
    queue_ = xQueueCreate(kQueueDepth, sizeof(Packet));
    if (!queue_) return ESP_ERR_NO_MEM;
    // Priority 3: below httpd (5) and base_station (6) so NTRIP reconnects
    // never starve the web server. Not pinned so the scheduler can balance
    // across both cores freely.
    if (xTaskCreate(task_entry, label_, 6144, this, 3, &task_) != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void NtripPushClient::stop() {
    if (!task_) return;
    stopping_ = true;
    reconnect_ = true;
    while (task_) vTaskDelay(pdMS_TO_TICKS(10));
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

void NtripPushClient::configure(
    bool enabled, const std::string &mountpoint,
    const std::string &password) {
    {
        std::lock_guard lock(config_mutex_);
        enabled_ = enabled && !mountpoint.empty();
        mountpoint_ = mountpoint;
        password_ = password;
        message_ = enabled_ ? "waiting" : "disabled";
    }
    reconnect_ = true;
}

void NtripPushClient::set_suspended(bool suspended) {
    suspended_ = suspended;
    if (suspended) reconnect_ = true;
}

void NtripPushClient::push(const uint8_t *data, size_t length) {
    if (!data || length == 0 || !queue_ || suspended_ || !connected_) return;
    Packet packet{};
    packet.length = std::min(length, kMaxPacket);
    memcpy(packet.data, data, packet.length);
    if (xQueueSend(queue_, &packet, 0) != pdTRUE) {
        ++dropped_batches_;
        reconnect_ = true;
    }
}

NtripStatus NtripPushClient::status() const {
    std::lock_guard lock(config_mutex_);
    return {
        enabled_,
        connected_,
        message_,
        bytes_sent_,
        dropped_batches_,
    };
}

void NtripPushClient::task_entry(void *argument) {
    auto *client = static_cast<NtripPushClient *>(argument);
    client->run();
    client->task_ = nullptr;
    vTaskDelete(nullptr);
}

void NtripPushClient::run() {
    Packet packet{};
    int failures = 0;
    while (!stopping_) {
        bool enabled = false;
        {
            std::lock_guard lock(config_mutex_);
            enabled = enabled_;
        }
        if (suspended_ || !enabled) {
            close_socket();
            if (queue_) xQueueReset(queue_);
            set_message(enabled ? "suspended" : "disabled", false);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (reconnect_.exchange(false)) {
            close_socket();
            if (queue_) xQueueReset(queue_);
        }
        if (socket_ < 0) {
            set_message("connecting", false);
            if (!connect_caster()) {
                ++failures;
                // After several consecutive failures invalidate the cached
                // address so the next attempt forces a fresh DNS lookup.
                if (failures >= 3) cached_addr_valid_ = false;
                const int retry = std::min(
                    kBaseRetryMs << std::min(failures, 3), kMaxRetryMs);
                vTaskDelay(pdMS_TO_TICKS(retry));
                continue;
            }
            failures = 0;
        }
        if (xQueueReceive(queue_, &packet, pdMS_TO_TICKS(1000)) == pdTRUE &&
            !send_all(packet.data, packet.length)) {
            set_message("write failed; retrying", false);
            close_socket();
        }
    }
    close_socket();
}

bool NtripPushClient::connect_caster() {
    std::string mountpoint;
    std::string password;
    {
        std::lock_guard lock(config_mutex_);
        mountpoint = mountpoint_;
        password = password_;
    }
    if (!connect_socket()) {
        set_message("TCP connect failed", false);
        return false;
    }

    std::string request;
    if (protocol_ == NtripProtocol::kV1) {
        request = "SOURCE " + password + " /" + mountpoint + "\r\n"
                  "Source-Agent: NTRIP ESP32BaseStation/2.0\r\n\r\n";
    } else {
        request = "POST /" + mountpoint + " HTTP/1.1\r\n"
                  "Host: " + std::string(host_) + ":" +
                  std::to_string(port_) + "\r\n"
                  "Ntrip-Version: Ntrip/2.0\r\n"
                  "User-Agent: NTRIP ESP32BaseStation/2.0\r\n"
                  "Authorization: Basic " +
                  base64(mountpoint + ":" + password) + "\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Connection: keep-alive\r\n\r\n";
    }
    if (!send_all(
            reinterpret_cast<const uint8_t *>(request.data()), request.size())) {
        set_message("handshake write failed", false);
        close_socket();
        return false;
    }
    const std::string response = read_line(4000);
    const bool accepted =
        response.rfind("ICY 200", 0) == 0 ||
        response.rfind("HTTP/1.1 200", 0) == 0 ||
        response.rfind("HTTP/1.0 200", 0) == 0;
    if (!accepted) {
        set_message(response.empty() ? "empty response" : "rejected: " + response,
                    false);
        close_socket();
        return false;
    }
    if (response.rfind("HTTP/", 0) == 0) drain_headers();
    set_message(
        protocol_ == NtripProtocol::kV2 ? "connected (v2)" : "connected (v1)",
        true);
    ESP_LOGI(kTag, "%s connected", label_);
    return true;
}

bool NtripPushClient::connect_socket() {
    // Re-resolve DNS only when we have no cached address or after repeated
    // failures. getaddrinfo() holds the lwIP mutex for up to ~2 s which stalls
    // TLS handshakes on the HTTPS server, so we avoid calling it every reconnect.
    if (!cached_addr_valid_) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *results = nullptr;
        const std::string service = std::to_string(port_);
        if (getaddrinfo(host_, service.c_str(), &hints, &results) != 0 ||
            !results) {
            return false;
        }
        memcpy(&cached_addr_, results->ai_addr,
               std::min(sizeof(cached_addr_),
                        static_cast<size_t>(results->ai_addrlen)));
        cached_addr_.sin_port = htons(port_);
        cached_addr_valid_ = true;
        freeaddrinfo(results);
    }

    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ < 0) {
        cached_addr_valid_ = false;  // force fresh resolve next attempt
        return false;
    }

    const int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
    int result = connect(socket_,
                         reinterpret_cast<sockaddr *>(&cached_addr_),
                         sizeof(cached_addr_));
    if (result < 0 && errno != EINPROGRESS) {
        close_socket();
        cached_addr_valid_ = false;  // may be a stale address; re-resolve next time
        return false;
    }
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket_, &write_set);
    timeval timeout{4, 0};
    result = select(socket_ + 1, nullptr, &write_set, nullptr, &timeout);
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (result <= 0 ||
        getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &error_length) < 0 ||
        error != 0) {
        close_socket();
        return false;
    }
    int keepalive = 1;
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    int keep_idle = 15;
    int keep_interval = 5;
    int keep_count = 3;
    setsockopt(socket_, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    setsockopt(
        socket_, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval,
        sizeof(keep_interval));
    setsockopt(
        socket_, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));
    return true;
}

bool NtripPushClient::send_all(const uint8_t *data, size_t length) {
    size_t offset = 0;
    int stalled_seconds = 0;
    while (offset < length && socket_ >= 0 && !suspended_ && !stopping_) {
        const int sent = send(
            socket_, data + offset, length - offset, MSG_DONTWAIT);
        if (sent > 0) {
            offset += sent;
            bytes_sent_ += sent;
            stalled_seconds = 0;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(socket_, &write_set);
            timeval timeout{1, 0};
            const int ready = select(
                socket_ + 1, nullptr, &write_set, nullptr, &timeout);
            if (ready > 0) continue;
            if (ready < 0 && errno == EINTR) continue;
            if (++stalled_seconds < 5) continue;
        }
        return false;
    }
    return offset == length;
}

std::string NtripPushClient::read_line(int timeout_ms) {
    std::string line;
    const TickType_t started = xTaskGetTickCount();
    while ((xTaskGetTickCount() - started) * portTICK_PERIOD_MS <
           static_cast<TickType_t>(timeout_ms)) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_, &read_set);
        timeval wait{0, 100000};
        const int ready = select(
            socket_ + 1, &read_set, nullptr, nullptr, &wait);
        if (ready == 0) continue;
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char byte = 0;
        const int received = recv(socket_, &byte, 1, MSG_DONTWAIT);
        if (received == 1) {
            if (byte == '\n') break;
            if (byte != '\r' && line.size() < 255) line.push_back(byte);
        } else if (received == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
    }
    return line;
}

void NtripPushClient::drain_headers() {
    while (!read_line(3000).empty()) {}
}

void NtripPushClient::close_socket() {
    connected_ = false;
    if (socket_ >= 0) {
        shutdown(socket_, SHUT_RDWR);
        close(socket_);
        socket_ = -1;
    }
}

void NtripPushClient::set_message(
    const std::string &message, bool connected) {
    std::lock_guard lock(config_mutex_);
    message_ = message;
    connected_ = connected;
}

std::string NtripPushClient::base64(const std::string &input) {
    size_t output_length = 0;
    mbedtls_base64_encode(
        nullptr, 0, &output_length,
        reinterpret_cast<const unsigned char *>(input.data()), input.size());
    std::vector<unsigned char> output(output_length + 1);
    if (mbedtls_base64_encode(
            output.data(), output.size(), &output_length,
            reinterpret_cast<const unsigned char *>(input.data()),
            input.size()) != 0) {
        return {};
    }
    return {
        reinterpret_cast<const char *>(output.data()),
        output_length,
    };
}
