#pragma once

#include <atomic>
#include <array>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

class LocalCaster {
public:
    static constexpr int kMaxClients = 8;

    struct ClientSnapshot {
        int count = 0;
        std::array<uint32_t, kMaxClients> ipv4{};
    };

    ~LocalCaster();

    esp_err_t start();
    void stop();
    void set_suspended(bool suspended);
    void push(const uint8_t *data, size_t length);
    int client_count() const;
    ClientSnapshot client_snapshot() const;

private:
    static constexpr size_t kMaxPacket = 1200;
    // Must exceed BaseStation::RtcmBatch::kMaxFramesPerBatch (12) -- a single
    // batch flush calls push() once per accumulated frame in a tight loop
    // (base_station.cpp flush_batch_frames()), faster than run()'s ~20ms
    // poll can drain. A queue at or below 12 periodically overflowed in
    // real operation (roughly every 30s, when the 5s/10s/30s periodic RTCM
    // messages all landed in the same 200ms batch window as the six 1Hz MSM7
    // frames) -- found via rinex-recorder, a persistent local NTRIP client
    // that made the resulting forced client disconnects visible (2026-07-08).
    static constexpr int kQueueDepth = 16;
    struct Packet {
        uint16_t length;
        uint8_t data[kMaxPacket];
    };

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> suspended_{true};
    std::atomic<bool> reset_clients_{false};
    std::atomic<int> client_count_{0};
    std::array<std::atomic<uint32_t>, kMaxClients> client_ipv4_{};
    int listener_ = -1;
    int clients_[kMaxClients] = {-1, -1, -1, -1, -1, -1, -1, -1};

    static void task_entry(void *argument);
    void run();
    bool open_listener();
    void accept_client();
    void broadcast(const Packet &packet);
    void close_clients();
    void close_all();
    void recount_clients();
    static bool send_all(int socket, const uint8_t *data, size_t length);
};
