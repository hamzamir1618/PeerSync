cmake_minimum_required(VERSION 3.15)
project(peersync VERSION 0.1.0 LANGUAGES C CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(PEERSYNC_BUILD_TESTS "Build peersync unit tests" ON)
option(PEERSYNC_BUILD_GUI "Build peersync GUI frontend" ON)
option(PEERSYNC_ENABLE_SANITIZERS "Enable Address and Undefined Behavior sanitizers" OFF)

if(PEERSYNC_ENABLE_SANITIZERS AND NOT WIN32)
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()


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
