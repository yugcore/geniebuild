#pragma once

#include "project.h"
#include "compiler.h"
#include "build_runner.h"
#include <vector>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ui {

struct WindowChromeHitTest {
    float header_height = 32.0f;
    RECT max_button_rect = { 0, 0, 0, 0 };
    std::vector<RECT> interactive_rects;
};

extern WindowChromeHitTest g_ChromeHitTest;

} // namespace ui
#endif

struct GLFWwindow;

namespace ui {

// Renders the dedicated top window title bar (32px): App title, project tag, and window controls (— □ ✕)
void RenderTitleBar(
    GLFWwindow* window,
    const core::Project& project
);

// Renders the dedicated action toolbar (40px): Open Folder, Settings, Output path, Debug/Release, Compiler, Build, FPS
void RenderToolBar(
    GLFWwindow* window,
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines
);

// Renders the integrated custom title bar (backward compatibility wrapper)
void RenderTopBar(
    GLFWwindow* window,
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines
);

// Triggers build or opens large-build confirmation modal if files > 500 (Spec §3.7)
void TriggerBuildWithGuard(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    const std::string& target_name = ""
);

// Triggers rebuild (clean + build) with guards (Spec §5.1.5)
void TriggerRebuildWithGuard(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    const std::string& target_name = ""
);

// Prompts confirmation before closing if a build is in progress (Spec §4)
void RequestExitConfirmation();

} // namespace ui
