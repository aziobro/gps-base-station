#pragma once

#include <string>

#include "esp_err.h"
#include "nvs.h"

struct BasePosition {
    double lat = 0;
    double lon = 0;
    double height = 0;
    bool valid = false;
};

struct ServiceCredentials {
    std::string mountpoint;
    std::string password;
};

struct WifiCredentials {
    std::string ssid;
    std::string password;
    bool valid = false;
};

class Storage {
public:
    Storage() = default;
    ~Storage();

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    esp_err_t open();
    void close();

    BasePosition load_position() const;
    esp_err_t save_position(double lat, double lon, double height);
    esp_err_t clear_position();

    bool admin_password_set() const;
    std::string admin_password() const;
    esp_err_t save_admin_password(const std::string &password);

    ServiceCredentials load_service(const char *service) const;
    esp_err_t save_service(const char *service,
                           const ServiceCredentials &credentials);
    bool service_enabled(const char *service, bool default_value = true) const;
    esp_err_t set_service_enabled(const char *service, bool enabled);

    WifiCredentials load_wifi() const;
    esp_err_t save_wifi(const WifiCredentials &credentials);

    // SoftAP (hotspot) WPA2 password. Empty string means "use the built-in
    // default" (see config::kDefaultApPassword).
    std::string ap_password() const;
    esp_err_t save_ap_password(const std::string &password);

    // RINEX antenna metadata written into the observation file header so PPP
    // services (OPUS, CSRS-PPP) apply phase-centre corrections automatically.
    // Defaults: model "HXCGPS500", radome "NONE", height 0.0 m (solve for ARP).
    std::string antenna_model() const;
    std::string antenna_radome() const;
    double      antenna_height() const;  // ARP DELTA H, metres
    esp_err_t   save_antenna(const std::string &model,
                             const std::string &radome, double height);

    // Global NTRIP push on/off (survives power cycles). Defaults to enabled.
    bool ntrip_streams_enabled(bool default_value = true) const;
    esp_err_t set_ntrip_streams_enabled(bool enabled);

    // RINEX raw-data collection on/off (survives power cycles). Defaults to off.
    // When enabled, collection resumes automatically after a reboot.
    bool rinex_collection_enabled(bool default_value = false) const;
    esp_err_t set_rinex_collection_enabled(bool enabled);

    // Marine-instrument theme: true = night (default), false = day. Shared by both
    // the touchscreen and the web UI so they agree across reboots.
    bool night_mode(bool default_value = true) const;
    esp_err_t set_night_mode(bool night);

private:
    nvs_handle_t handle_ = 0;
    bool open_ = false;

    bool get_bool(const char *key, bool default_value) const;
    std::string get_string(const char *key) const;
    double get_double(const char *key, double default_value) const;

    esp_err_t set_bool(const char *key, bool value);
    esp_err_t set_string(const char *key, const std::string &value);
    esp_err_t set_double(const char *key, double value);
    esp_err_t commit();

    static std::string service_key(const char *service, const char *suffix);
};
