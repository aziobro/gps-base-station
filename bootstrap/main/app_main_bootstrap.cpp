#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "storage.hpp"
#include "web_server_bootstrap.hpp"
#include "wifi_manager.hpp"

namespace {

constexpr char kTag[] = "gps_bootstrap";

esp_err_t init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "NVS erase failed");
        err = nvs_flash_init();
    }
    return err;
}

void validate_ota_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    if (running &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(kTag, "Bootstrap OTA image marked valid");
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(kTag, "GPS Bootstrap firmware starting");
    ESP_ERROR_CHECK(init_nvs());

    static Storage storage;
    ESP_ERROR_CHECK(storage.open());

    static WifiManager wifi_manager;
    ESP_ERROR_CHECK(wifi_manager.start(storage));
    ESP_LOGI(kTag, "WiFi: %s", wifi_manager.connected() ? "connected" : "AP mode");

    static BootstrapWebServer web_server;
    ESP_ERROR_CHECK(web_server.start(storage, wifi_manager));
    ESP_LOGI(kTag, "Bootstrap web server ready — navigate to https://<ip>/update");

    ESP_ERROR_CHECK(
        xTaskCreate(validate_ota_task, "ota_validate", 3072, nullptr, 3, nullptr)
            == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
