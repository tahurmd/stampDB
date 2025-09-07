#
# @file platform/pico/CMakeLists.txt
# @brief Pico port: platform glue library, firmware UF2 and LittleFS integration.
#
# Targets:
#  - stampdb_pico_port: platform glue for flash ops (__not_in_flash_func)
#  - stampdb_pico_fw: example firmware producing UF2
#  - littlefs: fetched as a static lib and linked for metadata
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

# LittleFS fetch & integration for Pico metadata
include(FetchContent)
FetchContent_Declare(
  littlefs
  GIT_REPOSITORY https://github.com/littlefs-project/littlefs.git
  GIT_TAG v2.9.2
)
FetchContent_MakeAvailable(littlefs)
add_library(littlefs STATIC ${littlefs_SOURCE_DIR}/lfs.c ${littlefs_SOURCE_DIR}/lfs_util.c)
target_include_directories(littlefs PUBLIC ${littlefs_SOURCE_DIR})
target_link_libraries(stampdb PRIVATE littlefs)
