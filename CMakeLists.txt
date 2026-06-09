cmake_minimum_required(VERSION 3.16)

# Single source of truth for the firmware version lives in version.txt.
# ESP-IDF reads it automatically (when PROJECT_VER is unset), embeds it in the
# app descriptor, and it surfaces via esp_app_get_description()->version — the
# same string served at /status and shown on the web status page. Bump it with
# tools/release.sh; never hard-code the version here.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gps_base_station)
