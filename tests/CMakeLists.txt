#
# @file tests/CMakeLists.txt
# @brief CTest suite for StampDB correctness, recovery, and performance bounds.
#
add_executable(test_basic tests_basic.c)
target_link_libraries(test_basic PRIVATE stampdb)
add_test(NAME basic COMMAND test_basic)
set_tests_properties(basic PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_codec tests_codec.c)
target_link_libraries(test_codec PRIVATE stampdb)
target_include_directories(test_codec PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME codec COMMAND test_codec)
set_tests_properties(codec PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_recovery tests_recovery.c)
target_link_libraries(test_recovery PRIVATE stampdb)
add_test(NAME recovery COMMAND test_recovery)
set_tests_properties(recovery PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_powercut tests_powercut_matrix.c)
target_link_libraries(test_powercut PRIVATE stampdb)
add_test(NAME powercut_matrix COMMAND test_powercut)
set_tests_properties(powercut_matrix PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 60)

add_executable(test_crc_isolation tests_crc_isolation.c)
target_link_libraries(test_crc_isolation PRIVATE stampdb)
add_test(NAME crc_isolation COMMAND test_crc_isolation)
set_tests_properties(crc_isolation PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_exporter tests_exporter.c)
target_link_libraries(test_exporter PRIVATE stampdb)
add_test(NAME exporter COMMAND test_exporter)
set_tests_properties(exporter PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_recovery_time tests_recovery_time.c)
target_link_libraries(test_recovery_time PRIVATE stampdb)
add_test(NAME recovery_time COMMAND test_recovery_time)
set_tests_properties(recovery_time PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 60)

add_executable(test_gc_latency tests_gc_latency.c)
target_link_libraries(test_gc_latency PRIVATE stampdb)
add_test(NAME gc_latency COMMAND test_gc_latency)
set_tests_properties(gc_latency PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 90)
