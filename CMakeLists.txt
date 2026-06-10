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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gps_base_station)
