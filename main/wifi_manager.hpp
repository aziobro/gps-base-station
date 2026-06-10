#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
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
    std::vector<WifiNetwork> scan_networks();
    esp_err_t update_credentials(const WifiCredentials &credentials);

private:
    static constexpr EventBits_t kConnectedBit = BIT0;
    static constexpr uint32_t kInitialConnectTimeoutMs = 8000;
    static constexpr uint32_t kAccessPointDelayMs = 8000;
    static constexpr uint32_t kRetryIntervalMs = 10000;
    static constexpr char kAccessPointSsid[] = "GPS-BaseStation";

    Storage *storage_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t recovery_task_ = nullptr;
    esp_netif_t *station_netif_ = nullptr;
    esp_netif_t *access_point_netif_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;

    std::atomic<bool> connected_{false};
    std::atomic<bool> access_point_active_{false};
    std::atomic<int64_t> disconnected_at_ms_{0};
    std::atomic<int64_t> last_retry_ms_{0};
    std::atomic<bool> stopping_{false};

    static void event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
    static void recovery_task_entry(void *arg);

    void handle_event(esp_event_base_t event_base, int32_t event_id,
                      void *event_data);
    void recovery_loop();

    esp_err_t configure_station(const WifiCredentials &credentials);
    esp_err_t enable_access_point();
    void request_connect();
};
