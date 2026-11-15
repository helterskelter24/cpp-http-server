cmake_minimum_required(VERSION 3.16)
project(cpp_http_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# -g keeps symbols in Release so perf and gdb output stay readable.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

set(HS_SANITIZE "" CACHE STRING "Sanitizer to enable: empty, asan or tsan")
set_property(CACHE HS_SANITIZE PROPERTY STRINGS "" asan tsan)

# Flags are collected in one interface target so every target gets the same ones.
add_library(hs_flags INTERFACE)
target_compile_options(hs_flags INTERFACE -Wall -Wextra -Wpedantic -Werror)
if(HS_SANITIZE STREQUAL "asan")
  target_compile_options(hs_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
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
file(GLOB_RECURSE HS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER HS_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")

add_library(hs_core STATIC ${HS_SOURCES})
target_include_directories(hs_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hs_core PUBLIC hs_flags Threads::Threads)

add_executable(httpserver src/main.cpp)
target_link_libraries(httpserver PRIVATE hs_core)

enable_testing()

file(GLOB HS_UNIT_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/*_test.cpp)
add_executable(unit_tests tests/unit/main.cpp ${HS_UNIT_TESTS})
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
target_link_libraries(unit_tests PRIVATE hs_core)
add_test(NAME unit COMMAND unit_tests)

# The shared integration suite drives the real binary over loopback sockets.
add_test(NAME integration_pool
         COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/integration/run_all.py
                 --binary $<TARGET_FILE:httpserver> --mode pool)
set_tests_properties(integration_pool PROPERTIES TIMEOUT 300)
