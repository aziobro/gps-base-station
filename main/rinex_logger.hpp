#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Collects GPS raw observations from the UM980 RANGEA ASCII stream and writes
// RINEX 3.03 observation files to /sdcard/rawdata/.  Each file covers one hour
// (120 epochs at 30 s intervals).  RTK output on COM3 must be suspended before
// calling start(); the caller switches COM3 back to RTCM after calling stop().
class RinexLogger {
public:
    struct Status {
        bool        active       = false;
        int         epochs       = 0;   // epochs written in current file
        int         files        = 0;   // files closed since start()
        std::string current_file;
    };

    // Start logging.  lat/lon in decimal degrees, height in metres (WGS-84).
    // Antenna metadata is written into the RINEX header so PPP services apply
    // phase-centre corrections (model = IGS/NGS antenna name, radome e.g.
    // "NONE", delta_h = ARP height in metres).
    void start(double lat, double lon, double height,
               const std::string &ant_model = "HXCGPS500",
               const std::string &ant_radome = "NONE",
               double ant_delta_h = 0.0);
    void stop();

    bool   is_active() const { return active_; }
    Status status()    const;

    // Feed raw bytes arriving from the UM980 data UART.
    void feed(const uint8_t *data, size_t len);

private:
    struct SigObs {
        double pseudorange   = 0.0;
        double carrier_phase = 0.0;  // accumulated Doppler range (cycles)
        float  doppler       = 0.0f; // Hz
        float  cn0           = 0.0f; // dB-Hz
        bool   valid         = false;
    };

    // Frequency bands per satellite, in RINEX header order:
    //   band 0 = L1 / E1 / B1I, band 1 = L2 / E5a / B3I, band 2 = L5 (GPS only).
    static constexpr int kBands = 3;

    struct SatObs {
        uint16_t prn      = 0;
        uint8_t  system   = 0;  // 0=GPS 1=GLO 3=GAL 4=BDS 5=QZSS
        int8_t   glo_freq = 0;  // GLONASS frequency channel (-7..+6)
        SigObs   sig[kBands];   // one observation set per frequency band
        int8_t   pref[kBands] = {-1, -1, -1};  // stored signal's within-band rank
    };

    void process_message();
    void open_file(int gps_week, double tow);
    void close_file();
    void rotate_if_needed(int gps_week, double tow);
    void write_header(int gps_week, double tow);
    void write_epoch(int gps_week, double tow, std::vector<SatObs> &sats);

    double ecef_x_ = 0, ecef_y_ = 0, ecef_z_ = 0;
    double lat_ = 0, lon_ = 0, height_ = 0;

    // Antenna metadata for the RINEX header (see start()).
    std::string ant_model_  = "HXCGPS500";
    std::string ant_radome_ = "NONE";
    double      ant_delta_h_ = 0.0;

    std::atomic<bool> active_{false};
    FILE       *file_         = nullptr;
    std::string current_file_;
    int         gps_week_open_ = 0;
    double      tow_open_      = 0.0;
    int         epochs_        = 0;  // epochs in current file
    int         files_         = 0;

    // Systems (bitmask over channel-status system ids) declared in this file's
    // SYS / # / OBS TYPES header, fixed from the first epoch.
    uint8_t     present_mask_  = 0;
    // Byte offset of the TIME OF LAST OBS header line, patched on close once the
    // final epoch time is known.
    long        last_obs_pos_  = 0;
    int         last_obs_week_ = 0;
    double      last_obs_tow_  = 0.0;

    static constexpr size_t kMsgBufSize = 32 * 1024;
    char   msg_buf_[kMsgBufSize];
    size_t msg_len_ = 0;
    bool   in_msg_  = false;
};
