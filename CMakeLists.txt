cmake_minimum_required(VERSION 3.15)
project(peersync VERSION 0.1.0 LANGUAGES CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(PEERSYNC_BUILD_TESTS "Build peersync unit tests" ON)
option(PEERSYNC_BUILD_GUI "Build peersync GUI frontend" ON)

# Core library
add_subdirectory(src/core)

# CLI frontend (always added)
add_subdirectory(src/cli)

# GUI frontend (conditionally added)
if(PEERSYNC_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

# Tests (conditionally added)
if(PEERSYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
