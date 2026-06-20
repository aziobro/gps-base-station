#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "storage.hpp"

struct WifiNetwork {
    std::string ssid;
    int rssi = -127;
    bool secured = false;
};

class WifiManager {
public:
    enum class State {
        kDisconnected,
        kConnected,
        kAccessPoint,
    };

    WifiManager() = default;
    ~WifiManager();

    WifiManager(const WifiManager &) = delete;
    WifiManager &operator=(const WifiManager &) = delete;

    esp_err_t start(Storage &storage);

    State state() const;
    bool connected() const;
    bool access_point_active() const;
    int rssi() const;
    std::string ssid() const;
    std::string ip_address() const;
    std::string access_point_ssid() const;  // SoftAP name, "" when AP is down
    std::string access_point_ip() const;     // SoftAP gateway IP, "" when down
    std::vector<WifiNetwork> scan_networks();
    esp_err_t update_credentials(const WifiCredentials &credentials);
    // Re-reads the AP password from storage and applies it to the running
    // SoftAP without a reboot. Call after saving a new AP password.
    esp_err_t apply_ap_settings();

private:
    static constexpr EventBits_t kConnectedBit = BIT0;
    static constexpr uint32_t kRetryIntervalMs = 10000;   // station re-association retry
    static constexpr int64_t  kApFallbackMs    = 120000;  // raise SoftAP after 2 min offline
    static constexpr int64_t  kApInfoRefreshMs = 5000;    // throttle esp_wifi_sta_get_ap_info cache

    Storage *storage_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t recovery_task_ = nullptr;
    esp_netif_t *station_netif_ = nullptr;
    esp_netif_t *access_point_netif_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    esp_event_handler_instance_t lost_ip_handler_ = nullptr;

    std::atomic<bool> connected_{false};
    std::atomic<bool> access_point_active_{false};
    std::atomic<bool> disable_ap_pending_{false};  // set on reconnect while AP is up
    std::atomic<int64_t> disconnected_at_ms_{0};
    std::atomic<int64_t> last_retry_ms_{0};
    std::atomic<bool> stopping_{false};

    // Cached STA association info, refreshed by recovery_loop at most every
    // kApInfoRefreshMs so rssi()/ssid() never issue a blocking SDIO RPC on the
    // LVGL/httpd hot paths. cached_* are guarded by ap_info_mutex_ (the SSID is
    // a fixed buffer because std::string tears under concurrent read/write).
    mutable std::mutex ap_info_mutex_;
    char cached_ssid_[33] = {};   // up to 32-byte SSID + NUL
    int cached_rssi_ = -127;
    bool cached_valid_ = false;   // true once a successful RPC has populated it
    std::atomic<int64_t> ap_info_last_ms_{0};  // 0 = refresh on next tick (boot / post-disconnect)

    static void event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
    static void recovery_task_entry(void *arg);

    void handle_event(esp_event_base_t event_base, int32_t event_id,
                      void *event_data);
    void recovery_loop();
    // Refresh cached STA SSID/RSSI via one esp_wifi_sta_get_ap_info() RPC,
    // throttled to kApInfoRefreshMs. Called only from recovery_loop.
    void refresh_ap_info();

    esp_err_t configure_station(const WifiCredentials &credentials);
    // Fills the SoftAP half of a wifi_config_t from the shared SSID constant
    // and the AP password stored in NVS (falling back to the documented
    // default when none is set or it is too short for WPA2).
    void fill_ap_config(wifi_config_t &config) const;
    esp_err_t enable_access_point();
    // Start in STA-only mode (no SoftAP). Used on initial boot and when
    // updating credentials. The AP is raised by recovery_loop after 2 min offline.
    esp_err_t enable_station_only(const WifiCredentials &credentials);
    // Bring the radio up in AP+STA: SoftAP beacons while the station retries.
    // Called by recovery_loop after kApFallbackMs without a connection.
    esp_err_t enable_ap_sta(const WifiCredentials &credentials);
    // Switch from APSTA back to pure STA after the station reconnects.
    esp_err_t disable_access_point();
    void request_connect();
};
