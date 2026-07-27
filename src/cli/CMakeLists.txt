include(FetchContent)
FetchContent_Declare(
    cli11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.6.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(cli11)

add_executable(peersync_cli main.cpp)
set_target_properties(peersync_cli PROPERTIES OUTPUT_NAME "peersync")
target_link_libraries(peersync_cli PRIVATE peersync_core CLI11::CLI11)

install(TARGETS peersync_cli DESTINATION bin)
