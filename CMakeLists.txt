cmake_minimum_required(VERSION 3.16)
project(cpp_http_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# -g keeps symbols in Release so perf and gdb output stay readable. Applied only
# while the flags are still CMake's default, so a -DCMAKE_CXX_FLAGS_RELEASE=...
# given on the command line is kept.
if(CMAKE_CXX_FLAGS_RELEASE STREQUAL "-O3 -DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g" CACHE STRING "Flags used by the CXX compiler during Release builds." FORCE)
endif()

set(HS_SANITIZE "" CACHE STRING "Sanitizer to enable: empty, asan or tsan")
set_property(CACHE HS_SANITIZE PROPERTY STRINGS "" asan tsan)

# Flags are collected in one interface target so every target gets the same ones.
add_library(hs_flags INTERFACE)
target_compile_options(hs_flags INTERFACE -Wall -Wextra -Wpedantic -Werror)
if(HS_SANITIZE STREQUAL "asan")
  # UBSan normally prints its report and carries on, so a unit test hitting
  # undefined behaviour would still exit 0; -fno-sanitize-recover makes it fail.
  target_compile_options(hs_flags INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=undefined
                         -fno-omit-frame-pointer)
  target_link_options(hs_flags INTERFACE -fsanitize=address,undefined)
elseif(HS_SANITIZE STREQUAL "tsan")
  target_compile_options(hs_flags INTERFACE -fsanitize=thread)
  target_link_options(hs_flags INTERFACE -fsanitize=thread)
elseif(NOT HS_SANITIZE STREQUAL "")
  message(FATAL_ERROR "HS_SANITIZE must be empty, asan or tsan (got '${HS_SANITIZE}')")
endif()

find_package(Threads REQUIRED)

# Everything under src/ except main.cpp is the library, so new files are picked
# up on the next build without editing this file.
file(GLOB_RECURSE HS_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
list(FILTER HS_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")

add_library(hs_core STATIC ${HS_SOURCES})
target_include_directories(hs_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(hs_core PUBLIC hs_flags Threads::Threads)

add_executable(httpserver src/main.cpp)
target_link_libraries(httpserver PRIVATE hs_core)

enable_testing()

file(GLOB HS_UNIT_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/unit/*_test.cpp)
add_executable(unit_tests tests/unit/main.cpp ${HS_UNIT_TESTS})
target_include_directories(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit ${PROJECT_SOURCE_DIR}/bench)
target_link_libraries(unit_tests PRIVATE hs_core)
add_test(NAME unit COMMAND unit_tests)

# The shared integration suite drives the real binary over loopback sockets.
add_test(NAME integration_pool
         COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/run_all.py
                 --binary $<TARGET_FILE:httpserver> --mode pool)
set_tests_properties(integration_pool PROPERTIES TIMEOUT 300)
add_test(NAME integration_epoll
         COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/run_all.py
                 --binary $<TARGET_FILE:httpserver> --mode epoll)
set_tests_properties(integration_epoll PROPERTIES TIMEOUT 300)

# The load generator is an independent client: it deliberately does not link
# hs_core, so no server code can influence what it measures.
add_executable(loadgen bench/loadgen.cpp)
target_link_libraries(loadgen PRIVATE hs_flags Threads::Threads)

# Needs the loadgen binary, so it selects only the loadgen tests; the two
# suites above are run without --loadgen and skip them.
add_test(NAME loadgen_smoke
         COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/run_all.py
                 --binary $<TARGET_FILE:httpserver> --mode epoll
                 --loadgen $<TARGET_FILE:loadgen> -k loadgen)
set_tests_properties(loadgen_smoke PROPERTIES TIMEOUT 120)
