add_executable(peersync_cli main.cpp)
set_target_properties(peersync_cli PROPERTIES OUTPUT_NAME "peersync")
target_link_libraries(peersync_cli PRIVATE peersync_core)
