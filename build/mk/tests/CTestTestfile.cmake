# CMake generated Testfile for 
# Source directory: /Users/tahur/Desktop/stampDB/tests
# Build directory: /Users/tahur/Desktop/stampDB/build/mk/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[basic]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_basic")
set_tests_properties([=[basic]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;7;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[codec]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_codec")
set_tests_properties([=[codec]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;13;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[recovery]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_recovery")
set_tests_properties([=[recovery]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;18;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[powercut_matrix]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_powercut")
set_tests_properties([=[powercut_matrix]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;23;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[crc_isolation]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_crc_isolation")
set_tests_properties([=[crc_isolation]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;28;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[exporter]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_exporter")
set_tests_properties([=[exporter]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;33;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[recovery_time]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_recovery_time")
set_tests_properties([=[recovery_time]=] PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;38;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
add_test([=[gc_latency]=] "/Users/tahur/Desktop/stampDB/build/mk/tests/test_gc_latency")
set_tests_properties([=[gc_latency]=] PROPERTIES  TIMEOUT "90" WORKING_DIRECTORY "/Users/tahur/Desktop/stampDB/build/mk" _BACKTRACE_TRIPLES "/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;43;add_test;/Users/tahur/Desktop/stampDB/tests/CMakeLists.txt;0;")
