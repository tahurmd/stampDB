#
# @file platform/pico/CMakeLists.txt
# @brief Pico port: platform glue library and firmware UF2.
#
# Targets:
#  - stampdb_pico_port: platform glue for flash ops (__not_in_flash_func)
#  - stampdb_pico_fw: example firmware producing UF2
#
cmake_minimum_required(VERSION 3.20)

include(FetchContent)
if (NOT DEFINED PICO_SDK_PATH AND NOT TARGET pico_stdlib)
  FetchContent_Declare(
    pico_sdk
    GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk.git
    GIT_TAG 2.0.0
  )
  FetchContent_GetProperties(pico_sdk)
  if(NOT pico_sdk_POPULATED)
    FetchContent_Populate(pico_sdk)
    set(PICO_SDK_PATH ${pico_sdk_SOURCE_DIR})
    include(${pico_sdk_SOURCE_DIR}/pico_sdk_init.cmake)
    pico_sdk_init()
  endif()
endif()

add_library(stampdb_pico_port platform_pico.c)
target_include_directories(stampdb_pico_port PRIVATE ${PICO_SDK_PATH}/src/rp2_common/pico_platform/include)
target_link_libraries(stampdb_pico_port pico_stdlib hardware_flash pico_multicore)

add_executable(stampdb_pico_fw main.c)
target_link_libraries(stampdb_pico_fw PRIVATE stampdb stampdb_pico_port pico_stdlib)
pico_add_extra_outputs(stampdb_pico_fw)

# No external FS dependency: metadata is stored in a raw reserved flash region
