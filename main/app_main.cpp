#include "driver/gpio.h"
#include "driver/uart.h"
#include "base_station.hpp"
#include "display.hpp"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "log_buffer.hpp"
#include "nvs_flash.h"
#include "sd_delete.hpp"
#include "sd_manager.hpp"
#include "storage.hpp"
#include "ui.hpp"
#include "wifi_manager.hpp"
#include "web_server.hpp"

namespace {

constexpr char kTag[] = "gps_base";
constexpr int kUm980Baud = 115200;

// ESP32-P4-WIFI6-Touch-LCD-4B pin assignment (P3 header)
// UM980 COM2: TX2->GPIO2/P3-pin5 (ESP RX), RX2->GPIO3/P3-pin7 (ESP TX)
// UM980 COM3: TX3->GPIO21/P3-pin10 (ESP RX), RX3->GPIO22/P3-pin12 (ESP TX)
// NOTE: GPIO16/17 reserved for P4<->C6 WiFi coprocessor UART — do not use.
// NOTE: GPIO37/38 are wired to the onboard CH343P USB-UART chip — do not use.
constexpr uart_port_t kCommandUart = UART_NUM_1;
constexpr gpio_num_t kCommandRx = GPIO_NUM_2;   // UM980 COM2 TX2 -> P3 pin 5
constexpr gpio_num_t kCommandTx = GPIO_NUM_3;   // UM980 COM2 RX2 <- P3 pin 7

constexpr uart_port_t kDataUart = UART_NUM_2;
constexpr gpio_num_t kDataRx = GPIO_NUM_21;  // UM980 COM3 TX3
constexpr gpio_num_t kDataTx = GPIO_NUM_22;  // UM980 COM3 RX3

// Power the shared 3.3 V I/O rail (LDO channel 4 / VDDPST_5) that feeds the
// microSD card and the display I/O. Acquired exactly once here so there is a
// single owner — the SD driver and the display BSP both rely on it already
// being up rather than acquiring it themselves (which previously collided and
// logged "can't acquire the channel"). Held for the lifetime of the program.
void enable_io_rail() {
    static esp_ldo_channel_handle_t vo4 = nullptr;
    if (vo4) return;
    esp_ldo_channel_config_t cfg = { .chan_id = 4, .voltage_mv = 3300, .flags = {} };
    esp_err_t err = esp_ldo_acquire_channel(&cfg, &vo4);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "LDO VO4 acquire failed: %s (assuming already supplied)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "LDO VO4 (3.3 V I/O rail) enabled");
    }
}

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
        .rx_glitch_filt_thresh = 0,
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

const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic/exception";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "unknown";
    }
}

void enable_rtk_task(void *argument) {
    vTaskDelay(pdMS_TO_TICKS(20000));
    auto *station = static_cast<BaseStation *>(argument);
    if (station) {
        // Honor the persisted global NTRIP on/off rather than forcing on.
        station->apply_persisted_streams();
        // Resume RINEX collection if it was on before the last reboot (SD is
        // mounted well before this 20 s window elapses).
        station->resume_persisted_rinex();
        ESP_LOGI(kTag, "RTK services restored to persisted state after startup window");
    }
    vTaskDelete(nullptr);
}

struct NetworkGateArgs {
    WifiManager *wifi = nullptr;
    BaseStation *station = nullptr;
};

void network_gate_task(void *argument) {
    auto *args = static_cast<NetworkGateArgs *>(argument);
    bool last_available = false;
    bool initialized = false;
    while (args && args->wifi && args->station) {
        const bool available = args->wifi->connected();
        if (!initialized || available != last_available) {
            args->station->set_network_available(available);
            last_available = available;
            initialized = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main() {
    log_buffer::init();  // capture console output for the /logs web page
    enable_io_rail();  // power VDDPST_5 / SD + display I/O before anything uses it
    ESP_LOGW(kTag, "Boot — last reset reason: %s", reset_reason_str(esp_reset_reason()));
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
    base_station.set_network_available(wifi_manager.connected());

    static SdManager sd_manager;
    if (sd_manager.mount() == ESP_OK) {
        sd_manager.ensure_dirs();
    } else {
        ESP_LOGW(kTag, "SD card not available — file browser will show empty state");
    }

    // Wire the shared single-job bulk-delete executor (used by both UIs).
    sd_delete::bind(&sd_manager, &base_station);

    static Display display;
    if (display.init() == ESP_OK) {
        static Ui ui;
        if (ui.init(display, base_station, sd_manager, wifi_manager, storage) != ESP_OK) {
            ESP_LOGW(kTag, "UI init failed — display will show blank");
        }
    } else {
        ESP_LOGW(kTag, "Display init failed — touchscreen unavailable");
    }

    static AdminWebServer web_server;
    ESP_ERROR_CHECK(web_server.start(storage, wifi_manager, base_station, sd_manager));
    ESP_LOGI(kTag, "Administration server ready; RTK services held briefly");

    ESP_ERROR_CHECK(
        xTaskCreate(
            enable_rtk_task, "rtk_enable", 2048, &base_station, 3, nullptr)
            == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    static NetworkGateArgs network_gate_args{&wifi_manager, &base_station};
    ESP_ERROR_CHECK(
        xTaskCreate(
            network_gate_task, "network_gate", 2048, &network_gate_args, 3,
            nullptr)
            == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(
        xTaskCreate(
            validate_ota_task, "ota_validate", 3072, &base_station, 3, nullptr)
            == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
