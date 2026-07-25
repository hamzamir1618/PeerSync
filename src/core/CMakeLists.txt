add_library(peersync_core core.cpp)
add_library(peersync::core ALIAS peersync_core)

target_include_directories(peersync_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    $<INSTALL_INTERFACE:include>
)

# Set the library output name to peersync_core
set_target_properties(peersync_core PROPERTIES OUTPUT_NAME "peersync_core")
