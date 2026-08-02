cmake_minimum_required(VERSION 3.15)
project(peersync VERSION 0.1.0 LANGUAGES C CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(PEERSYNC_BUILD_TESTS "Build peersync unit tests" ON)
option(PEERSYNC_BUILD_GUI "Build peersync GUI frontend" ON)
option(PEERSYNC_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

if(PEERSYNC_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PEERSYNC_BUILD_FUZZERS requires Clang compiler (found ${CMAKE_CXX_COMPILER_ID})")
    endif()
    message(STATUS "Enabling libFuzzer instrumentation")
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(PEERSYNC_ENABLE_SANITIZERS AND NOT WIN32)
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

# Fuzzers (conditionally added)
if(PEERSYNC_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# CPack packaging configuration
include(GNUInstallDirs)

set(CPACK_PACKAGE_NAME "peersync")
set(CPACK_PACKAGE_VENDOR "peersync")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local network offline file & directory synchronization tool")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if(WIN32 OR APPLE)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
