#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

enum class NtripProtocol {
    kV1,
    kV2,
};

struct NtripStatus {
    bool enabled = false;
    bool connected = false;
    std::string message;            // live state (connecting / connected / …)
    uint64_t bytes_sent = 0;        // RTCM payload bytes pushed to the caster
    uint32_t dropped_batches = 0;   // batches discarded because the queue was full
    uint32_t reconnects = 0;        // times a live connection dropped and reconnected
    std::string last_error;         // sticky last failure/rejection reason
    uint32_t connected_sec = 0;     // age of the current connection (0 when down)
    uint32_t last_send_age_sec = 0; // seconds since the last successful payload send
    bool ever_sent = false;         // false until the first payload byte is sent
};

class NtripPushClient {
public:
    NtripPushClient(const char *label, const char *host, uint16_t port,
                    NtripProtocol protocol, uint32_t connect_stagger_ms = 0);
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
    uint32_t connect_stagger_ms_;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> suspended_{true};
    std::atomic<bool> reconnect_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint32_t> dropped_batches_{0};
    std::atomic<uint32_t> reconnects_{0};
    std::atomic<bool> ever_connected_{false};
    // Microsecond timestamps (esp_timer) for connection uptime / send freshness.
    std::atomic<int64_t> connected_since_us_{0};
    std::atomic<int64_t> last_send_us_{0};
    std::atomic<int64_t> resumed_us_{0};
    mutable std::mutex config_mutex_;
    bool enabled_ = false;
    std::string mountpoint_;
    std::string password_;
    std::string message_ = "disabled";
    std::string last_error_;
    int socket_ = -1;
    // Cached resolved address — avoids a blocking getaddrinfo() on every
    // reconnect, which holds the lwIP mutex and stalls HTTPS handshakes.
    sockaddr_in cached_addr_{};
    bool cached_addr_valid_ = false;

    static void task_entry(void *argument);
    void run();
    bool connect_caster();
    bool connect_socket();
    bool send_all(const uint8_t *data, size_t length);
    std::string read_line(int timeout_ms);
    void drain_headers();
    void close_socket();
    void set_message(const std::string &message, bool connected);
    // Records a sticky failure/rejection reason (survives later state changes)
    // and mirrors it into the live message.
    void set_error(const std::string &error);
    static std::string base64(const std::string &input);
};
