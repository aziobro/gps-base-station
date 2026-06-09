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
    void start(double lat, double lon, double height);
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

    struct SatObs {
        uint16_t prn      = 0;
        uint8_t  system   = 0;  // 0=GPS 1=GLO 3=GAL 4=BDS 5=QZSS
        int8_t   glo_freq = 0;  // GLONASS frequency channel (-7..+6)
        SigObs   sig1;          // primary (L1 / E1 / B1I)
        SigObs   sig2;          // secondary (L2 / E5a / B3I)
    };

    void process_message();
    void open_file(int gps_week, double tow);
    void close_file();
    void rotate_if_needed(int gps_week, double tow);
    void write_header(int gps_week, double tow);
    void write_epoch(int gps_week, double tow, std::vector<SatObs> &sats);

    static char sys_char(uint8_t sys);
    static int  display_prn(uint8_t sys, uint16_t prn);
    static bool prefers_sig2(uint8_t sys, uint8_t sig);

    double ecef_x_ = 0, ecef_y_ = 0, ecef_z_ = 0;
    double lat_ = 0, lon_ = 0, height_ = 0;

    std::atomic<bool> active_{false};
    FILE       *file_         = nullptr;
    std::string current_file_;
    int         gps_week_open_ = 0;
    double      tow_open_      = 0.0;
    int         epochs_        = 0;  // epochs in current file
    int         files_         = 0;

    static constexpr size_t kMsgBufSize = 32 * 1024;
    char   msg_buf_[kMsgBufSize];
    size_t msg_len_ = 0;
    bool   in_msg_  = false;
};
