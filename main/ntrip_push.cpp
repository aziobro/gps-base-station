#include "ntrip_push.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <vector>

#include "esp_log.h"
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
    if (xTaskCreatePinnedToCore(
            task_entry, label_, 6144, this, 4, &task_, 0) != pdPASS) {
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
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *results = nullptr;
    const std::string service = std::to_string(port_);
    if (getaddrinfo(host_, service.c_str(), &hints, &results) != 0 ||
        !results) {
        return false;
    }
    socket_ = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    if (socket_ < 0) {
        freeaddrinfo(results);
        return false;
    }

    const int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
    int result = connect(socket_, results->ai_addr, results->ai_addrlen);
    freeaddrinfo(results);
    if (result < 0 && errno != EINPROGRESS) {
        close_socket();
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
    fcntl(socket_, F_SETFL, flags);
    timeval io_timeout{3, 0};
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    int keepalive = 1;
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    return true;
}

bool NtripPushClient::send_all(const uint8_t *data, size_t length) {
    size_t offset = 0;
    while (offset < length && socket_ >= 0 && !suspended_ && !stopping_) {
        const int sent = send(socket_, data + offset, length - offset, 0);
        if (sent > 0) {
            offset += sent;
            bytes_sent_ += sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return offset == length;
}

std::string NtripPushClient::read_line(int timeout_ms) {
    std::string line;
    const TickType_t started = xTaskGetTickCount();
    while ((xTaskGetTickCount() - started) * portTICK_PERIOD_MS <
           static_cast<TickType_t>(timeout_ms)) {
        char byte = 0;
        const int received = recv(socket_, &byte, 1, 0);
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
