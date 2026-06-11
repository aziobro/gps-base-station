#include "wifi_manager.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_check.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "wifi";

int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

// ESP32-P4 hosted WiFi can't read MAC from the C6 coprocessor, so derive a
// locally-administered MAC from the chip's own eFuse ID instead.
void set_wifi_mac_from_efuse(wifi_interface_t iface) {
    uint8_t base[6] = {};
    esp_efuse_mac_get_default(base);
    base[0] = (base[0] & 0xFE) | 0x02;  // locally administered, unicast
    if (iface == WIFI_IF_AP) base[5] ^= 0x01;  // differentiate AP from STA
    esp_err_t err = esp_wifi_set_mac(iface, base);
    ESP_LOGI(kTag, "MAC %s set %02x:%02x:%02x:%02x:%02x:%02x (%s)",
             iface == WIFI_IF_AP ? "AP" : "STA",
             base[0], base[1], base[2], base[3], base[4], base[5],
             err == ESP_OK ? "ok" : esp_err_to_name(err));
}

}  // namespace

WifiManager::~WifiManager() {
    stopping_ = true;
    if (recovery_task_) {
        while (recovery_task_) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (wifi_handler_) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_);
    }
    if (ip_handler_) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
    }
    if (events_) vEventGroupDelete(events_);
}

esp_err_t WifiManager::start(Storage &storage) {
    storage_ = &storage;
    events_ = xEventGroupCreate();
    if (!events_) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "Network stack init failed");
    esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
        return loop_result;
    }

    station_netif_ = esp_netif_create_default_wifi_sta();
    access_point_netif_ = esp_netif_create_default_wifi_ap();
    if (!station_netif_ || !access_point_netif_) return ESP_ERR_NO_MEM;

    // Bring up the esp_hosted transport to the ESP32-C6 WiFi coprocessor over
    // SDIO before initialising WiFi. Required on ESP32-P4 (no native radio).
    ESP_RETURN_ON_ERROR(esp_hosted_init(), kTag, "esp_hosted init failed");
    ESP_RETURN_ON_ERROR(
        esp_hosted_connect_to_slave(), kTag, "esp_hosted slave connect failed");

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), kTag, "WiFi init failed");
    // This firmware owns credential persistence via Storage; keep the driver's
    // own config in RAM so it never reloads stale settings from NVS.
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, this, &wifi_handler_),
        kTag, "WiFi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, this, &ip_handler_),
        kTag, "IP event registration failed");

    const WifiCredentials credentials = storage.load_wifi();
    if (!credentials.valid) {
        ESP_LOGW(kTag, "No saved WiFi credentials; starting access point");
        ESP_RETURN_ON_ERROR(enable_access_point(), kTag, "AP start failed");
    } else {
        // Run AP + station together so the GPS-BaseStation SoftAP stays
        // reachable for configuration whether or not the saved network is up.
        ESP_RETURN_ON_ERROR(
            enable_ap_sta(credentials), kTag, "AP+STA start failed");
    }

    if (xTaskCreate(
            recovery_task_entry, "wifi_recovery", 4096, this, 4,
            &recovery_task_) != pdPASS) {
        recovery_task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

WifiManager::State WifiManager::state() const {
    if (connected_) return State::kConnected;
    if (access_point_active_) return State::kAccessPoint;
    return State::kDisconnected;
}

bool WifiManager::connected() const {
    return connected_;
}

bool WifiManager::access_point_active() const {
    return access_point_active_;
}

int WifiManager::rssi() const {
    if (!connected_) return -127;
    wifi_ap_record_t record{};
    if (esp_wifi_sta_get_ap_info(&record) != ESP_OK) return -127;
    return record.rssi;
}

std::string WifiManager::ssid() const {
    if (!connected_) {
        return access_point_active_ ? kAccessPointSsid : "";
    }
    wifi_ap_record_t record{};
    if (esp_wifi_sta_get_ap_info(&record) != ESP_OK) return {};
    return reinterpret_cast<const char *>(record.ssid);
}

std::string WifiManager::ip_address() const {
    if (!connected_ || !station_netif_) return {};
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(station_netif_, &info) != ESP_OK) return {};
    char address[16]{};
    snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
    return address;
}

std::string WifiManager::access_point_ssid() const {
    return access_point_active_ ? std::string(kAccessPointSsid) : std::string();
}

std::string WifiManager::access_point_ip() const {
    if (!access_point_active_ || !access_point_netif_) return {};
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(access_point_netif_, &info) != ESP_OK) return {};
    char address[16]{};
    snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
    return address;
}

std::vector<WifiNetwork> WifiManager::scan_networks() {
    wifi_scan_config_t scan{};
    scan.show_hidden = false;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return {};
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) return {};
    count = std::min<uint16_t>(count, 24);
    std::vector<wifi_ap_record_t> records(count);
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) return {};
    std::vector<WifiNetwork> networks;
    networks.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const std::string ssid(
            reinterpret_cast<const char *>(records[i].ssid));
        if (ssid.empty()) continue;
        auto existing = std::find_if(
            networks.begin(), networks.end(),
            [&ssid](const WifiNetwork &network) {
                return network.ssid == ssid;
            });
        if (existing == networks.end()) {
            networks.push_back({
                ssid,
                records[i].rssi,
                records[i].authmode != WIFI_AUTH_OPEN,
            });
        } else if (records[i].rssi > existing->rssi) {
            existing->rssi = records[i].rssi;
            existing->secured = records[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    std::sort(
        networks.begin(), networks.end(),
        [](const WifiNetwork &left, const WifiNetwork &right) {
            return left.rssi > right.rssi;
        });
    return networks;
}

void WifiManager::event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    static_cast<WifiManager *>(arg)->handle_event(
        event_base, event_id, event_data);
}

void WifiManager::handle_event(esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        connected_ = true;
        disconnected_at_ms_ = 0;
        xEventGroupSetBits(events_, kConnectedBit);
        ESP_LOGI(kTag, "Station connected: " IPSTR, IP2STR(&event->ip_info.ip));
        // In AP+STA mode the SoftAP stays up deliberately; do not tear it down.
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = connected_.exchange(false);
        xEventGroupClearBits(events_, kConnectedBit);
        if (disconnected_at_ms_ == 0) disconnected_at_ms_ = now_ms();
        if (was_connected) {
            last_retry_ms_ = now_ms() - kRetryIntervalMs;
            ESP_LOGW(kTag, "Station connection lost");
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(kTag, "SoftAP started — beaconing as %s", kAccessPointSsid);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(kTag, "AP client connected");
        return;
    }
}

void WifiManager::recovery_task_entry(void *arg) {
    auto *manager = static_cast<WifiManager *>(arg);
    manager->recovery_loop();
    manager->recovery_task_ = nullptr;
    vTaskDelete(nullptr);
}

void WifiManager::recovery_loop() {
    while (!stopping_) {
        const int64_t now = now_ms();
        // In AP+STA the SoftAP is always beaconing; we only need to keep nudging
        // the station to (re)associate with the saved network while it is down.
        const bool have_creds = storage_ && storage_->load_wifi().valid;
        if (have_creds && !connected_ && now - last_retry_ms_ >= kRetryIntervalMs) {
            ESP_LOGI(kTag, "Retrying station association");
            last_retry_ms_ = now;
            request_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t WifiManager::configure_station(
    const WifiCredentials &credentials) {
    wifi_config_t config{};
    strlcpy(reinterpret_cast<char *>(config.sta.ssid),
            credentials.ssid.c_str(), sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(config.sta.password),
            credentials.password.c_str(), sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

esp_err_t WifiManager::enable_access_point() {
    wifi_config_t config{};
    strlcpy(reinterpret_cast<char *>(config.ap.ssid),
            kAccessPointSsid, sizeof(config.ap.ssid));
    config.ap.ssid_len = strlen(kAccessPointSsid);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;

    // Pure AP — used only when no station credentials are stored. Stop any
    // running mode first, then bring the interface up as a dedicated AP.
    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), kTag, "Get mode failed");
    if (mode != WIFI_MODE_NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    }

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_AP), kTag, "AP mode failed");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_AP, &config), kTag, "AP config failed");
    set_wifi_mac_from_efuse(WIFI_IF_AP);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "WiFi start failed");

    access_point_active_ = true;
    connected_ = false;
    last_retry_ms_ = now_ms();
    ESP_LOGI(kTag, "AP active: %s at 192.168.4.1", kAccessPointSsid);
    return ESP_OK;
}

// Bring the radio up in AP+STA: the SoftAP beacons continuously while the
// station associates with the saved network. (Historically the ESP32-C6
// hosted radio would not beacon in APSTA — this path re-tests that now that
// the SDIO link is stable.)
esp_err_t WifiManager::enable_ap_sta(const WifiCredentials &credentials) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), kTag, "Get mode failed");
    if (mode != WIFI_MODE_NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    }

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "APSTA mode failed");

    wifi_config_t ap_config{};
    strlcpy(reinterpret_cast<char *>(ap_config.ap.ssid),
            kAccessPointSsid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(kAccessPointSsid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "AP config failed");
    ESP_RETURN_ON_ERROR(
        configure_station(credentials), kTag, "Station config failed");
    set_wifi_mac_from_efuse(WIFI_IF_AP);
    set_wifi_mac_from_efuse(WIFI_IF_STA);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "WiFi start failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));

    access_point_active_ = true;
    connected_ = false;
    disconnected_at_ms_ = 0;
    last_retry_ms_ = 0;
    request_connect();
    ESP_LOGI(kTag, "AP+STA active: AP %s + station \"%s\"",
             kAccessPointSsid, credentials.ssid.c_str());
    return ESP_OK;
}

esp_err_t WifiManager::update_credentials(const WifiCredentials &credentials) {
    esp_wifi_disconnect();
    // Bring the radio back up in AP+STA so the SoftAP stays reachable while the
    // newly entered network is attempted.
    return enable_ap_sta(credentials);
}

void WifiManager::request_connect() {
    esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(kTag, "Connect request failed: %s", esp_err_to_name(result));
    }
}
