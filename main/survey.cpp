#include "survey.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "app_config.hpp"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "survey";
constexpr double kMetresPerDegree = 111319.5;

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

std::string_view without_checksum(std::string_view value) {
    const size_t checksum = value.find('*');
    return checksum == std::string_view::npos ? value : value.substr(0, checksum);
}

bool csv_field(std::string_view value, size_t index, std::string_view &field) {
    size_t start = 0;
    for (size_t current = 0; start <= value.size(); ++current) {
        size_t end = value.find(',', start);
        if (end == std::string_view::npos) end = value.size();
        if (current == index) {
            field = value.substr(start, end - start);
            return true;
        }
        if (end == value.size()) break;
        start = end + 1;
    }
    field = {};
    return false;
}

bool parse_double(std::string_view text, double &value) {
    if (text.empty()) return false;
    char scratch[40]{};
    if (text.size() >= sizeof(scratch)) return false;
    memcpy(scratch, text.data(), text.size());
    errno = 0;
    char *end = nullptr;
    value = strtod(scratch, &end);
    return errno == 0 && end != scratch && *end == '\0' &&
           std::isfinite(value);
}

int parse_int(std::string_view text) {
    text = without_checksum(text);
    if (text.empty()) return 0;
    char scratch[16]{};
    if (text.size() >= sizeof(scratch)) return 0;
    memcpy(scratch, text.data(), text.size());
    return atoi(scratch);
}

}  // namespace

void SurveyManager::start() {
    std::lock_guard lock(mutex_);
    state_ = SurveyState::kCollecting;
    result_ = {};
    live_ = {};
    live_.state = state_;
    line_len_ = 0;
    started_us_ = esp_timer_get_time();
    completion_pending_ = false;
    samples_ = 0;
    mean_lat_ = mean_lon_ = mean_height_ = 0;
    m2_lat_ = m2_lon_ = m2_height_ = 0;
    block_index_ = UINT32_MAX;
    block_samples_ = block_count_ = 0;
    block_sum_lat_ = block_sum_lon_ = block_sum_height_ = 0;
    block_mean_lat_ = block_mean_lon_ = block_mean_height_ = 0;
    block_m2_lat_ = block_m2_lon_ = block_m2_height_ = 0;
    satellite_count_ = 0;
    gsv_count_ = 0;
    gsv_system_ = 0;
}

void SurveyManager::reset() {
    std::lock_guard lock(mutex_);
    state_ = SurveyState::kIdle;
    result_ = {};
    live_ = {};
    line_len_ = 0;
    completion_pending_ = false;
    satellite_count_ = 0;
    gsv_count_ = 0;
    gsv_system_ = 0;
}

void SurveyManager::feed(const uint8_t *data, size_t length) {
    if (!data) return;
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < length; ++i) {
        const char byte = static_cast<char>(data[i]);
        if (byte == '\n') {
            line_[line_len_] = '\0';
            parse_line(std::string_view(line_.data(), line_len_));
            line_len_ = 0;
        } else if (byte != '\r') {
            if (line_len_ < kMaxLine) {
                line_[line_len_++] = byte;
            } else {
                line_len_ = 0;
            }
        }
    }
}

SurveySnapshot SurveyManager::snapshot() const {
    std::lock_guard lock(mutex_);
    SurveySnapshot snapshot = live_;
    snapshot.state = state_;
    if (state_ == SurveyState::kCollecting) {
        snapshot.elapsed_sec =
            static_cast<uint32_t>((esp_timer_get_time() - started_us_) / 1000000);
    }
    return snapshot;
}

SurveyResult SurveyManager::result() const {
    std::lock_guard lock(mutex_);
    return result_;
}

bool SurveyManager::take_completed_result(SurveyResult &result) {
    std::lock_guard lock(mutex_);
    if (!completion_pending_) return false;
    result = result_;
    completion_pending_ = false;
    return true;
}

size_t SurveyManager::satellites(
    SatelliteInfo *output, size_t capacity) const {
    if (!output || capacity == 0) return 0;
    std::lock_guard lock(mutex_);
    const size_t count = std::min(capacity, satellite_count_);
    memcpy(output, satellite_list_.data(), count * sizeof(SatelliteInfo));
    return count;
}

void SurveyManager::parse_line(std::string_view line) {
    if (starts_with(line, "$GNGSA")) parse_gsa(line);
    else if (starts_with(line, "$GPGSV")) parse_gsv(line, 1);
    else if (starts_with(line, "$GLGSV")) parse_gsv(line, 2);
    else if (starts_with(line, "$GAGSV")) parse_gsv(line, 3);
    else if (starts_with(line, "$GBGSV")) parse_gsv(line, 4);
    else if (state_ == SurveyState::kCollecting &&
             starts_with(line, "#BESTPOSA")) {
        parse_best_position(line);
    }
}

void SurveyManager::parse_best_position(std::string_view line) {
    const size_t separator = line.find(';');
    if (separator == std::string_view::npos) return;
    const std::string_view payload = without_checksum(line.substr(separator + 1));
    std::string_view field;
    if (!csv_field(payload, 0, field) || field != "SOL_COMPUTED") return;

    double lat = 0, lon = 0, height = 0;
    double lat_sigma = 0, lon_sigma = 0, height_sigma = 0;
    std::string_view lat_field, lon_field, height_field;
    std::string_view lat_sigma_field, lon_sigma_field, height_sigma_field;
    if (!csv_field(payload, 2, lat_field) ||
        !csv_field(payload, 3, lon_field) ||
        !csv_field(payload, 4, height_field) ||
        !csv_field(payload, 7, lat_sigma_field) ||
        !csv_field(payload, 8, lon_sigma_field) ||
        !csv_field(payload, 9, height_sigma_field) ||
        !parse_double(lat_field, lat) ||
        !parse_double(lon_field, lon) ||
        !parse_double(height_field, height) ||
        !parse_double(lat_sigma_field, lat_sigma) ||
        !parse_double(lon_sigma_field, lon_sigma) ||
        !parse_double(height_sigma_field, height_sigma)) {
        return;
    }
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180 ||
        height < -1000 || height > 20000 || lat_sigma < 0 ||
        lon_sigma < 0 || height_sigma < 0 || lat_sigma > 100 ||
        lon_sigma > 100 || height_sigma > 100) {
        return;
    }

    ++samples_;
    update_welford(lat, samples_, mean_lat_, m2_lat_);
    update_welford(lon, samples_, mean_lon_, m2_lon_);
    update_welford(height, samples_, mean_height_, m2_height_);

    const uint32_t elapsed =
        static_cast<uint32_t>((esp_timer_get_time() - started_us_) / 1000000);
    const uint32_t next_block = elapsed / config::kSurveyBlockTimeSec;
    if (block_index_ == UINT32_MAX) {
        block_index_ = next_block;
    } else if (block_index_ != next_block) {
        commit_block();
        block_index_ = next_block;
    }
    block_sum_lat_ += lat;
    block_sum_lon_ += lon;
    block_sum_height_ += height;
    ++block_samples_;

    float lat_m = 9999;
    float lon_m = 9999;
    float height_m = 9999;
    float stability = 9999;
    if (block_count_ >= 2) {
        lat_m = standard_error(block_m2_lat_, block_count_) * kMetresPerDegree;
        lon_m = standard_error(block_m2_lon_, block_count_) *
                kMetresPerDegree * cos(lat * M_PI / 180.0);
        height_m = standard_error(block_m2_height_, block_count_);
        stability = sqrtf(lat_m * lat_m + lon_m * lon_m +
                          height_m * height_m);
    }

    live_.lat = mean_lat_;
    live_.lon = mean_lon_;
    live_.height = mean_height_;
    live_.stability = stability;
    live_.instantaneous_sigma =
        std::max({static_cast<float>(lat_sigma),
                  static_cast<float>(lon_sigma),
                  static_cast<float>(height_sigma)});
    live_.satellites_used =
        csv_field(payload, 13, field) ? parse_int(field) : 0;
    live_.satellites_tracked =
        csv_field(payload, 14, field) ? parse_int(field) : 0;
    live_.samples = samples_;
    live_.blocks = block_count_;
    live_.elapsed_sec = elapsed;
    live_.valid = true;

    ESP_LOGI(kTag,
             "%lus samples=%d blocks=%d stability=%.3fm "
             "position=%.8f,%.8f,%.3f",
             static_cast<unsigned long>(elapsed), samples_, block_count_,
             stability, mean_lat_, mean_lon_, mean_height_);

    if (elapsed >= config::kSurveyMinTimeSec &&
        block_count_ >= config::kSurveyMinBlocks &&
        stability <= config::kSurveyMaxStabilityM) {
        result_ = {
            mean_lat_, mean_lon_, mean_height_,
            lat_m, lon_m, height_m, elapsed,
        };
        state_ = SurveyState::kDone;
        live_.state = state_;
        completion_pending_ = true;
        ESP_LOGI(kTag, "Survey complete after %lu seconds",
                 static_cast<unsigned long>(elapsed));
    }
}

void SurveyManager::parse_gsa(std::string_view line) {
    std::string_view field;
    if (!csv_field(line, 18, field)) return;
    int count = 0;
    for (size_t i = 3; i <= 14; ++i) {
        if (csv_field(line, i, field) && !field.empty()) ++count;
    }
    if (!csv_field(line, 18, field)) return;
    switch (parse_int(field)) {
        case 1: live_.gps = count; break;
        case 2: live_.glonass = count; break;
        case 3: live_.galileo = count; break;
        case 4: live_.beidou = count; break;
        default: break;
    }
}

void SurveyManager::parse_gsv(std::string_view line, uint8_t system) {
    const std::string_view payload = without_checksum(line);
    std::string_view field;
    std::string_view message_count_field;
    std::string_view message_number_field;
    if (!csv_field(payload, 1, message_count_field) ||
        !csv_field(payload, 2, message_number_field)) {
        return;
    }
    const int message_count = parse_int(message_count_field);
    const int message_number = parse_int(message_number_field);
    if (message_count < 1 || message_number < 1) return;

    if (message_number == 1 || system != gsv_system_) {
        gsv_count_ = 0;
        gsv_system_ = system;
    }
    for (size_t i = 4; gsv_count_ < gsv_buffer_.size(); i += 4) {
        std::string_view prn_field, elevation_field, azimuth_field, snr_field;
        if (!csv_field(payload, i, prn_field) ||
            !csv_field(payload, i + 1, elevation_field) ||
            !csv_field(payload, i + 2, azimuth_field)) {
            break;
        }
        const int prn = parse_int(prn_field);
        if (prn == 0) continue;
        gsv_buffer_[gsv_count_++] = {
            static_cast<uint16_t>(std::clamp(prn, 0, 999)),
            static_cast<uint8_t>(std::clamp(parse_int(elevation_field), 0, 90)),
            static_cast<uint16_t>(std::clamp(parse_int(azimuth_field), 0, 359)),
            static_cast<uint8_t>(
                csv_field(payload, i + 3, snr_field)
                    ? std::clamp(parse_int(snr_field), 0, 99) : 0),
            system,
        };
    }
    if (message_number != message_count) return;

    size_t output = 0;
    for (size_t i = 0; i < satellite_count_; ++i) {
        if (satellite_list_[i].system != system) {
            satellite_list_[output++] = satellite_list_[i];
        }
    }
    for (size_t i = 0; i < gsv_count_ && output < satellite_list_.size(); ++i) {
        satellite_list_[output++] = gsv_buffer_[i];
    }
    satellite_count_ = output;
}

void SurveyManager::commit_block() {
    if (block_samples_ == 0) return;
    const double lat = block_sum_lat_ / block_samples_;
    const double lon = block_sum_lon_ / block_samples_;
    const double height = block_sum_height_ / block_samples_;
    ++block_count_;
    update_welford(lat, block_count_, block_mean_lat_, block_m2_lat_);
    update_welford(lon, block_count_, block_mean_lon_, block_m2_lon_);
    update_welford(
        height, block_count_, block_mean_height_, block_m2_height_);
    block_samples_ = 0;
    block_sum_lat_ = block_sum_lon_ = block_sum_height_ = 0;
}

void SurveyManager::update_welford(
    double value, int count, double &mean, double &m2) {
    const double delta = value - mean;
    mean += delta / count;
    m2 += delta * (value - mean);
}

float SurveyManager::standard_error(double m2, int count) {
    if (count < 2) return 9999;
    return static_cast<float>(sqrt(m2 / (count - 1)) / sqrt(count));
}
