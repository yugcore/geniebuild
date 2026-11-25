#pragma once

#include "project.h"
#include "compiler.h"
#include "build_runner.h"
#include <vector>
#include <string>

namespace ui {

// Opens the New Target Wizard modal (Ctrl+Shift+T)
void OpenNewTargetWizard();

// Renders the middle pane: Build List (v1) or Targets & Source Rules (v2, progressive disclosure)
void RenderBuildListPanel(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    bool embedded = false
);

} // namespace ui
