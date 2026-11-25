#pragma once

#include "compiler.h"
#include "build_runner.h"
#include <vector>
#include <string>

namespace ui {

// Renders the right pane: Output terminal
// Displays live compilation output stream, guard diagnostics, exit code, and status.
void RenderOutputPanel(
    const core::CompilerManager& compiler_manager,
    const core::BuildRunner& build_runner,
    std::vector<core::OutLine>& output_lines,
    std::string& build_start_error
);

} // namespace ui
