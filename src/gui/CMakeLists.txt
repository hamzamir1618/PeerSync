add_executable(peersync_gui main.cpp)
set_target_properties(peersync_gui PROPERTIES OUTPUT_NAME "peersync-gui")
target_link_libraries(peersync_gui PRIVATE peersync_core)
