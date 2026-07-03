#include "base_station.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "app_config.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "base_station";
constexpr int64_t kRtcmBatchUs = 200000;
// Even quarter-period spacing across the 4 RTCM destinations (local caster +
// 3 NTRIP casters) so their flushes land at different instants within each
// 200ms cycle instead of all firing together.
constexpr int64_t kRtcmStaggerUs = kRtcmBatchUs / 4;

}  // namespace

BaseStation::BaseStation(
    Storage &storage, uart_port_t command_uart, uart_port_t data_uart)
    : storage_(storage),
      command_uart_(command_uart),
      data_uart_(data_uart),
      receiver_(command_uart),
      rtk2go_("RTK2go", config::kRtk2goHost, config::kRtk2goPort,
              NtripProtocol::kV1),
      onocoy_("Onocoy", config::kOnocoyHost, config::kOnocoyPort,
              NtripProtocol::kV2, 10000),
      rtkdata_("RTKdata", config::kRtkdataHost, config::kRtkdataPort,
               NtripProtocol::kV1, 20000) {}

BaseStation::~BaseStation() {
    stop();
}

esp_err_t BaseStation::start() {
    stopping_ = false;
    actions_ = xQueueCreate(4, sizeof(Action));
    if (!actions_) return ESP_ERR_NO_MEM;

    user_streams_enabled_ = storage_.ntrip_streams_enabled();
    transient_streams_suspended_ = true;

    auto cleanup_failed_start = [this](esp_err_t err, const char *message) {
        ESP_LOGE(kTag, "%s: %s", message, esp_err_to_name(err));
        local_caster_.stop();
        rtkdata_.stop();
        onocoy_.stop();
        rtk2go_.stop();
        if (actions_) {
            vQueueDelete(actions_);
            actions_ = nullptr;
        }
        return err;
    };

    const ServiceCredentials rtk2go = storage_.load_service("rtk2go");
    const ServiceCredentials onocoy = storage_.load_service("onocoy");
    const ServiceCredentials rtkdata = storage_.load_service("rtkdata");
    esp_err_t err = rtk2go_.start(
        storage_.service_enabled("rtk2go"), rtk2go.mountpoint,
        rtk2go.password);
    if (err != ESP_OK) return cleanup_failed_start(err, "RTK2go task failed");

    err = onocoy_.start(
        storage_.service_enabled("onocoy"), onocoy.mountpoint,
        onocoy.password);
    if (err != ESP_OK) return cleanup_failed_start(err, "Onocoy task failed");

    err = rtkdata_.start(
        storage_.service_enabled("rtkdata"), rtkdata.mountpoint,
        rtkdata.password);
    if (err != ESP_OK) return cleanup_failed_start(err, "RTKdata task failed");

    err = local_caster_.start();
    if (err != ESP_OK) return cleanup_failed_start(err, "Local caster task failed");

    if (xTaskCreatePinnedToCore(
            task_entry, "base_station", 7168, this, 6, &task_, 1) != pdPASS) {
        task_ = nullptr;
        return cleanup_failed_start(ESP_ERR_NO_MEM, "Base station task failed");
    }
    return ESP_OK;
}

void BaseStation::stop() {
    stopping_ = true;
    while (task_) vTaskDelay(pdMS_TO_TICKS(10));
    local_caster_.stop();
    rtk2go_.stop();
    onocoy_.stop();
    rtkdata_.stop();
    if (actions_) {
        vQueueDelete(actions_);
        actions_ = nullptr;
    }
}

esp_err_t BaseStation::request_survey() {
    if (!actions_) return ESP_ERR_INVALID_STATE;
    const Action action{ActionType::kSurvey, 0, 0, 0};
    return xQueueSend(actions_, &action, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t BaseStation::request_raw_collection(bool enable) {
    if (!actions_) return ESP_ERR_INVALID_STATE;
    // Persist the user's intent so collection resumes after a reboot.
    ESP_ERROR_CHECK_WITHOUT_ABORT(storage_.set_rinex_collection_enabled(enable));
    const ActionType t = enable ? ActionType::kStartRaw : ActionType::kStopRaw;
    const Action action{t, 0, 0, 0};
    return xQueueSend(actions_, &action, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

void BaseStation::resume_persisted_rinex() {
    if (!actions_ || !storage_.rinex_collection_enabled()) return;
    // Queue a start without re-persisting; enter_raw_collection() requires Base
    // TX mode, which is the normal post-reboot state when a position is stored.
    const Action action{ActionType::kStartRaw, 0, 0, 0};
    xQueueSend(actions_, &action, 0);
    ESP_LOGI(kTag, "Resuming persisted RINEX collection after reboot");
}

esp_err_t BaseStation::request_position(
    double lat, double lon, double height) {
    if (!actions_) return ESP_ERR_INVALID_STATE;
    const Action action{ActionType::kPosition, lat, lon, height};
    return xQueueSend(actions_, &action, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

void BaseStation::reload_services() {
    const ServiceCredentials rtk2go = storage_.load_service("rtk2go");
    const ServiceCredentials onocoy = storage_.load_service("onocoy");
    const ServiceCredentials rtkdata = storage_.load_service("rtkdata");
    rtk2go_.configure(
        storage_.service_enabled("rtk2go"), rtk2go.mountpoint,
        rtk2go.password);
    onocoy_.configure(
        storage_.service_enabled("onocoy"), onocoy.mountpoint,
        onocoy.password);
    rtkdata_.configure(
        storage_.service_enabled("rtkdata"), rtkdata.mountpoint,
        rtkdata.password);
    apply_stream_state();
}

void BaseStation::set_streams_suspended(bool suspended) {
    transient_streams_suspended_ = suspended;
    apply_stream_state();
}

void BaseStation::set_streams_enabled(bool enabled) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(storage_.set_ntrip_streams_enabled(enabled));
    user_streams_enabled_ = enabled;
    apply_stream_state();
}

void BaseStation::apply_persisted_streams() {
    user_streams_enabled_ = storage_.ntrip_streams_enabled();
    transient_streams_suspended_ = false;
    apply_stream_state();
}

void BaseStation::set_network_available(bool available) {
    if (network_available_.exchange(available) == available) return;
    apply_stream_state();
    ESP_LOGI(
        kTag, "Outbound NTRIP streams %s because station WiFi is %s",
        available ? "enabled" : "suspended",
        available ? "connected" : "offline");
}

bool BaseStation::healthy() const {
    const int64_t heartbeat = heartbeat_us_;
    return heartbeat > 0 && esp_timer_get_time() - heartbeat < 2000000;
}

BaseStationStatus BaseStation::status() const {
    BaseStationStatus status{};
    status.mode = mode_.load(std::memory_order_relaxed);
    status.survey = survey_.snapshot();
    status.rtk2go = rtk2go_.status();
    status.onocoy = onocoy_.status();
    status.rtkdata = rtkdata_.status();
    status.local_client_ips = local_caster_.client_snapshot();
    status.local_clients = status.local_client_ips.count;
    status.rtcm_bytes_per_second = rtcm_bps_.load(std::memory_order_relaxed);
    status.rtcm_bytes_total = rtcm_total_.load(std::memory_order_relaxed);
    return status;
}

size_t BaseStation::satellites(
    SatelliteInfo *output, size_t capacity) const {
    return survey_.satellites(output, capacity);
}

void BaseStation::task_entry(void *argument) {
    auto *station = static_cast<BaseStation *>(argument);
    station->run();
    station->task_ = nullptr;
    vTaskDelete(nullptr);
}

void BaseStation::run() {
    const BasePosition position = storage_.load_position();
    if (position.valid) {
        enter_transmit(position.lat, position.lon, position.height);
    } else {
        enter_survey(false);
    }

    int64_t rate_started = esp_timer_get_time();
    uint32_t rate_bytes = 0;
    while (!stopping_) {
        heartbeat_us_ = esp_timer_get_time();
        Action action{};
        while (xQueueReceive(actions_, &action, 0) == pdTRUE) {
            handle_action(action);
        }
        read_command_uart();
        if (mode_ == BaseMode::kSurvey) {
            uint8_t discard[256];
            while (uart_read_bytes(
                       data_uart_, discard, sizeof(discard), 0) > 0) {}
            SurveyResult completed;
            if (survey_.take_completed_result(completed)) {
                if (storage_.save_position(
                        completed.lat, completed.lon, completed.height) == ESP_OK) {
                    enter_transmit(
                        completed.lat, completed.lon, completed.height);
                } else {
                    ESP_LOGE(kTag, "Survey position could not be saved");
                }
            }
        } else if (raw_collection_) {
            read_data_uart_raw();
        } else {
            const uint64_t before = rtcm_total_;
            read_data_uart();
            rate_bytes += static_cast<uint32_t>(rtcm_total_ - before);
        }

        const int64_t now = esp_timer_get_time();
        if (now - rate_started >= 1000000) {
            const int64_t elapsed = now - rate_started;
            rtcm_bps_ = static_cast<uint32_t>(
                static_cast<uint64_t>(rate_bytes) * 1000000ULL / elapsed);
            rate_bytes = 0;
            rate_started = now;
        }
        vTaskDelay(1);
    }
    set_streams_suspended(true);
}

void BaseStation::handle_action(const Action &action) {
    if (action.type == ActionType::kSurvey) {
        if (raw_collection_) exit_raw_collection();
        enter_survey(true);
        return;
    }
    if (action.type == ActionType::kStartRaw) {
        enter_raw_collection();
        return;
    }
    if (action.type == ActionType::kStopRaw) {
        exit_raw_collection();
        return;
    }
    if (raw_collection_) exit_raw_collection();
    if (storage_.save_position(
            action.lat, action.lon, action.height) != ESP_OK) {
        ESP_LOGE(kTag, "Manual position could not be saved");
        return;
    }
    enter_transmit(action.lat, action.lon, action.height);
}

void BaseStation::enter_survey(bool clear_position) {
    mode_ = BaseMode::kSurvey;
    has_rtcm_data_ = false;
    apply_stream_state();
    if (clear_position) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(storage_.clear_position());
    }
    uart_flush_input(data_uart_);
    reset_rtcm_parser();
    reset_rtcm_batch();
    survey_.start();
    ESP_ERROR_CHECK_WITHOUT_ABORT(receiver_.configure_survey_output());
    ESP_LOGI(kTag, "Survey mode; all RTCM streams suspended");
}

void BaseStation::enter_transmit(double lat, double lon, double height) {
    mode_ = BaseMode::kTransmit;
    has_rtcm_data_ = false;
    survey_.reset();
    uart_flush_input(data_uart_);
    reset_rtcm_parser();
    reset_rtcm_batch();
    ESP_ERROR_CHECK_WITHOUT_ABORT(receiver_.configure_base(lat, lon, height));
    apply_stream_state();
    ESP_LOGI(kTag, "Base transmission mode");
}

void BaseStation::enter_raw_collection() {
    if (mode_ != BaseMode::kTransmit) {
        ESP_LOGW(kTag, "Raw collection requires Base TX mode");
        return;
    }
    if (raw_collection_) return;
    raw_collection_ = true;
    apply_stream_state();  // suspend all NTRIP/local streams
    uart_flush_input(data_uart_);
    reset_rtcm_parser();
    reset_rtcm_batch();
    ESP_ERROR_CHECK_WITHOUT_ABORT(receiver_.configure_raw_output());
    const BasePosition pos = storage_.load_position();
    rinex_logger_.start(pos.lat, pos.lon, pos.height,
                        storage_.antenna_model(), storage_.antenna_radome(),
                        storage_.antenna_height());
    ESP_LOGI(kTag, "Raw collection mode entered");
}

void BaseStation::exit_raw_collection() {
    if (!raw_collection_) return;
    rinex_logger_.stop();
    raw_collection_ = false;
    uart_flush_input(data_uart_);
    // Restore RTCM output; has_rtcm_data_ is reset so streams stay suspended
    // until the first RTCM batch arrives (prevents empty connections to RTK2go).
    const BasePosition pos = storage_.load_position();
    has_rtcm_data_ = false;
    reset_rtcm_parser();
    reset_rtcm_batch();
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        receiver_.configure_base(pos.lat, pos.lon, pos.height));
    apply_stream_state();
    ESP_LOGI(kTag, "Raw collection mode exited, RTCM restored");
}

bool BaseStation::effective_streams_suspended() const {
    return transient_streams_suspended_ || !user_streams_enabled_ ||
        mode_ != BaseMode::kTransmit || !has_rtcm_data_ || raw_collection_;
}

bool BaseStation::effective_outbound_streams_suspended() const {
    return effective_streams_suspended() || !network_available_;
}

void BaseStation::apply_stream_state() {
    const bool suspended = effective_streams_suspended();
    local_caster_.set_suspended(suspended);
    const bool outbound_suspended = effective_outbound_streams_suspended();
    rtk2go_.set_suspended(outbound_suspended);
    onocoy_.set_suspended(outbound_suspended);
    rtkdata_.set_suspended(outbound_suspended);
}

void BaseStation::read_command_uart() {
    uint8_t buffer[256];
    int received = 0;
    do {
        received = uart_read_bytes(
            command_uart_, buffer, sizeof(buffer), 0);
        if (received > 0) survey_.feed(buffer, received);
    } while (received == static_cast<int>(sizeof(buffer)));
}

void BaseStation::read_data_uart_raw() {
    uint8_t buf[256];
    int n;
    while ((n = uart_read_bytes(data_uart_, buf, sizeof(buf), 0)) > 0) {
        rinex_logger_.feed(buf, n);
    }
}

void BaseStation::read_data_uart() {
    uint8_t buffer[256];
    int received = 0;
    do {
        received = uart_read_bytes(data_uart_, buffer, sizeof(buffer), 0);
        for (int i = 0; i < received; ++i) {
            feed_rtcm_byte(buffer[i]);
        }
    } while (received == static_cast<int>(sizeof(buffer)));

    const int64_t now = esp_timer_get_time();
    flush_due_batch(local_batch_, now,
                     [this](const uint8_t *d, size_t n) { local_caster_.push(d, n); });
    flush_due_batch(rtk2go_batch_, now,
                     [this](const uint8_t *d, size_t n) { rtk2go_.push(d, n); });
    flush_due_batch(onocoy_batch_, now,
                     [this](const uint8_t *d, size_t n) { onocoy_.push(d, n); });
    flush_due_batch(rtkdata_batch_, now,
                     [this](const uint8_t *d, size_t n) { rtkdata_.push(d, n); });
}

void BaseStation::feed_rtcm_byte(uint8_t byte) {
    if (rtcm_frame_length_ == 0) {
        if (byte != 0xD3) return;
        rtcm_frame_[rtcm_frame_length_++] = byte;
        return;
    }

    rtcm_frame_[rtcm_frame_length_++] = byte;

    if (rtcm_frame_length_ == 2 && (rtcm_frame_[1] & 0xFC) != 0) {
        reset_rtcm_parser();
        if (byte == 0xD3) rtcm_frame_[rtcm_frame_length_++] = byte;
        return;
    }

    if (rtcm_frame_length_ == 3) {
        const size_t payload_length =
            (static_cast<size_t>(rtcm_frame_[1] & 0x03) << 8) |
            static_cast<size_t>(rtcm_frame_[2]);
        rtcm_frame_expected_ = payload_length + 6;
        if (rtcm_frame_expected_ > rtcm_frame_.size() ||
            rtcm_frame_expected_ < 6) {
            reset_rtcm_parser();
        }
        return;
    }

    if (rtcm_frame_expected_ == 0 ||
        rtcm_frame_length_ < rtcm_frame_expected_) {
        return;
    }

    const uint32_t actual_crc =
        (static_cast<uint32_t>(rtcm_frame_[rtcm_frame_expected_ - 3]) << 16) |
        (static_cast<uint32_t>(rtcm_frame_[rtcm_frame_expected_ - 2]) << 8) |
        static_cast<uint32_t>(rtcm_frame_[rtcm_frame_expected_ - 1]);
    const uint32_t expected_crc =
        rtcm_crc24q(rtcm_frame_.data(), rtcm_frame_expected_ - 3);
    if (actual_crc == expected_crc) {
        publish_rtcm_frame(rtcm_frame_.data(), rtcm_frame_expected_);
    } else {
        ESP_LOGW(kTag, "Dropped RTCM frame with bad CRC");
    }
    reset_rtcm_parser();
}

void BaseStation::reset_rtcm_parser() {
    rtcm_frame_length_ = 0;
    rtcm_frame_expected_ = 0;
}

void BaseStation::reset_rtcm_batch() {
    local_batch_.length = 0;
    local_batch_.frame_count = 0;
    local_batch_.next_flush_us = 0;
    rtk2go_batch_.length = 0;
    rtk2go_batch_.frame_count = 0;
    rtk2go_batch_.next_flush_us = 0;
    onocoy_batch_.length = 0;
    onocoy_batch_.frame_count = 0;
    onocoy_batch_.next_flush_us = 0;
    rtkdata_batch_.length = 0;
    rtkdata_batch_.frame_count = 0;
    rtkdata_batch_.next_flush_us = 0;
}

void BaseStation::flush_batch_frames(
    RtcmBatch &batch,
    const std::function<void(const uint8_t *, size_t)> &flush) {
    size_t start = 0;
    for (size_t i = 0; i < batch.frame_count; ++i) {
        const size_t end = batch.frame_ends[i];
        flush(batch.data.data() + start, end - start);
        start = end;
    }
    batch.length = 0;
    batch.frame_count = 0;
}

void BaseStation::publish_to_batch(
    RtcmBatch &batch, const uint8_t *data, size_t length, int64_t now,
    int64_t phase_offset_us,
    const std::function<void(const uint8_t *, size_t)> &flush) {
    const bool bytes_full =
        batch.length > 0 && batch.length + length > batch.data.size();
    const bool frames_full =
        batch.frame_count >= batch.frame_ends.size();
    if (bytes_full || frames_full) {
        flush_batch_frames(batch, flush);
        batch.next_flush_us = now + kRtcmBatchUs;
    }
    memcpy(batch.data.data() + batch.length, data, length);
    batch.length += length;
    batch.frame_ends[batch.frame_count++] = batch.length;
    if (batch.next_flush_us == 0) {
        // First batch for this destination: arm its recurring schedule
        // offset by phase_offset_us so it doesn't land on the same instant
        // as the other destinations. Every later flush reschedules itself
        // kRtcmBatchUs after whenever it actually fired, which preserves
        // this initial stagger indefinitely.
        batch.next_flush_us = now + phase_offset_us + kRtcmBatchUs;
    }
    if (now >= batch.next_flush_us) {
        flush_batch_frames(batch, flush);
        batch.next_flush_us = now + kRtcmBatchUs;
    }
}

void BaseStation::flush_due_batch(
    RtcmBatch &batch, int64_t now,
    const std::function<void(const uint8_t *, size_t)> &flush) {
    if (batch.length > 0 && batch.next_flush_us != 0 &&
        now >= batch.next_flush_us) {
        flush_batch_frames(batch, flush);
        batch.next_flush_us = now + kRtcmBatchUs;
    }
}

void BaseStation::publish_rtcm_frame(const uint8_t *data, size_t length) {
    if (!data || length == 0) return;
    if (length > kMaxRtcmFrame) {
        ESP_LOGW(kTag, "Dropped oversized RTCM frame: %u bytes",
                 static_cast<unsigned>(length));
        return;
    }
    if (!has_rtcm_data_.exchange(true)) {
        apply_stream_state();  // first valid RTCM frame unsuspends clients
    }

    const int64_t now = esp_timer_get_time();
    publish_to_batch(local_batch_, data, length, now, 0,
                      [this](const uint8_t *d, size_t n) { local_caster_.push(d, n); });
    publish_to_batch(rtk2go_batch_, data, length, now, kRtcmStaggerUs,
                      [this](const uint8_t *d, size_t n) { rtk2go_.push(d, n); });
    publish_to_batch(onocoy_batch_, data, length, now, kRtcmStaggerUs * 2,
                      [this](const uint8_t *d, size_t n) { onocoy_.push(d, n); });
    publish_to_batch(rtkdata_batch_, data, length, now, kRtcmStaggerUs * 3,
                      [this](const uint8_t *d, size_t n) { rtkdata_.push(d, n); });
    rtcm_total_ += length;
}

uint32_t BaseStation::rtcm_crc24q(const uint8_t *data, size_t length) {
    uint32_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (int bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFB;
        }
    }
    return crc & 0xFFFFFF;
}
