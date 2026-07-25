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

add_executable(peersync_tests test_placeholder.cpp test_socket.cpp)
target_link_libraries(peersync_tests PRIVATE peersync_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(peersync_tests)
