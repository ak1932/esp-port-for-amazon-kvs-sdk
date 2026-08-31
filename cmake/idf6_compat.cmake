# ESP-IDF v6 compatibility shim. Include BEFORE project().
#
# v6 split spi_flash's public dependencies out of `hal` (a common requirement of
# every component) into new components — esp_hal_mspi (hal/spi_flash_types.h)
# and esp_blockdev (esp_blockdev.h) — which are not common. Any component that
# includes esp_flash.h without declaring spi_flash therefore stops building,
# including third-party ones we don't control (e.g. espressif/network_provisioning
# <= 1.2.4). Restore the v5.x situation by making them common too; drop this once
# those components declare the dependency themselves.
foreach(_c esp_hal_mspi esp_blockdev)
    # Test for the component's CMakeLists.txt, not just the directory: ESP-IDF v5.5
    # ships components/esp_blockdev as a headers-only directory that is NOT a
    # component, so a directory check would append an unresolvable name there.
    if(EXISTS "$ENV{IDF_PATH}/components/${_c}/CMakeLists.txt")
        idf_build_set_property(__COMPONENT_REQUIRES_COMMON ${_c} APPEND)
    endif()
endforeach()
