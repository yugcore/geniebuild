#pragma once

#include "compiler.h"
#include "build_runner.h"
#include "run_runner.h"
#include <vector>
#include <string>

namespace core {
class Project;
}

namespace ui {

// Requests switching Output pane to Build (false) or Run (true) tab (V2-16)
void RequestOutputTab(bool run_tab);

// Renders the right pane: Output terminal (with progressive-disclosure Build | Run tabs,
// V2-20 progress/ETA/jobs/summary, and V2-21 disk-backed 2M-line log, filters, search, F8)
void RenderOutputPanel(
    const core::CompilerManager& compiler_manager,
    const core::BuildRunner& build_runner,
    std::vector<core::OutLine>& output_lines,
    std::string& build_start_error,
    core::RunRunner* run_runner = nullptr,
    std::vector<core::OutLine>* run_lines = nullptr,
    std::string* run_start_error = nullptr,
    const core::Project* project = nullptr
);

} // namespace ui
