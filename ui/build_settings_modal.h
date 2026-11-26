#pragma once

#include "core/project.h"
#include "core/compiler.h"

namespace ui {

// Opens the Build Settings modal dialog (Spec §5.1.1), optionally pre-selecting a target
void OpenBuildSettingsModal(const std::string& target_name = "");

// Opens Build Settings scoped to a folder or file override (V2-3)
void OpenBuildSettingsForOverride(
    const std::string& target_name,
    const std::string& folder_rel,
    const std::string& file_rel
);

// Opens Build Settings directly on a target's Steps tab (V2-6)
void OpenBuildSettingsForSteps(
    const std::string& target_name,
    int step_index = -1
);

// Opens Build Settings directly on a named target's PCH (tab 7) or Unity (tab 8) tab (V2-8 / V2-9)
void OpenBuildSettingsForPch(const std::string& target_name);
void OpenBuildSettingsForUnity(const std::string& target_name);

// Opens the Manage Configurations dialog (V2-4)
void OpenManageConfigsModal();

// Opens the Toolchain Manager dialog (V2-14 & V2-17)
void OpenToolchainManagerModal();

// Checks if Build Settings modal is currently open
bool IsBuildSettingsModalOpen();

// Renders the Build Settings modal dialog, Manage Configurations, and Toolchain Manager modals
void RenderBuildSettingsModal(core::Project& project, core::CompilerManager& compiler_manager);

} // namespace ui
