#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

enum class SurveyState {
    kIdle,
    kCollecting,
    kDone,
    kError,
};

struct SurveyResult {
    double lat = 0;
    double lon = 0;
    double height = 0;
    float lat_sigma = 0;
    float lon_sigma = 0;
    float height_sigma = 0;
    uint32_t elapsed_sec = 0;
};

struct SatelliteInfo {
    uint16_t prn = 0;
    uint8_t elevation = 0;
    uint16_t azimuth = 0;
    uint8_t snr = 0;
    uint8_t system = 0;
};

struct SurveySnapshot {
    SurveyState state = SurveyState::kIdle;
    double lat = 0;
    double lon = 0;
    double height = 0;
    float stability = 0;
    float instantaneous_sigma = 0;
    int satellites_used = 0;
    int satellites_tracked = 0;
    int gps = 0;
    int glonass = 0;
    int galileo = 0;
    int beidou = 0;
    int samples = 0;
    int blocks = 0;
    uint32_t elapsed_sec = 0;
    bool valid = false;
};

class SurveyManager {
public:
    void start();
    void reset();
    void feed(const uint8_t *data, size_t length);

    SurveySnapshot snapshot() const;
    SurveyResult result() const;
    bool take_completed_result(SurveyResult &result);
    size_t satellites(SatelliteInfo *output, size_t capacity) const;

private:
    static constexpr size_t kMaxLine = 512;
    static constexpr size_t kMaxSatellites = 64;
    static constexpr size_t kMaxGsvSatellites = 20;

    mutable std::mutex mutex_;
    SurveyState state_ = SurveyState::kIdle;
    SurveyResult result_;
    SurveySnapshot live_;
    std::string line_;
    int64_t started_us_ = 0;
    bool completion_pending_ = false;

    int samples_ = 0;
    double mean_lat_ = 0;
    double mean_lon_ = 0;
    double mean_height_ = 0;
    double m2_lat_ = 0;
    double m2_lon_ = 0;
    double m2_height_ = 0;

    uint32_t block_index_ = UINT32_MAX;
    int block_samples_ = 0;
    double block_sum_lat_ = 0;
    double block_sum_lon_ = 0;
    double block_sum_height_ = 0;
    int block_count_ = 0;
    double block_mean_lat_ = 0;
    double block_mean_lon_ = 0;
    double block_mean_height_ = 0;
    double block_m2_lat_ = 0;
    double block_m2_lon_ = 0;
    double block_m2_height_ = 0;

    std::array<SatelliteInfo, kMaxSatellites> satellite_list_{};
    size_t satellite_count_ = 0;
    std::array<SatelliteInfo, kMaxGsvSatellites> gsv_buffer_{};
    size_t gsv_count_ = 0;
    uint8_t gsv_system_ = 0;

    void parse_line(const std::string &line);
    void parse_best_position(const std::string &line);
    void parse_gsa(const std::string &line);
    void parse_gsv(const std::string &line, uint8_t system);
    void commit_block();

    static void update_welford(
        double value, int count, double &mean, double &m2);
    static float standard_error(double m2, int count);
};
