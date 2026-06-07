#include "storage.hpp"

#include <cstring>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "storage";
constexpr char kNamespace[] = "gps_base";

}  // namespace

Storage::~Storage() {
    close();
}

esp_err_t Storage::open() {
    if (open_) return ESP_OK;
    ESP_RETURN_ON_ERROR(
        nvs_open(kNamespace, NVS_READWRITE, &handle_),
        kTag, "Failed to open NVS namespace");
    open_ = true;
    return ESP_OK;
}

void Storage::close() {
    if (!open_) return;
    nvs_close(handle_);
    handle_ = 0;
    open_ = false;
}

BasePosition Storage::load_position() const {
    BasePosition position;
    position.valid = get_bool("pos_valid", false);
    position.lat = get_double("lat", 0);
    position.lon = get_double("lon", 0);
    position.height = get_double("hgt", 0);
    return position;
}

esp_err_t Storage::save_position(double lat, double lon, double height) {
    ESP_RETURN_ON_ERROR(set_double("lat", lat), kTag, "Latitude save failed");
    ESP_RETURN_ON_ERROR(set_double("lon", lon), kTag, "Longitude save failed");
    ESP_RETURN_ON_ERROR(set_double("hgt", height), kTag, "Height save failed");
    ESP_RETURN_ON_ERROR(set_bool("pos_valid", true), kTag, "Position flag failed");
    return commit();
}

esp_err_t Storage::clear_position() {
    ESP_RETURN_ON_ERROR(set_bool("pos_valid", false), kTag, "Position clear failed");
    return commit();
}

bool Storage::admin_password_set() const {
    return get_bool("admin_set", false);
}

std::string Storage::admin_password() const {
    return get_string("admin_pw");
}

esp_err_t Storage::save_admin_password(const std::string &password) {
    ESP_RETURN_ON_ERROR(set_string("admin_pw", password), kTag, "Password save failed");
    ESP_RETURN_ON_ERROR(set_bool("admin_set", true), kTag, "Password flag failed");
    return commit();
}

ServiceCredentials Storage::load_service(const char *service) const {
    return {
        get_string(service_key(service, "_mp").c_str()),
        get_string(service_key(service, "_pw").c_str()),
    };
}

esp_err_t Storage::save_service(
    const char *service, const ServiceCredentials &credentials) {
    ESP_RETURN_ON_ERROR(
        set_string(service_key(service, "_mp").c_str(), credentials.mountpoint),
        kTag, "Mountpoint save failed");
    ESP_RETURN_ON_ERROR(
        set_string(service_key(service, "_pw").c_str(), credentials.password),
        kTag, "Service password save failed");
    return commit();
}

bool Storage::service_enabled(const char *service, bool default_value) const {
    return get_bool(service_key(service, "_en").c_str(), default_value);
}

esp_err_t Storage::set_service_enabled(const char *service, bool enabled) {
    ESP_RETURN_ON_ERROR(
        set_bool(service_key(service, "_en").c_str(), enabled),
        kTag, "Service flag save failed");
    return commit();
}

WifiCredentials Storage::load_wifi() const {
    WifiCredentials credentials;
    credentials.ssid = get_string("wifi_ssid");
    credentials.password = get_string("wifi_pw");
    credentials.valid = !credentials.ssid.empty();
    return credentials;
}

esp_err_t Storage::save_wifi(const WifiCredentials &credentials) {
    ESP_RETURN_ON_ERROR(
        set_string("wifi_ssid", credentials.ssid), kTag, "WiFi SSID save failed");
    ESP_RETURN_ON_ERROR(
        set_string("wifi_pw", credentials.password), kTag, "WiFi password save failed");
    return commit();
}

bool Storage::get_bool(const char *key, bool default_value) const {
    if (!open_) return default_value;
    uint8_t value = default_value ? 1 : 0;
    if (nvs_get_u8(handle_, key, &value) != ESP_OK) return default_value;
    return value == 1;
}

std::string Storage::get_string(const char *key) const {
    if (!open_) return {};
    size_t length = 0;
    if (nvs_get_str(handle_, key, nullptr, &length) != ESP_OK || length == 0) {
        return {};
    }

    std::vector<char> buffer(length);
    if (nvs_get_str(handle_, key, buffer.data(), &length) != ESP_OK) return {};
    return std::string(buffer.data());
}

double Storage::get_double(const char *key, double default_value) const {
    if (!open_) return default_value;
    double value = default_value;
    size_t length = sizeof(value);
    if (nvs_get_blob(handle_, key, &value, &length) != ESP_OK ||
        length != sizeof(value)) {
        return default_value;
    }
    return value;
}

esp_err_t Storage::set_bool(const char *key, bool value) {
    if (!open_) return ESP_ERR_INVALID_STATE;
    return nvs_set_u8(handle_, key, value ? 1 : 0);
}

esp_err_t Storage::set_string(const char *key, const std::string &value) {
    if (!open_) return ESP_ERR_INVALID_STATE;
    return nvs_set_str(handle_, key, value.c_str());
}

esp_err_t Storage::set_double(const char *key, double value) {
    if (!open_) return ESP_ERR_INVALID_STATE;
    return nvs_set_blob(handle_, key, &value, sizeof(value));
}

esp_err_t Storage::commit() {
    if (!open_) return ESP_ERR_INVALID_STATE;
    return nvs_commit(handle_);
}

std::string Storage::service_key(const char *service, const char *suffix) {
    return std::string(service) + suffix;
}
