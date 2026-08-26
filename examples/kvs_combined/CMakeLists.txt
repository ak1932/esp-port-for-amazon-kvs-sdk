# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# for usrsctp submodule
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF v6 moved hal/spi_flash_types.h into the non-common esp_hal_mspi
# component; espressif/network_provisioning 1.2.4 (pulled in by
# CONFIG_APP_NETWORK_PROV_BLE) includes esp_flash.h without declaring the
# dependency. No-op on 5.4/5.5.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/idf6_compat.cmake)

project(kvs_combined)
