#include "base_station.hpp"

#include <algorithm>
#include <array>

#include "app_config.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "base_station";
constexpr int64_t kRtcmBatchUs = 200000;

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
    rtcm_batch_length_ = 0;
    survey_.start();
    ESP_ERROR_CHECK_WITHOUT_ABORT(receiver_.configure_survey_output());
    ESP_LOGI(kTag, "Survey mode; all RTCM streams suspended");
}

void BaseStation::enter_transmit(double lat, double lon, double height) {
    mode_ = BaseMode::kTransmit;
    has_rtcm_data_ = false;
    survey_.reset();
    uart_flush_input(data_uart_);
    rtcm_batch_length_ = 0;
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        receiver_.configure_base(pos.lat, pos.lon, pos.height));
    apply_stream_state();
    ESP_LOGI(kTag, "Raw collection mode exited, RTCM restored");
}

bool BaseStation::effective_streams_suspended() const {
    return transient_streams_suspended_ || !user_streams_enabled_ ||
        mode_ != BaseMode::kTransmit || !has_rtcm_data_ || raw_collection_;
}

void BaseStation::apply_stream_state() {
    const bool suspended = effective_streams_suspended();
    local_caster_.set_suspended(suspended);
    rtk2go_.set_suspended(suspended);
    onocoy_.set_suspended(suspended);
    rtkdata_.set_suspended(suspended);
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
    while (rtcm_batch_length_ < rtcm_batch_.size()) {
        const int received = uart_read_bytes(
            data_uart_, rtcm_batch_.data() + rtcm_batch_length_,
            rtcm_batch_.size() - rtcm_batch_length_, 0);
        if (received <= 0) break;
        if (rtcm_batch_length_ == 0) {
            rtcm_batch_started_us_ = esp_timer_get_time();
        }
        rtcm_batch_length_ += received;
    }
    if (rtcm_batch_length_ == 0) return;
    if (rtcm_batch_length_ < rtcm_batch_.size() &&
        esp_timer_get_time() - rtcm_batch_started_us_ < kRtcmBatchUs) {
        return;
    }
    if (!has_rtcm_data_.exchange(true)) {
        apply_stream_state();  // first RTCM batch — unsuspend push clients
    }
    local_caster_.push(rtcm_batch_.data(), rtcm_batch_length_);
    rtk2go_.push(rtcm_batch_.data(), rtcm_batch_length_);
    onocoy_.push(rtcm_batch_.data(), rtcm_batch_length_);
    rtkdata_.push(rtcm_batch_.data(), rtcm_batch_length_);
    rtcm_total_ += rtcm_batch_length_;
    rtcm_batch_length_ = 0;
}
