#include "driver/gpio.h"
#include "driver/uart.h"
#include "base_station.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "storage.hpp"
#include "wifi_manager.hpp"
#include "web_server.hpp"

namespace {

constexpr char kTag[] = "gps_base";
constexpr int kUm980Baud = 115200;

constexpr uart_port_t kCommandUart = UART_NUM_1;
constexpr gpio_num_t kCommandRx = GPIO_NUM_18;
constexpr gpio_num_t kCommandTx = GPIO_NUM_19;

constexpr uart_port_t kDataUart = UART_NUM_2;
constexpr gpio_num_t kDataRx = GPIO_NUM_16;
constexpr gpio_num_t kDataTx = GPIO_NUM_17;

esp_err_t init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "NVS erase failed");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t init_uart(uart_port_t port, gpio_num_t rx, gpio_num_t tx,
                    int rx_buffer_size) {
    const uart_config_t config = {
        .baud_rate = kUm980Baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    ESP_RETURN_ON_ERROR(
        uart_driver_install(port, rx_buffer_size, 0, 0, nullptr, 0),
        kTag, "UART driver install failed");
    ESP_RETURN_ON_ERROR(
        uart_param_config(port, &config),
        kTag, "UART configuration failed");
    return uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void validate_ota_task(void *argument) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    auto *station = static_cast<BaseStation *>(argument);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    if (running &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (station && station->healthy()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_mark_app_valid_cancel_rollback());
            ESP_LOGI(kTag, "OTA image marked valid after 30-second health window");
        } else {
            ESP_LOGE(kTag, "Base task unhealthy; restarting without OTA validation");
            esp_restart();
        }
    }
    vTaskDelete(nullptr);
}

void enable_rtk_task(void *argument) {
    vTaskDelay(pdMS_TO_TICKS(20000));
    auto *station = static_cast<BaseStation *>(argument);
    if (station) {
        station->set_streams_suspended(false);
        ESP_LOGI(kTag, "RTK services enabled after web startup window");
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main() {
    ESP_ERROR_CHECK(init_nvs());
    static Storage storage;
    ESP_ERROR_CHECK(storage.open());
    ESP_ERROR_CHECK(init_uart(kCommandUart, kCommandRx, kCommandTx, 2048));
    ESP_ERROR_CHECK(init_uart(kDataUart, kDataRx, kDataTx, 4096));

    ESP_LOGI(kTag, "ESP-IDF migration firmware booted");
    ESP_LOGI(kTag, "UM980 COM2: RX=%d TX=%d, COM3: RX=%d TX=%d",
             kCommandRx, kCommandTx, kDataRx, kDataTx);

    const BasePosition position = storage.load_position();
    const WifiCredentials wifi = storage.load_wifi();
    ESP_LOGI(kTag, "Stored position: %s, WiFi credentials: %s",
             position.valid ? "valid" : "none",
             wifi.valid ? "present" : "none");

    static WifiManager wifi_manager;
    ESP_ERROR_CHECK(wifi_manager.start(storage));
    ESP_LOGI(kTag, "WiFi state: %s",
             wifi_manager.connected() ? "connected" : "AP fallback");

    static BaseStation base_station(storage, kCommandUart, kDataUart);
    // Reserve the RTK task resources while keeping every data stream offline.
    // HTTPS is brought up before outbound or local RTCM transmission begins.
    base_station.set_streams_suspended(true);
    ESP_ERROR_CHECK(base_station.start());

    static AdminWebServer web_server;
    ESP_ERROR_CHECK(web_server.start(storage, wifi_manager, base_station));
    ESP_LOGI(kTag, "Administration server ready; RTK services held briefly");

    ESP_ERROR_CHECK(
        xTaskCreate(
            enable_rtk_task, "rtk_enable", 2048, &base_station, 3, nullptr)
            == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(
        xTaskCreate(
            validate_ota_task, "ota_validate", 3072, &base_station, 3, nullptr)
            == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
