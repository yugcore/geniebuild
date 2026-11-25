#pragma once

#include "project.h"

namespace ui {

// Renders the left pane: File tree with visual marks for files in the build list
void RenderFileTreePanel(core::Project& project, bool embedded = false);

// Explicitly forces a rescan of the project directory tree
void RefreshFileTree();

// Returns true if a background folder indexing job is running
bool IsFileTreeIndexing();

// Safely terminates background indexing thread on shutdown (§2.3 & §2.8)
void ShutdownFileTree();

} // namespace ui
