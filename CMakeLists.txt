##
## @file CMakeLists.txt
## @brief Top-level build: core library, shared library, CLI tools, and tests.
##
## Targets:
##  - stampdb: static core library (host sim or Pico port)
##  - stampdb_shared: shared library for Python ctypes
##  - stampctl: CLI exporter/retention tool
##  - tests: CTest suite in tests/
##
cmake_minimum_required(VERSION 3.20)
project(stampdb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(STAMPDB_PLATFORM_SIM "Build for host simulator platform" ON)
set(STAMPDB_META_RESERVED_BYTES "32768" CACHE STRING "Bytes reserved for metadata region at top of flash")

add_library(stampdb
  src/stampdb.c
  src/codec.c
  src/crc32c.c
  src/ring.c
  src/recovery.c
  src/read_iter.c
  src/meta_lfs.c
)

target_include_directories(stampdb PUBLIC include PRIVATE src)
if(STAMPDB_META_RESERVED_BYTES MATCHES "^[0-9]+$")
  add_compile_definitions(STAMPDB_META_RESERVED=${STAMPDB_META_RESERVED_BYTES})
endif()

if(STAMPDB_PLATFORM_SIM)
  target_sources(stampdb PRIVATE sim/flash.c sim/platform_sim.c)
  target_compile_definitions(stampdb PRIVATE STAMPDB_PLATFORM_SIM=1)
else()
  add_subdirectory(platform/pico)
  target_link_libraries(stampdb PRIVATE stampdb_pico_port)
endif()

add_executable(stampctl tools/stampctl.c)
target_link_libraries(stampctl PRIVATE stampdb)

enable_testing()
add_subdirectory(tests)

# Shared library for Python ctypes
add_library(stampdb_shared SHARED
  src/stampdb.c
  src/codec.c
  src/crc32c.c
  src/ring.c
  src/recovery.c
  src/read_iter.c
  src/meta_lfs.c
)
target_include_directories(stampdb_shared PUBLIC include PRIVATE src)
if(STAMPDB_PLATFORM_SIM)
  target_sources(stampdb_shared PRIVATE sim/flash.c sim/platform_sim.c)
  target_compile_definitions(stampdb_shared PRIVATE STAMPDB_PLATFORM_SIM=1)
else()
  target_link_libraries(stampdb_shared PRIVATE stampdb_pico_port)
endif()
