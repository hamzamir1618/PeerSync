add_library(peersync_core core.cpp socket.cpp socket.h message_framing.cpp message_framing.h protocol.cpp protocol.h)
add_library(peersync::core ALIAS peersync_core)

target_include_directories(peersync_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

if(WIN32)
    target_link_libraries(peersync_core PUBLIC ws2_32)
endif()

# Set the library output name to peersync_core
set_target_properties(peersync_core PROPERTIES OUTPUT_NAME "peersync_core")
