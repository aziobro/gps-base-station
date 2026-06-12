cmake_minimum_required(VERSION 3.16)
# Reuse already-downloaded managed components from the parent project.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gps_bootstrap)
