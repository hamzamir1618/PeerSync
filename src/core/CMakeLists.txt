include(FetchContent)
FetchContent_Declare(
    xxhash
    URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(xxhash)

add_library(peersync_core core.cpp socket.cpp socket.h message_framing.cpp message_framing.h protocol.cpp protocol.h discovery.cpp discovery.h pairing.cpp pairing.h delta.cpp delta.h transfer.cpp transfer.h sync_orchestrator.cpp ../../include/peersync/sync_orchestrator.h ${xxhash_SOURCE_DIR}/xxhash.c)
add_library(peersync::core ALIAS peersync_core)

target_include_directories(peersync_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
    ${xxhash_SOURCE_DIR}
)

if(WIN32)
    target_link_libraries(peersync_core PUBLIC ws2_32)
endif()

# Set the library output name to peersync_core
set_target_properties(peersync_core PROPERTIES OUTPUT_NAME "peersync_core")

