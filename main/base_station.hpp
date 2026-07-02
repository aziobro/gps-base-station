#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "local_caster.hpp"
#include "ntrip_push.hpp"
#include "rinex_logger.hpp"
#include "storage.hpp"
#include "survey.hpp"
#include "um980.hpp"

enum class BaseMode {
    kSurvey,
    kTransmit,
};

struct BaseStationStatus {
    BaseMode mode = BaseMode::kSurvey;
    SurveySnapshot survey;
    NtripStatus rtk2go;
    NtripStatus onocoy;
    NtripStatus rtkdata;
    int local_clients = 0;
    LocalCaster::ClientSnapshot local_client_ips;
    uint32_t rtcm_bytes_per_second = 0;
    uint64_t rtcm_bytes_total = 0;
};

class BaseStation {
public:
    BaseStation(Storage &storage, uart_port_t command_uart, uart_port_t data_uart);
    ~BaseStation();

    esp_err_t start();
    void stop();
    esp_err_t request_survey();
    esp_err_t request_position(double lat, double lon, double height);
    esp_err_t request_raw_collection(bool enable);
    // Re-arm RINEX collection on boot if it was enabled before the last reboot.
    void resume_persisted_rinex();
    void reload_services();
    // Transient suspend (boot hold-off, OTA, shutdown) — not persisted.
    void set_streams_suspended(bool suspended);
    // User-facing global NTRIP toggle — persisted to NVS and applied.
    void set_streams_enabled(bool enabled);
    // Apply the persisted NTRIP on/off state (called after the boot hold-off).
    void apply_persisted_streams();
    // Internet availability gate for outbound NTRIP push clients. Local caster
    // remains controlled by the normal RTCM/base/raw/user stream state.
    void set_network_available(bool available);
    bool streams_suspended() const { return effective_streams_suspended(); }
    bool streams_enabled() const { return user_streams_enabled_; }
    bool healthy() const;

    BaseStationStatus status() const;
    RinexLogger::Status rinex_status() const { return rinex_logger_.status(); }
    size_t satellites(SatelliteInfo *output, size_t capacity) const;

private:
    enum class ActionType {
        kSurvey,
        kPosition,
        kStartRaw,
        kStopRaw,
    };
    struct Action {
        ActionType type;
        double lat;
        double lon;
        double height;
    };

    Storage &storage_;
    uart_port_t command_uart_;
    uart_port_t data_uart_;
    Um980 receiver_;
    SurveyManager survey_;
    LocalCaster local_caster_;
    NtripPushClient rtk2go_;
    NtripPushClient onocoy_;
    NtripPushClient rtkdata_;
    RinexLogger rinex_logger_;
    QueueHandle_t actions_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> transient_streams_suspended_{true};
    std::atomic<bool> user_streams_enabled_{true};
    std::atomic<bool> network_available_{false};
    std::atomic<bool> has_rtcm_data_{false};
    std::atomic<bool> raw_collection_{false};
    std::atomic<BaseMode> mode_{BaseMode::kSurvey};
    std::atomic<uint32_t> rtcm_bps_{0};
    std::atomic<uint64_t> rtcm_total_{0};
    std::atomic<int64_t> heartbeat_us_{0};
    static constexpr size_t kMaxRtcmFrame = 1200;
    std::array<uint8_t, kMaxRtcmFrame> rtcm_frame_{};
    size_t rtcm_frame_length_ = 0;
    size_t rtcm_frame_expected_ = 0;

    // Per-destination RTCM batch, each flushed on its own ~200ms cadence
    // phase-offset from the others (kRtcmStaggerUs in base_station.cpp) so
    // the local caster and three NTRIP casters don't all attempt to send
    // over the shared WiFi/SDIO link in the same instant every cycle.
    struct RtcmBatch {
        std::array<uint8_t, kMaxRtcmFrame> data{};
        size_t length = 0;
        int64_t next_flush_us = 0;  // 0 = not yet armed
    };
    RtcmBatch local_batch_;
    RtcmBatch rtk2go_batch_;
    RtcmBatch onocoy_batch_;
    RtcmBatch rtkdata_batch_;

    static void task_entry(void *argument);
    void run();
    void handle_action(const Action &action);
    void enter_survey(bool clear_position);
    void enter_transmit(double lat, double lon, double height);
    void enter_raw_collection();
    void exit_raw_collection();
    bool effective_streams_suspended() const;
    bool effective_outbound_streams_suspended() const;
    void apply_stream_state();
    void read_command_uart();
    void read_data_uart();
    void read_data_uart_raw();
    void feed_rtcm_byte(uint8_t byte);
    void reset_rtcm_parser();
    void reset_rtcm_batch();
    void publish_to_batch(RtcmBatch &batch, const uint8_t *data, size_t length,
                           int64_t now, int64_t phase_offset_us,
                           const std::function<void(const uint8_t *, size_t)> &flush);
    void flush_due_batch(RtcmBatch &batch, int64_t now,
                          const std::function<void(const uint8_t *, size_t)> &flush);
    void publish_rtcm_frame(const uint8_t *data, size_t length);
    static uint32_t rtcm_crc24q(const uint8_t *data, size_t length);
};
