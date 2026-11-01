cmake_minimum_required(VERSION 3.16)

# This wrapper deliberately uses the ESP-Hosted source already resolved by the
# P4 application.  Keeping host and C6 source at the same component revision
# prevents protocol-version drift between the two chips.
set(ESP_HOSTED_SLAVE_MAIN
    "${CMAKE_CURRENT_LIST_DIR}/../p4_main/managed_components/espressif__esp_hosted/slave/main")

if(NOT IS_DIRECTORY "${ESP_HOSTED_SLAVE_MAIN}")
    message(FATAL_ERROR "ESP-Hosted slave source is missing: ${ESP_HOSTED_SLAVE_MAIN}")
endif()

set(EXTRA_COMPONENT_DIRS "${ESP_HOSTED_SLAVE_MAIN}")
set(PROJECT_VER "2.12.13")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXCLUDE_COMPONENTS lwip esp_netif)
set(COMPONENTS
    esp_timer esptool_py bootloader main nvs_flash esp_rom esp_wifi protocomm
    driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio esp_pm
    openthread app_update)
project(network_adapter)
