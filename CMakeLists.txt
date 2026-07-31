cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target")
set(FULLOCH_BOARD "waveshare_touch_lcd_185c_box_rev2" CACHE STRING "Fulloch board profile")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fulloch_satellite)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FULLOCH_PROVISIONING_DIR "${CMAKE_SOURCE_DIR}/provisioning" CACHE PATH
    "Directory containing device.config.json and device.credentials.json")
set(FULLOCH_PROVISIONING_CONFIG "${FULLOCH_PROVISIONING_DIR}/device.config.json" CACHE FILEPATH
    "Device configuration JSON file")
set(FULLOCH_PROVISIONING_CREDENTIALS "${FULLOCH_PROVISIONING_DIR}/device.credentials.json" CACHE FILEPATH
    "Device credential JSON file")
set(FULLOCH_NVS_IMAGE "${CMAKE_BINARY_DIR}/nvs.bin")

add_custom_command(
    OUTPUT "${FULLOCH_NVS_IMAGE}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/provision.py" stage
            --config "${FULLOCH_PROVISIONING_CONFIG}"
            --credentials "${FULLOCH_PROVISIONING_CREDENTIALS}"
            --output "${FULLOCH_NVS_IMAGE}"
            --nvs-generator "$ENV{IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
            --size 0x10000
    DEPENDS
            "${CMAKE_SOURCE_DIR}/tools/provision.py"
            "${FULLOCH_PROVISIONING_CONFIG}"
            "${FULLOCH_PROVISIONING_CREDENTIALS}"
    COMMENT "Validating device bundle and generating NVS image"
    VERBATIM
)
add_custom_target(fulloch_provisioning ALL DEPENDS "${FULLOCH_NVS_IMAGE}")
esptool_py_flash_to_partition(flash nvs "${FULLOCH_NVS_IMAGE}")
