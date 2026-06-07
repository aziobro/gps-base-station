#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum class NtripProtocol {
    kV1,
    kV2,
};

struct NtripStatus {
    bool enabled = false;
    bool connected = false;
    std::string message;
    uint64_t bytes_sent = 0;
    uint32_t dropped_batches = 0;
};

class NtripPushClient {
public:
    NtripPushClient(const char *label, const char *host, uint16_t port,
                    NtripProtocol protocol);
    ~NtripPushClient();

    esp_err_t start(
        bool enabled, const std::string &mountpoint,
        const std::string &password);
    void stop();
    void configure(
        bool enabled, const std::string &mountpoint,
        const std::string &password);
    void set_suspended(bool suspended);
    void push(const uint8_t *data, size_t length);
    NtripStatus status() const;

private:
    static constexpr size_t kMaxPacket = 1024;
    static constexpr int kQueueDepth = 12;

    struct Packet {
        uint16_t length;
        uint8_t data[kMaxPacket];
    };

    const char *label_;
    const char *host_;
    uint16_t port_;
    NtripProtocol protocol_;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> suspended_{true};
    std::atomic<bool> reconnect_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint32_t> dropped_batches_{0};
    mutable std::mutex config_mutex_;
    bool enabled_ = false;
    std::string mountpoint_;
    std::string password_;
    std::string message_ = "disabled";
    int socket_ = -1;

    static void task_entry(void *argument);
    void run();
    bool connect_caster();
    bool connect_socket();
    bool send_all(const uint8_t *data, size_t length);
    std::string read_line(int timeout_ms);
    void drain_headers();
    void close_socket();
    void set_message(const std::string &message, bool connected);
    static std::string base64(const std::string &input);
};
