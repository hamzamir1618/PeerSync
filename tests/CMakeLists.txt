include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(CTest)

add_executable(peersync_tests test_placeholder.cpp test_socket.cpp test_message_framing.cpp test_protocol.cpp test_discovery.cpp test_pairing.cpp test_pairing_integration.cpp test_delta.cpp test_transfer.cpp test_sync_orchestrator.cpp test_cli_integration.cpp test_gui_logic.cpp)
target_link_libraries(peersync_tests PRIVATE peersync_core GTest::gtest_main)
target_compile_definitions(peersync_tests PRIVATE PEERSYNC_CLI_PATH="$<TARGET_FILE:peersync_cli>")

include(GoogleTest)
gtest_discover_tests(peersync_tests
    TEST_FILTER "CliIntegrationTest.DiscoverFindsListeningAdvertiser"
    PROPERTIES LABELS "requires_multicast"
)
gtest_discover_tests(peersync_tests
    TEST_FILTER "-CliIntegrationTest.DiscoverFindsListeningAdvertiser"
    PROPERTIES LABELS "unit"
)

add_executable(peersync_perf_tests perf_delta_benchmark.cpp)
target_link_libraries(peersync_perf_tests PRIVATE peersync_core GTest::gtest_main)

gtest_discover_tests(peersync_perf_tests
    PROPERTIES LABELS "performance"
)

add_executable(peersync_stress_tests perf_resumability_stress.cpp)
target_link_libraries(peersync_stress_tests PRIVATE peersync_core GTest::gtest_main)

gtest_discover_tests(peersync_stress_tests
    PROPERTIES LABELS "performance;stress"
)

