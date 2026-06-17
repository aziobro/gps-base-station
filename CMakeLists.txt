cmake_minimum_required(VERSION 3.16)

# Single source of truth for the firmware version lives in version.txt.
# ESP-IDF reads it automatically (when PROJECT_VER is unset), embeds it in the
# app descriptor, and it surfaces via esp_app_get_description()->version — the
# same string served at /status and shown on the web status page. Bump it with
# tools/release.sh; never hard-code the version here.

# esp_wifi_remote 1.6.1 Kconfig uses `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"`.
# IDF_VERSION is "6.0.1" but only "Kconfig.idf_v6.0.in" ships, so the orsource
# silently fails and all CONFIG_WIFI_RMT_* symbols go missing, breaking the build.
# Create a shim that delegates to the existing file when managed_components are present.
set(_WR_DIR "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_wifi_remote")
set(_WR_SHIM "${_WR_DIR}/Kconfig.idf_v6.0.1.in")
if(EXISTS "${_WR_DIR}" AND NOT EXISTS "${_WR_SHIM}")
    file(WRITE "${_WR_SHIM}" "rsource \"./Kconfig.idf_v6.0.in\"\n")
endif()

# esp_hosted 2.12.9 renamed the host-to-ESP throttle Kconfig symbol, but one
# generated host config header still checks the old name. Patch the managed
# component before configure so the sdkconfig thresholds are actually sent to
# the C6 coprocessor.
set(_HOSTED_CFG
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_hosted/host/port/esp/freertos/include/port_esp_hosted_host_config.h")
if(EXISTS "${_HOSTED_CFG}")
    file(READ "${_HOSTED_CFG}" _HOSTED_CFG_TEXT)
    string(REPLACE
        "#ifdef CONFIG_HOST_TO_ESP_WIFI_DATA_THROTTLE"
        "#if defined(CONFIG_ESP_HOSTED_HOST_TO_ESP_WIFI_DATA_THROTTLE) || \\\n    defined(CONFIG_HOST_TO_ESP_WIFI_DATA_THROTTLE)"
        _HOSTED_CFG_PATCHED
        "${_HOSTED_CFG_TEXT}")
    if(NOT _HOSTED_CFG_PATCHED STREQUAL _HOSTED_CFG_TEXT)
        file(WRITE "${_HOSTED_CFG}" "${_HOSTED_CFG_PATCHED}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gps_base_station)
