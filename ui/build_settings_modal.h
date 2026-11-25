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

// Opens the Manage Configurations dialog (V2-4)
void OpenManageConfigsModal();

// Checks if Build Settings modal is currently open
bool IsBuildSettingsModalOpen();

// Renders the Build Settings modal dialog and Manage Configurations modal
void RenderBuildSettingsModal(core::Project& project, const core::CompilerManager& compiler_manager);

} // namespace ui
