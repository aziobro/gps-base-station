#include "um980.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "app_config.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "um980";
constexpr TickType_t kCommandDelay = pdMS_TO_TICKS(200);

}  // namespace

esp_err_t Um980::command(const char *text) {
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    const int text_length = strlen(text);
    ESP_LOGI(kTag, "TX> %s", text);  // DIAGNOSTIC
    if (uart_write_bytes(command_uart_, text, text_length) != text_length ||
        uart_write_bytes(command_uart_, "\r\n", 2) != 2) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(
        uart_wait_tx_done(command_uart_, pdMS_TO_TICKS(500)),
        kTag, "Command transmit failed");
    vTaskDelay(kCommandDelay);
    return ESP_OK;
}

esp_err_t Um980::commandf(const char *format, ...) {
    char buffer[160];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || length >= static_cast<int>(sizeof(buffer))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return command(buffer);
}

esp_err_t Um980::stop_output() {
    ESP_RETURN_ON_ERROR(command("UNLOGALL COM2"), kTag, "COM2 stop failed");
    return command("UNLOGALL COM3");
}

esp_err_t Um980::configure_satellite_output() {
    ESP_RETURN_ON_ERROR(
        command("LOG COM2 GNGSA ONTIME 10"), kTag, "GSA log failed");
    ESP_RETURN_ON_ERROR(
        command("LOG COM2 GPGSV ONTIME 10"), kTag, "GPS GSV log failed");
    ESP_RETURN_ON_ERROR(
        command("LOG COM2 GLGSV ONTIME 10"), kTag, "GLONASS GSV log failed");
    ESP_RETURN_ON_ERROR(
        command("LOG COM2 GAGSV ONTIME 10"), kTag, "Galileo GSV log failed");
    return command("LOG COM2 GBGSV ONTIME 10");
}

esp_err_t Um980::configure_survey_output() {
    ESP_RETURN_ON_ERROR(stop_output(), kTag, "Output reset failed");
    ESP_RETURN_ON_ERROR(
        command("LOG COM2 BESTPOSA ONTIME 5"), kTag, "BESTPOS log failed");
    return configure_satellite_output();
}

esp_err_t Um980::configure_raw_output() {
    ESP_RETURN_ON_ERROR(command("UNLOGALL COM3"), kTag, "COM3 stop failed");
    // RANGEA every 30 seconds — keep satellite status on COM2.
    ESP_RETURN_ON_ERROR(
        command("LOG COM3 RANGEA ONTIME 30"), kTag, "RANGEA log failed");
    ESP_LOGI(kTag, "COM3 switched to RANGEA (raw collection mode)");
    return ESP_OK;
}

esp_err_t Um980::configure_signal_group() {
    // Per the Unicore N4 reference manual (SIGNALGROUP, section 4.23): the
    // module resets automatically when this value actually changes, and the
    // new value is saved automatically (no SAVECONFIG needed). Re-issuing
    // the same value on a later call is a no-op -- no reset, no delay cost.
    // Give it generous settling time before the caller sends anything else;
    // the manual doesn't state a reboot duration, so this errs long.
    ESP_RETURN_ON_ERROR(
        command("CONFIG SIGNALGROUP 2"), kTag, "SIGNALGROUP failed");
    vTaskDelay(pdMS_TO_TICKS(5000));
    return ESP_OK;
}

esp_err_t Um980::configure_base(double lat, double lon, double height) {
    ESP_RETURN_ON_ERROR(stop_output(), kTag, "Output reset failed");
    ESP_RETURN_ON_ERROR(
        commandf("CONFIG BASE GEODETIC %.8f %.8f %.4f", lat, lon, height),
        kTag, "Base position failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1005 ONTIME %d",
                 config::kRtcmBasePositionRateSec),
        kTag, "RTCM1005 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1077 ONTIME %d", config::kRtcmGpsRateSec),
        kTag, "RTCM1077 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1087 ONTIME %d", config::kRtcmGlonassRateSec),
        kTag, "RTCM1087 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1097 ONTIME %d", config::kRtcmGalileoRateSec),
        kTag, "RTCM1097 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1117 ONTIME %d", config::kRtcmQzssRateSec),
        kTag, "RTCM1117 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1127 ONTIME %d", config::kRtcmBeidouRateSec),
        kTag, "RTCM1127 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1137 ONTIME %d", config::kRtcmNavicRateSec),
        kTag, "RTCM1137 failed");
    ESP_RETURN_ON_ERROR(
        commandf("LOG COM3 RTCM1230 ONTIME %d", config::kRtcmGlonassBiasRateSec),
        kTag, "RTCM1230 failed");
    ESP_RETURN_ON_ERROR(
        configure_satellite_output(), kTag, "Satellite output failed");
    ESP_RETURN_ON_ERROR(command("SAVECONFIG"), kTag, "Save config failed");
    ESP_LOGI(kTag, "Base configured at %.8f, %.8f, %.4f", lat, lon, height);
    return ESP_OK;
}
