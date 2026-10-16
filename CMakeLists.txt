# Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (ESP32-S3-WROOM-1-N16R8)
cmake_minimum_required(VERSION 3.16)

# The shared board runtime: ../esp32emu
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32cpc)

# CPC ROM images (not in git) go into 16 KB slots of the cpc_roms partition
# on "idf.py flash"; the system cartridge from slot 10 on (main/rom_store.c)
partition_table_get_partition_info(rom_partition "--partition-name cpc_roms" "offset")
if(rom_partition)
    set(slot 0)
    foreach(rom os464 basic10 os664 basic664 os6128 basic11 amsdos osplus basicplus)
        set(rom_image "${CMAKE_SOURCE_DIR}/roms/${rom}.rom")
        if(EXISTS "${rom_image}")
            math(EXPR offset "${rom_partition} + ${slot} * 16384" OUTPUT_FORMAT HEXADECIMAL)
            esptool_py_flash_target_image(flash "rom_${rom}" "${offset}" "${rom_image}" ALWAYS_PLAINTEXT)
        endif()
        math(EXPR slot "${slot} + 1")
    endforeach()
    set(cart_image "${CMAKE_SOURCE_DIR}/roms/system.cpr")
    if(EXISTS "${cart_image}")
        math(EXPR offset "${rom_partition} + 10 * 16384" OUTPUT_FORMAT HEXADECIMAL)
        esptool_py_flash_target_image(flash "rom_system_cpr" "${offset}" "${cart_image}" ALWAYS_PLAINTEXT)
    endif()
endif()
