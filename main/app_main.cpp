#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

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

}  // namespace

extern "C" void app_main() {
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_uart(kCommandUart, kCommandRx, kCommandTx, 2048));
    ESP_ERROR_CHECK(init_uart(kDataUart, kDataRx, kDataTx, 4096));

    ESP_LOGI(kTag, "ESP-IDF migration firmware booted");
    ESP_LOGI(kTag, "UM980 COM2: RX=%d TX=%d, COM3: RX=%d TX=%d",
             kCommandRx, kCommandTx, kDataRx, kDataTx);
}
