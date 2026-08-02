include(FetchContent)

# 1. Fetch GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glfw)

# 2. Fetch Dear ImGui
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.8.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

# 3. Find System OpenGL
find_package(OpenGL REQUIRED)

# 4. Compile Dear ImGui + Backends
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# 5. Compile Vendored tinyfiledialogs
add_library(tinyfiledialogs STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tinyfiledialogs.c
)
target_include_directories(tinyfiledialogs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
)
if(WIN32)
    target_link_libraries(tinyfiledialogs PUBLIC comdlg32 ole32)
endif()

# 6. Build peersync GUI Executable
add_executable(peersync_gui main.cpp theme.cpp)
set_target_properties(peersync_gui PROPERTIES OUTPUT_NAME "peersync-gui")

target_link_libraries(peersync_gui PRIVATE
    peersync_core
    imgui
    glfw
    OpenGL::GL
    tinyfiledialogs
)
if(WIN32)
    target_link_libraries(peersync_gui PRIVATE comdlg32 ole32)
endif()

install(TARGETS peersync_gui DESTINATION bin)
