cmake_minimum_required(VERSION 3.16)

# Silence CMake 3.30+ deprecation warning for FetchContent_Populate
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

project(geniebuild LANGUAGES C CXX)

# Default to Release build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable FetchContent for managing project dependencies
include(FetchContent)

# ------------------------------------------------------------------------------
# Dependency 1: GLFW (Windowing & OpenGL context management)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.5.1
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ------------------------------------------------------------------------------
# Dependency 2: OpenGL
# ------------------------------------------------------------------------------
find_package(OpenGL REQUIRED)

# ------------------------------------------------------------------------------
# Dependency 3: Dear ImGui (Immediate Mode GUI)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.93.0
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
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
endif()

# ------------------------------------------------------------------------------
# Dependency 4: nlohmann/json (Modern JSON for C++)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ------------------------------------------------------------------------------
# Dependency 5: reproc (Process execution and streaming)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    reproc
    GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
    GIT_TAG        14.2.8
)
set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPROC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(reproc)

# ------------------------------------------------------------------------------
# Dependency 6: nativefiledialog-extended (Native folder & file dialogs)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.4.1
)
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nfd)

# ------------------------------------------------------------------------------
# Application Executable
# ------------------------------------------------------------------------------
set(GENIEBUILD_SOURCES
    main.cpp
    core/project.cpp
    core/source_rules.cpp
    core/target.cpp
    core/settings_resolver.cpp
    core/compiler.cpp
    core/build_runner.cpp
    ui/theme.cpp
    ui/top_bar.cpp
    ui/file_tree_panel.cpp
    ui/build_list_panel.cpp
    ui/output_panel.cpp
    ui/build_settings_modal.cpp
)

set(GENIEBUILD_HEADERS
    core/project.h
    core/source_rules.h
    core/target.h
    core/settings_resolver.h
    core/compiler.h
    core/build_runner.h
    ui/theme.h
    ui/top_bar.h
    ui/file_tree_panel.h
    ui/build_list_panel.h
    ui/output_panel.h
    ui/tree_view.h
    ui/build_settings_modal.h
)

add_executable(geniebuild ${GENIEBUILD_SOURCES} ${GENIEBUILD_HEADERS})

target_include_directories(geniebuild PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_link_libraries(geniebuild PRIVATE
    imgui
    glfw
    OpenGL::GL
    nlohmann_json::nlohmann_json
    reproc++
    nfd
)

if(WIN32)
    target_link_libraries(geniebuild PRIVATE dwmapi ole32 uuid)
endif()

# On MinGW, statically link the C++ runtime and pthreads so the binary is portable
if(MINGW)
    target_link_options(geniebuild PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Deploy assets directory adjacent to the binary so fonts and resources are always found
add_custom_command(TARGET geniebuild POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:geniebuild>/assets
)


