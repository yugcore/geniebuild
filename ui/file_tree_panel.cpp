#include "file_tree_panel.h"
#include "top_bar.h"
#include "build_settings_modal.h"
#include "source_rules.h"
#include "theme.h"
#include "icons.h"
#include "tree_view.h"
#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace ui {

enum class FileNodeType {
    Directory,
    SourceFile,
    HeaderFile
};

struct FileNode {
    std::string name;             // cached display name (filename)
    std::string name_lower;       // precomputed lowercase name for zero-alloc filtering (§2.3)
    std::string rel_path;         // cached normalized relative path
    std::string full_path_str;    // cached full path string for ImGui ID
    std::filesystem::path full_path;
    FileNodeType type = FileNodeType::Directory;
    bool is_c_source = false;     // true for .c, false for .cpp/.cc/.cxx
    bool children_loaded = false; // loaded lazily on expansion or via background indexing
    std::vector<FileNode> children;
};

static std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

static std::string NormalizePath(const std::string& path_str) {
    std::string result = path_str;
    std::replace(result.begin(), result.end(), '\\', '/');
    if (result.rfind("./", 0) == 0) {
        result = result.substr(2);
    }
    return result;
}

static bool ClassifyFile(const std::filesystem::path& path, FileNodeType& out_type, bool& out_is_c) {
    const std::string ext = ToLower(path.extension().generic_u8string());

    if (ext == ".c") {
        out_type = FileNodeType::SourceFile;
        out_is_c = true;
        return true;
    }
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
        out_type = FileNodeType::SourceFile;
        out_is_c = false;
        return true;
    }
    if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx") {
        out_type = FileNodeType::HeaderFile;
        out_is_c = false;
        return true;
    }
    return false;
}

// Reads directory only when expanded and caches its children
static void LoadDirectoryChildren(FileNode& dir_node, const std::filesystem::path& root_path) {
    if (dir_node.children_loaded) {
        return;
    }
    dir_node.children_loaded = true;
    dir_node.children.clear();

    std::error_code ec;
    std::filesystem::directory_iterator it(dir_node.full_path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return;
    }

    for (const auto& entry : it) {
        std::error_code entry_ec;
        const auto status = entry.status(entry_ec);
        if (entry_ec) {
            continue;
        }

        const auto& path = entry.path();
        const std::string fname = path.filename().generic_u8string();

        if (std::filesystem::is_directory(status)) {
            if (core::Project::ShouldSkipDirectory(fname)) {
                continue;
            }

            FileNode sub;
            sub.name = fname;
            sub.name_lower = ToLower(fname);
            sub.full_path = path;
            sub.full_path_str = path.generic_u8string();
            std::filesystem::path rel = std::filesystem::relative(path, root_path, entry_ec);
            sub.rel_path = NormalizePath(rel.generic_u8string());
            sub.type = FileNodeType::Directory;
            sub.is_c_source = false;
            sub.children_loaded = false; // lazy load when expanded
            dir_node.children.push_back(std::move(sub));
        } else if (std::filesystem::is_regular_file(status)) {
            FileNodeType ftype;
            bool is_c = false;
            if (ClassifyFile(path, ftype, is_c)) {
                FileNode file_node;
                file_node.name = fname;
                file_node.name_lower = ToLower(fname);
                file_node.full_path = path;
                file_node.full_path_str = path.generic_u8string();
                std::filesystem::path rel = std::filesystem::relative(path, root_path, entry_ec);
                file_node.rel_path = NormalizePath(rel.generic_u8string());
                file_node.type = ftype;
                file_node.is_c_source = is_c;
                file_node.children_loaded = true;
                dir_node.children.push_back(std::move(file_node));
            }
        }
    }

    // Sort: directories first, then alphabetical by name
    std::sort(dir_node.children.begin(), dir_node.children.end(), [](const FileNode& a, const FileNode& b) {
        if (a.type == FileNodeType::Directory && b.type != FileNodeType::Directory) {
            return true;
        }
        if (a.type != FileNodeType::Directory && b.type == FileNodeType::Directory) {
            return false;
        }
        return a.name < b.name;
    });
}

// Recursively builds a complete directory tree for background indexing (Spec §2.3)
static void BuildCompleteTreeRecursive(FileNode& dir_node, const std::filesystem::path& root_path, const std::atomic<bool>& abort_flag) {
    if (abort_flag.load()) return;
    LoadDirectoryChildren(dir_node, root_path);

    for (auto& child : dir_node.children) {
        if (abort_flag.load()) return;
        if (child.type == FileNodeType::Directory) {
            BuildCompleteTreeRecursive(child, root_path, abort_flag);
        }
    }
}

// Recursively collects all source files within a directory for right-click batch addition
static void CollectSourcesInFolder(const std::filesystem::path& folder_path, const std::filesystem::path& root_path, std::vector<std::string>& out_sources) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(folder_path, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;

    auto is_source_file = [](const std::filesystem::path& p) {
        std::string ext = ToLower(p.extension().generic_u8string());
        return (ext == ".cpp" || ext == ".c" || ext == ".cc" || ext == ".cxx");
    };

    while (it != end && !ec) {
        std::error_code entry_ec;
        auto status = it->status(entry_ec);
        if (std::filesystem::is_directory(status)) {
            if (core::Project::ShouldSkipDirectory(it->path().filename().generic_u8string())) {
                it.disable_recursion_pending();
            }
        } else if (std::filesystem::is_regular_file(status)) {
            if (is_source_file(it->path())) {
                std::filesystem::path rel = std::filesystem::relative(it->path(), root_path, entry_ec);
                if (!entry_ec) {
                    out_sources.push_back(NormalizePath(rel.generic_u8string()));
                }
            }
        }
        it.increment(ec);
    }
}

// Batch removes all queued files originating inside a specified directory
static void RemoveSourcesInFolder(const std::string& folder_rel_path, core::Project& project) {
    std::string prefix = NormalizePath(folder_rel_path);
    if (!prefix.empty() && prefix.back() != '/') {
        prefix += "/";
    }

    const auto& files = project.GetFiles();
    std::vector<std::string> to_remove;
    for (const auto& file : files) {
        if (prefix.empty() || file.rfind(prefix, 0) == 0) {
            to_remove.push_back(file);
        }
    }
    if (!to_remove.empty()) {
        project.RemoveFiles(to_remove);
    }
}

// Zero-allocation filter match using precomputed lowercase names (Spec §2.3)
static bool NodeMatchesFilter(const FileNode& node, const std::string& filter_lower) {
    if (filter_lower.empty()) return true;

    if (node.name_lower.find(filter_lower) != std::string::npos) {
        return true;
    }
    if (node.type == FileNodeType::Directory) {
        for (const auto& child : node.children) {
            if (NodeMatchesFilter(child, filter_lower)) {
                return true;
            }
        }
    }
    return false;
}

// Determines if any node in subtree contains a source or header file
static bool NodeContainsSourceOrHeader(const FileNode& node) {
    if (node.type == FileNodeType::SourceFile || node.type == FileNodeType::HeaderFile) {
        return true;
    }
    for (const auto& child : node.children) {
        if (NodeContainsSourceOrHeader(child)) {
            return true;
        }
    }
    return false;
}

static FileNode s_ProjectTree;
static std::filesystem::path s_CurrentRoot;
static bool s_NeedsRescan = true;
static std::unordered_set<std::string> s_ExpandedFolders;
static std::string s_SelectedPath;
static int s_CachedSelectedIndex = -1;
static bool s_FocusFilterRequested = false;
static int s_ScrollToIndex = -1;
static bool s_ShowExplorerRemoveAllModal = false;

// Performance §2.2 & §2.3: Cached flat rows and background indexing state
static std::vector<TreeRow> s_CachedFlatRows;
static bool s_RowsDirty = true;
static uint64_t s_LastProjectRevision = (uint64_t)-1;
static uint64_t s_ExpandRevision = 0;
static uint64_t s_LastRenderedExpandRev = (uint64_t)-1;

static std::string s_CurrentFilterText = "";
static std::string s_DebouncedFilterText = "";
static std::chrono::steady_clock::time_point s_LastFilterEditTime;
static bool s_FilterDebounceActive = false;

// Background indexing state for filter search without freezing the UI (§2.3)
static std::thread s_IndexThread;
static std::atomic<bool> s_IsIndexing{false};
static std::atomic<bool> s_AbortIndexing{false};
static bool s_TreeFullyIndexed = false;
static std::mutex s_IndexMutex;
static std::unique_ptr<FileNode> s_PendingIndexedTree;
static std::atomic<bool> s_HasPendingIndexedTree{false};

void RefreshFileTree() {
    s_NeedsRescan = true;
    s_TreeFullyIndexed = false;
    s_RowsDirty = true;
}

void ShutdownFileTree() {
    s_AbortIndexing.store(true);
    if (s_IndexThread.joinable()) {
        s_IndexThread.join();
    }
}

bool IsFileTreeIndexing() {
    return s_IsIndexing.load();
}

static void StartBackgroundIndexing(const std::filesystem::path& root_path) {
    if (s_IsIndexing.load() || s_TreeFullyIndexed) {
        return;
    }

    s_AbortIndexing.store(true);
    if (s_IndexThread.joinable()) {
        s_IndexThread.join();
    }
    s_AbortIndexing.store(false);
    s_IsIndexing.store(true);

    s_IndexThread = std::thread([root_path]() {
        auto indexed_root = std::make_unique<FileNode>();
        indexed_root->name = root_path.filename().generic_u8string();
        if (indexed_root->name.empty()) {
            indexed_root->name = root_path.generic_u8string();
        }
        indexed_root->name_lower = ToLower(indexed_root->name);
        indexed_root->full_path = root_path;
        indexed_root->full_path_str = root_path.generic_u8string();
        indexed_root->rel_path = "";
        indexed_root->type = FileNodeType::Directory;
        indexed_root->children_loaded = false;

        BuildCompleteTreeRecursive(*indexed_root, root_path, s_AbortIndexing);

        if (!s_AbortIndexing.load()) {
            {
                std::lock_guard<std::mutex> lock(s_IndexMutex);
                s_PendingIndexedTree = std::move(indexed_root);
            }
            s_HasPendingIndexedTree.store(true);
        }
        s_IsIndexing.store(false);
    });
}

// Recursively flattens visible directory tree nodes into flat rows with guide line bitmasks
static bool OverrideMatchesFolder(const std::string& match_pat, const std::string& folder_rel) {
    if (folder_rel.empty()) return false;
    return match_pat == folder_rel ||
           match_pat == (folder_rel + "/**") ||
           match_pat == (folder_rel + "/*");
}

static std::string SummarizeOverride(const core::FlagOverride& ov, const std::string& target_name, const std::string& active_cfg) {
    std::string s = "[" + target_name + "] " + ov.match;
    if (!ov.condition.empty()) s += " (if " + ov.condition + ")";
    std::vector<std::string> parts;
    auto add_opt = [&](const std::string& opt, const std::string& dbg, bool p, bool u,
                       const std::vector<std::string>& defs,
                       const std::vector<std::string>& cflags,
                       const std::vector<std::string>& incdirs) {
        if (!opt.empty()) parts.push_back("opt=" + opt);
        if (!dbg.empty()) parts.push_back("debug=" + dbg);
        if (p) parts.push_back("no-pch");
        if (u) parts.push_back("no-unity");
        if (!defs.empty()) parts.push_back("+" + std::to_string(defs.size()) + " defines");
        if (!cflags.empty()) parts.push_back("+" + std::to_string(cflags.size()) + " flags");
        if (!incdirs.empty()) parts.push_back("+" + std::to_string(incdirs.size()) + " includes");
    };
    add_opt(ov.optimize, ov.debugInfo, ov.excludeFromPch, ov.excludeFromUnity,
            ov.settings.defines, ov.settings.compileFlags, ov.settings.includeDirs);
    auto cit = ov.configs.find(active_cfg);
    if (cit != ov.configs.end()) {
        add_opt(cit->second.optimize, cit->second.debugInfo, false, false,
                cit->second.defines, cit->second.compileFlags, {});
    }
    if (!parts.empty()) {
        s += ": ";
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) s += ", ";
            s += parts[i];
        }
    }
    return s;
}

static void PopulateNodeOverrideMarker(const core::Project& project, TreeRow& row, bool is_folder) {
    if (row.rel_path.empty()) return;
    const std::string& active_cfg = project.GetConfiguration();
    std::vector<std::string> lines;
    for (const auto& t : project.GetTargets()) {
        if (t.overrides.empty()) continue;
        if (!is_folder) {
            bool owned = std::find(t.expandedFiles.begin(), t.expandedFiles.end(), row.rel_path) != t.expandedFiles.end();
            if (!owned) continue;
        }
        for (const auto& ov : t.overrides) {
            bool matched = false;
            if (is_folder) {
                matched = OverrideMatchesFolder(ov.match, row.rel_path);
            } else {
                auto glob = core::CompiledGlob::Compile(ov.match);
                matched = glob.Matches(row.rel_path);
            }
            if (matched) {
                lines.push_back(SummarizeOverride(ov, t.name, active_cfg));
            }
        }
    }
    if (!lines.empty()) {
        row.has_override = true;
        row.override_tooltip = "Flag overrides:";
        for (const auto& l : lines) {
            row.override_tooltip += "\n  " + l;
        }
    }
}

static void FlattenNode(
    FileNode& node,
    int depth,
    uint64_t ancestor_has_next,
    bool is_last_child,
    const core::Project& project,
    const std::string& filter_lower,
    std::vector<TreeRow>& out_rows
) {
    if (!filter_lower.empty() && !NodeMatchesFilter(node, filter_lower)) {
        return;
    }

    TreeRow row;
    row.id_str = node.full_path_str;
    row.name = node.name;
    row.rel_path = node.rel_path;
    row.depth = depth;
    row.ancestor_has_next = ancestor_has_next;
    row.is_last_child = is_last_child;
    row.user_data = &node;

    if (node.type == FileNodeType::Directory) {
        row.type = TreeRowType::Folder;
        // Default open if filter is active, otherwise check expansion set
        bool is_open = (!filter_lower.empty()) || (s_ExpandedFolders.find(node.full_path_str) != s_ExpandedFolders.end());
        row.is_open = is_open;
        PopulateNodeOverrideMarker(project, row, true);
        out_rows.push_back(row);

        if (is_open) {
            // Lazy load single directory on user demand if not yet indexed
            if (!node.children_loaded) {
                LoadDirectoryChildren(node, project.GetRootPath());
            }

            std::vector<size_t> visible_child_indices;
            for (size_t i = 0; i < node.children.size(); ++i) {
                if (filter_lower.empty() || NodeMatchesFilter(node.children[i], filter_lower)) {
                    visible_child_indices.push_back(i);
                }
            }

            const size_t count = visible_child_indices.size();
            for (size_t i = 0; i < count; ++i) {
                const bool child_is_last = (i == count - 1);
                uint64_t next_ancestor_mask = ancestor_has_next;
                if (!is_last_child && depth > 0) {
                    next_ancestor_mask |= (1ULL << (depth - 1));
                } else if (depth > 0) {
                    next_ancestor_mask &= ~(1ULL << (depth - 1));
                }

                FlattenNode(
                    node.children[visible_child_indices[i]],
                    depth + 1,
                    next_ancestor_mask,
                    child_is_last,
                    project,
                    filter_lower,
                    out_rows
                );
            }
        }
    } else {
        if (node.type == FileNodeType::SourceFile) {
            row.type = TreeRowType::SourceFile;
            row.is_c = node.is_c_source;
            const auto& files_set = project.GetFilesSet();
            row.in_build = (files_set.find(node.rel_path) != files_set.end());
            if (row.in_build) {
                PopulateNodeOverrideMarker(project, row, false);
                if (project.IsMultiTargetOrRulesUI()) {
                    for (const auto& tname : project.GetTargetsOwningFile(node.rel_path)) {
                        row.target_dots.push_back(Theme::GetTargetColor(tname));
                    }
                }
            }
        } else {
            row.type = TreeRowType::HeaderFile;
        }
        out_rows.push_back(row);
    }
}

void RenderFileTreePanel(core::Project& project, bool embedded) {
    static char s_TreeFilter[128] = "";
    const std::filesystem::path& project_root = project.GetRootPath();

    // Check if background indexing completed and swap in snapshot on UI thread (§2.3 & §2.8)
    if (s_HasPendingIndexedTree.load()) {
        std::unique_ptr<FileNode> pending;
        {
            std::lock_guard<std::mutex> lock(s_IndexMutex);
            pending = std::move(s_PendingIndexedTree);
            s_HasPendingIndexedTree.store(false);
        }
        if (pending) {
            s_ProjectTree = std::move(*pending);
            s_TreeFullyIndexed = true;
            s_RowsDirty = true;
        }
    }

    if (s_NeedsRescan || project_root != s_CurrentRoot) {
        s_AbortIndexing.store(true);
        if (s_IndexThread.joinable()) {
            s_IndexThread.join();
        }

        s_CurrentRoot = project_root;
        s_ProjectTree = FileNode{};
        if (!project_root.empty()) {
            s_ProjectTree.name = project_root.filename().generic_u8string();
            if (s_ProjectTree.name.empty()) {
                s_ProjectTree.name = project_root.generic_u8string();
            }
            s_ProjectTree.name_lower = ToLower(s_ProjectTree.name);
            s_ProjectTree.full_path = project_root;
            s_ProjectTree.full_path_str = project_root.generic_u8string();
            s_ProjectTree.rel_path = "";
            s_ProjectTree.type = FileNodeType::Directory;
            s_ProjectTree.children_loaded = false;

            LoadDirectoryChildren(s_ProjectTree, project_root);

            // Root is expanded by default
            s_ExpandedFolders.insert(s_ProjectTree.full_path_str);
        }
        s_NeedsRescan = false;
        s_TreeFullyIndexed = false;
        s_RowsDirty = true;
    }

    // -------------------------------------------------------------------------
    // Tier 1: Project Explorer Pane
    // -------------------------------------------------------------------------
    if (Theme::PaneBegin("FileTreeCard", Theme::Col::BgExplorer, Theme::Col::Border, ImVec2(0, 0), ImVec2(0, 0), embedded)) {
        // Status badge: SCANNING..., INDEXING..., or clean header
        std::string badge_text;
        bool is_indexing = s_IsIndexing.load();

        if (project.IsScanning()) {
            badge_text = std::string(ICON_FA_SPINNER) + " SCANNING...";
        } else if (is_indexing) {
            badge_text = std::string(ICON_FA_SPINNER) + " INDEXING...";
        }

        const float pane_w = ImGui::GetContentRegionAvail().x;
        const bool compact_header = (pane_w < 260.0f * Theme::DpiScale);
        const float refresh_btn_w = compact_header ? Theme::Btn::IconW : Theme::Btn::RefreshW;
        const float badge_w = badge_text.empty() ? 0.0f : ImGui::CalcTextSize(badge_text.c_str()).x;
        const float total_right_w = refresh_btn_w + (badge_text.empty() ? 0.0f : (Theme::Sp::S2 + badge_w));

        Theme::BeginPaneHeader("PROJECT EXPLORER", total_right_w, embedded);

        // 1. Refresh button (collapses to icon-only on narrow panes so nothing clips)
        if (compact_header) {
            if (Theme::IconButton("##RefreshTreeBtn", ICON_FA_ROTATE, "Refresh: rescan project directory and refresh file caches", false, ImVec2(refresh_btn_w, Theme::Btn::SmallH))) {
                RefreshFileTree();
                project.RefreshMissingFilesCache();
            }
        } else {
            if (Theme::Button(ICON_FA_ROTATE " Refresh", "Rescan project directory and refresh file caches", false, ImVec2(refresh_btn_w, Theme::Btn::SmallH))) {
                RefreshFileTree();
                project.RefreshMissingFilesCache();
            }
        }
        Theme::Tooltip("Folders automatically skipped during scan:\n"
                       "  - .git, .geniebuild, and all dot-folders\n"
                       "  - build, CMakeFiles, cmake-build-*, _deps\n"
                       "  - out, bin, obj, x64, Debug, Release\n"
                       "  - thirdparty, 3rdparty, third_party, external, deps, vendor\n"
                       "  - node_modules, tests");

        if (!badge_text.empty()) {
            ImGui::SameLine(0.0f, Theme::Sp::S2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Theme::Col::Warning, "%s", badge_text.c_str());
        }

        Theme::EndPaneHeader();

        // Filter text box with 150ms debouncing (Spec §2.3) and clear (x) button / Esc (Spec §3.2)
        const float content_w = ImGui::GetContentRegionAvail().x - Theme::Sp::S3;
        bool has_filter = (s_TreeFilter[0] != '\0');
        const float clear_btn_size = ImGui::GetFrameHeight();
        const float filter_w = has_filter ? (content_w - clear_btn_size - Theme::Sp::S1) : content_w;

        ImGui::SetNextItemWidth(filter_w);
        if (s_FocusFilterRequested) {
            ImGui::SetKeyboardFocusHere();
            s_FocusFilterRequested = false;
        }
        if (ImGui::InputTextWithHint("##TreeFilter", ICON_FA_SEARCH " Filter files...", s_TreeFilter, sizeof(s_TreeFilter))) {
            s_FilterDebounceActive = true;
            s_LastFilterEditTime = std::chrono::steady_clock::now();
        }
        Theme::Tooltip("Filter files and folders by name (Ctrl+F to focus, Esc to clear)");

        if (has_filter) {
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (Theme::IconButton("##ClearTreeFilter", ICON_FA_XMARK, "Clear filter (Esc)", false, ImVec2(clear_btn_size, clear_btn_size))) {
                s_TreeFilter[0] = '\0';
                s_DebouncedFilterText = "";
                s_FilterDebounceActive = false;
                s_RowsDirty = true;
            }
        }

        // Esc shortcut to clear filter (Spec §3.2)
        if (has_filter && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_TreeFilter[0] = '\0';
            s_DebouncedFilterText = "";
            s_FilterDebounceActive = false;
            s_RowsDirty = true;
        }

        ImGui::Spacing();

        // Handle 150ms filter debounce
        if (s_FilterDebounceActive) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_LastFilterEditTime).count();
            if (elapsed >= 150) {
                s_FilterDebounceActive = false;
                std::string new_filter = ToLower(s_TreeFilter);
                if (new_filter != s_DebouncedFilterText) {
                    s_DebouncedFilterText = new_filter;
                    s_RowsDirty = true;

                    // If user typed a query and directory is not yet fully indexed, launch background indexer (§2.3)
                    if (!s_DebouncedFilterText.empty() && !s_TreeFullyIndexed && !s_IsIndexing.load()) {
                        StartBackgroundIndexing(project_root);
                    }
                }
            }
        }

        // Check if revision or expansion changed
        if (project.Revision() != s_LastProjectRevision) {
            s_LastProjectRevision = project.Revision();
            s_RowsDirty = true;
        }

        if (s_ExpandRevision != s_LastRenderedExpandRev) {
            s_LastRenderedExpandRev = s_ExpandRevision;
            s_RowsDirty = true;
        }

        // Performance §2.2: Rebuild cached flat rows ONLY when dirty
        if (s_RowsDirty) {
            s_CachedFlatRows.clear();

            if (!s_ProjectTree.children.empty() || project.IsScanning()) {
                TreeRow root_row;
                root_row.id_str = s_ProjectTree.full_path_str;
                root_row.name = s_ProjectTree.name.empty() ? project_root.filename().generic_u8string() : s_ProjectTree.name;
                root_row.rel_path = "";
                root_row.type = TreeRowType::Folder;
                root_row.depth = 0;
                root_row.ancestor_has_next = 0;
                root_row.is_last_child = true;
                bool root_is_open = s_ExpandedFolders.find(s_ProjectTree.full_path_str) != s_ExpandedFolders.end();
                root_row.is_open = root_is_open;
                root_row.user_data = &s_ProjectTree;
                s_CachedFlatRows.push_back(root_row);

                if (root_is_open) {
                    std::vector<size_t> visible_root_children;
                    for (size_t i = 0; i < s_ProjectTree.children.size(); ++i) {
                        if (s_DebouncedFilterText.empty() || NodeMatchesFilter(s_ProjectTree.children[i], s_DebouncedFilterText)) {
                            visible_root_children.push_back(i);
                        }
                    }

                    const size_t count = visible_root_children.size();
                    for (size_t i = 0; i < count; ++i) {
                        const bool is_last = (i == count - 1);
                        FlattenNode(
                            s_ProjectTree.children[visible_root_children[i]],
                            1,
                            0,
                            is_last,
                            project,
                            s_DebouncedFilterText,
                            s_CachedFlatRows
                        );
                    }
                }
            }

            // Recompute cached selection index (§6.1 P4)
            s_CachedSelectedIndex = -1;
            if (!s_SelectedPath.empty()) {
                for (int i = 0; i < (int)s_CachedFlatRows.size(); ++i) {
                    if (s_CachedFlatRows[i].id_str == s_SelectedPath) {
                        s_CachedSelectedIndex = i;
                        break;
                    }
                }
            }
            s_RowsDirty = false;
        }

        // Scrollable tree area rendered via ImGuiListClipper over cached rows (Spec §2.2)
        const float scroll_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - Theme::Sp::S1);
        if (ImGui::BeginChild("FileTreeInnerScroll", ImVec2(0, scroll_h), false)) {
            // Handle keyboard navigation for Project Explorer (§3.5 & §6.1 P4)
            bool pane_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            ImGuiIO& io = ImGui::GetIO();

            if (pane_focused && !io.WantTextInput) {
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
                    s_FocusFilterRequested = true;
                }

                if (!s_CachedFlatRows.empty()) {
                    int sel_idx = s_CachedSelectedIndex;
                    if (sel_idx < 0 && !s_SelectedPath.empty()) {
                        for (int i = 0; i < (int)s_CachedFlatRows.size(); ++i) {
                            if (s_CachedFlatRows[i].id_str == s_SelectedPath) {
                                sel_idx = i;
                                s_CachedSelectedIndex = i;
                                break;
                            }
                        }
                    }

                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                        int new_idx = (sel_idx > 0) ? (sel_idx - 1) : 0;
                        s_SelectedPath = s_CachedFlatRows[new_idx].id_str;
                        s_CachedSelectedIndex = new_idx;
                        s_ScrollToIndex = new_idx;
                    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                        int new_idx = (sel_idx >= 0 && sel_idx + 1 < (int)s_CachedFlatRows.size()) ? (sel_idx + 1) : (sel_idx < 0 ? 0 : sel_idx);
                        s_SelectedPath = s_CachedFlatRows[new_idx].id_str;
                        s_CachedSelectedIndex = new_idx;
                        s_ScrollToIndex = new_idx;
                    } else if (sel_idx >= 0) {
                        const TreeRow& cur = s_CachedFlatRows[sel_idx];
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                            if (cur.type == TreeRowType::Folder) {
                                if (!cur.is_open) {
                                    s_ExpandedFolders.insert(cur.id_str);
                                    s_ExpandRevision++;
                                    s_RowsDirty = true;
                                } else if (sel_idx + 1 < (int)s_CachedFlatRows.size()) {
                                    s_SelectedPath = s_CachedFlatRows[sel_idx + 1].id_str;
                                    s_ScrollToIndex = sel_idx + 1;
                                }
                            }
                        } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                            if (cur.type == TreeRowType::Folder && cur.is_open) {
                                s_ExpandedFolders.erase(cur.id_str);
                                s_ExpandRevision++;
                                s_RowsDirty = true;
                            } else if (cur.depth > 0) {
                                for (int p = sel_idx - 1; p >= 0; --p) {
                                    if (s_CachedFlatRows[p].depth == cur.depth - 1) {
                                        s_SelectedPath = s_CachedFlatRows[p].id_str;
                                        s_ScrollToIndex = p;
                                        break;
                                    }
                                }
                            }
                        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                            if (cur.type == TreeRowType::Folder) {
                                if (cur.is_open) {
                                    s_ExpandedFolders.erase(cur.id_str);
                                } else {
                                    s_ExpandedFolders.insert(cur.id_str);
                                }
                                s_ExpandRevision++;
                                s_RowsDirty = true;
                            } else if (cur.type == TreeRowType::SourceFile) {
                                if (cur.in_build) {
                                    project.RemoveFile(cur.rel_path);
                                } else {
                                    project.AddFile(cur.rel_path);
                                }
                            }
                        }
                    }
                }
            }
            // Empty state decided strictly from model tree, not visible rows (§6.2 B1)
            bool show_empty_state = false;
            if (!project.IsScanning() && !s_IsIndexing.load() && s_DebouncedFilterText.empty()) {
                if (s_ProjectTree.children.empty()) {
                    show_empty_state = true;
                } else if (s_TreeFullyIndexed && !NodeContainsSourceOrHeader(s_ProjectTree)) {
                    show_empty_state = true;
                }
            }

            if (!project.HasProject()) {
                // Welcome / Empty state cleanly padded & positioned inside pane bounds
                const float pad_x = Theme::Sp::S4;
                const float avail_w = ImGui::GetContentRegionAvail().x;
                const float box_w = std::max(60.0f, avail_w - pad_x * 2.0f);
                ImGui::SetCursorPos(ImVec2(pad_x, std::max(Theme::Sp::S4, ImGui::GetContentRegionAvail().y * 0.28f)));
                ImGui::PushTextWrapPos(pad_x + box_w);
                ImGui::TextColored(Theme::Col::Text, "No project opened.");
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                ImGui::TextColored(Theme::Col::TextDim, "Open a project folder or drag and drop a folder here to browse C/C++ sources.");
                ImGui::PopTextWrapPos();

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                if (Theme::Button(ICON_FA_FOLDER_OPEN " Open Folder", "Pick a C/C++ project folder", false, ImVec2(std::min(box_w, 150.0f * Theme::DpiScale), Theme::Btn::DefaultH))) {
                    nfdchar_t* outPath = nullptr;
                    nfdresult_t res = NFD_PickFolder(&outPath, nullptr);
                    if (res == NFD_OKAY && outPath != nullptr) {
                        project.Load(std::filesystem::u8path(outPath));
                        RefreshFileTree();
                        NFD_FreePath(outPath);
                    }
                }
            } else if (show_empty_state) {
                // Actionable empty state cleanly padded inside pane bounds
                const float pad_x = Theme::Sp::S4;
                const float avail_w = ImGui::GetContentRegionAvail().x;
                const float box_w = std::max(60.0f, avail_w - pad_x * 2.0f);
                ImGui::SetCursorPos(ImVec2(pad_x, std::max(Theme::Sp::S4, ImGui::GetContentRegionAvail().y * 0.25f)));
                ImGui::PushTextWrapPos(pad_x + box_w);
                if (project.IsBuildOutputFolder()) {
                    ImGui::TextColored(Theme::Col::Warning, "%s Notice: This looks like a build output folder.", ICON_FA_TRIANGLE_EXCLAMATION);
                    ImGui::Spacing();
                    ImGui::SetCursorPosX(pad_x);
                    ImGui::TextColored(Theme::Col::TextDim, "CMake or Ninja cache detected. Auto-fill scan was skipped to prevent queuing build artifacts.");
                } else {
                    ImGui::TextColored(Theme::Col::TextDim, "No C/C++ files found in this folder.");
                }
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                ImGui::PushFont(g_Fonts.mono);
                ImGui::TextColored(Theme::Col::Text, "%s", project_root.generic_u8string().c_str());
                ImGui::PopFont();
                ImGui::PopTextWrapPos();

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                if (Theme::Button(ICON_FA_FOLDER_OPEN " Open Folder", "Pick another project folder", false, ImVec2(std::min(box_w, 150.0f * Theme::DpiScale), Theme::Btn::DefaultH))) {
                    nfdchar_t* outPath = nullptr;
                    std::string cur_dir = project_root.generic_u8string();
                    nfdresult_t res = NFD_PickFolder(&outPath, cur_dir.empty() ? nullptr : cur_dir.c_str());
                    if (res == NFD_OKAY && outPath != nullptr) {
                        project.Load(std::filesystem::u8path(outPath));
                        RefreshFileTree();
                        NFD_FreePath(outPath);
                    }
                }
            } else if (!s_DebouncedFilterText.empty() && s_CachedFlatRows.size() <= 1) {
                // Filter matches nothing cleanly padded
                const float pad_x = Theme::Sp::S4;
                const float box_w = std::max(60.0f, ImGui::GetContentRegionAvail().x - pad_x * 2.0f);
                ImGui::SetCursorPos(ImVec2(pad_x, Theme::Sp::S4));
                ImGui::PushTextWrapPos(pad_x + box_w);
                ImGui::TextColored(Theme::Col::TextDim, "No matches for '%s'.", s_TreeFilter);
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                if (Theme::Button("Clear Filter", "Reset filter search box", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                    s_TreeFilter[0] = '\0';
                    s_DebouncedFilterText = "";
                    s_FilterDebounceActive = false;
                    s_RowsDirty = true;
                }
            } else {
                // Eliminate item spacing gap between rows for continuous guide lines & exact clipper range (§6.1 P3)
                ImGuiStyle& style = ImGui::GetStyle();
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));

                // Virtualized clipper rendering of flat tree rows (zero allocation per frame!)
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(s_CachedFlatRows.size()), Theme::RowHeight);
                if (s_ScrollToIndex >= 0 && s_ScrollToIndex < (int)s_CachedFlatRows.size()) {
                    clipper.IncludeItemByIndex(s_ScrollToIndex);
                }

                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        TreeRow& r = s_CachedFlatRows[i];
                        if (i == s_ScrollToIndex) {
                            ImGui::SetScrollHereY(0.5f);
                            s_ScrollToIndex = -1;
                        }
                        TreeRowResult res = RenderTreeRow(r, s_SelectedPath, false, true);

                        if ((r.type == TreeRowType::Folder || r.type == TreeRowType::SourceFile) &&
                            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                            std::string payload = (r.type == TreeRowType::Folder ? "D:" : "F:") + r.rel_path;
                            ImGui::SetDragDropPayload("GENIE_EXPLORER_PATH", payload.c_str(), payload.size() + 1);
                            ImGui::TextUnformatted(r.name.c_str());
                            ImGui::EndDragDropSource();
                        }

                        if (res.selected_changed) {
                            s_SelectedPath = r.id_str;
                            s_CachedSelectedIndex = i;
                        }

                        if (res.toggle_expanded && r.type == TreeRowType::Folder) {
                            if (s_ExpandedFolders.find(r.id_str) != s_ExpandedFolders.end()) {
                                s_ExpandedFolders.erase(r.id_str);
                            } else {
                                s_ExpandedFolders.insert(r.id_str);
                            }
                            s_ExpandRevision++;
                            s_RowsDirty = true;
                        }

                        if (res.double_clicked) {
                            if (r.type == TreeRowType::Folder) {
                                if (s_ExpandedFolders.find(r.id_str) != s_ExpandedFolders.end()) {
                                    s_ExpandedFolders.erase(r.id_str);
                                } else {
                                    s_ExpandedFolders.insert(r.id_str);
                                }
                                s_ExpandRevision++;
                                s_RowsDirty = true;
                            } else if (r.type == TreeRowType::SourceFile) {
#ifndef NDEBUG
                                auto t0 = std::chrono::high_resolution_clock::now();
#endif
                                if (r.in_build) {
                                    project.RemoveFile(r.rel_path);
                                } else {
                                    project.AddFile(r.rel_path);
                                }
#ifndef NDEBUG
                                auto t1 = std::chrono::high_resolution_clock::now();
                                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                                if (ms > 1.0) {
                                    std::cout << "[PERF] Double click toggle took " << ms << " ms\n";
                                }
#endif
                            }
                        }

                        // Tooltip on row hover (bound strictly to row, never obscured by hover sub-buttons)
                        if (res.tooltip_hovered) {
                            std::string ov_suffix = (r.has_override && !r.override_tooltip.empty()) ? ("\n\n" + r.override_tooltip) : "";
                            if (r.type == TreeRowType::Folder) {
                                std::string tip = "Folder: " + r.name + "\nDouble-click to expand/collapse" + ov_suffix;
                                Theme::Tooltip(tip.c_str());
                            } else if (r.type == TreeRowType::SourceFile) {
                                auto owners = project.GetTargetsOwningFile(r.rel_path);
                                if (!owners.empty() && project.IsMultiTargetOrRulesUI()) {
                                    std::string joined;
                                    for (size_t oi = 0; oi < owners.size(); ++oi) {
                                        if (oi > 0) joined += ", ";
                                        joined += owners[oi];
                                    }
                                    std::string tip = "In targets (" + joined + "): " + r.rel_path + ov_suffix;
                                    Theme::Tooltip(tip.c_str());
                                } else if (r.in_build) {
                                    std::string tip = "In build list: " + r.rel_path + "\nDouble-click to remove from build list" + ov_suffix;
                                    Theme::Tooltip(tip.c_str());
                                } else {
                                    std::string tip = std::string(r.is_c ? "C" : "C++") + " source: " + r.rel_path + "\nDouble-click to add to build list" + ov_suffix;
                                    Theme::Tooltip(tip.c_str());
                                }
                            } else {
                                std::string tip = "Header file (context only): " + r.rel_path + "\nHeaders cannot be compiled directly.";
                                Theme::Tooltip(tip.c_str());
                            }
                        }

                        // Context menu popup per row
                        if (ImGui::BeginPopupContextItem()) {
                            const auto& targets = project.GetTargets();
                            if (r.type == TreeRowType::Folder) {
                                std::string rule_pat = r.rel_path.empty()
                                    ? "**/*.{c,cc,cpp,cxx}"
                                    : (r.rel_path + "/**/*.{c,cc,cpp,cxx}");
                                if (targets.size() <= 1) {
                                    if (ImGui::MenuItem("Add folder as rule")) {
                                        core::SourceRule rule;
                                        rule.include = rule_pat;
                                        project.AddSourceRuleToTarget("", rule);
                                    }
                                } else {
                                    if (ImGui::BeginMenu("Add folder as rule to")) {
                                        for (const auto& t : targets) {
                                            if (t.kind == core::TargetKind::Interface) continue;
                                            if (ImGui::MenuItem(t.name.c_str())) {
                                                core::SourceRule rule;
                                                rule.include = rule_pat;
                                                project.AddSourceRuleToTarget(t.name, rule);
                                            }
                                        }
                                        ImGui::EndMenu();
                                    }
                                }
                                if (ImGui::MenuItem("Add all sources in folder")) {
                                    std::filesystem::path folder_p = project_root / r.rel_path;
                                    std::vector<std::string> sources;
                                    CollectSourcesInFolder(folder_p, project_root, sources);
                                    if (!sources.empty()) {
                                        project.AddFiles(sources);
                                    }
                                }
                                if (ImGui::MenuItem("Remove all from folder")) {
                                    if ((r.depth == 0 || r.rel_path.empty()) && project.GetFiles().size() > 20) {
                                        s_ShowExplorerRemoveAllModal = true;
                                    } else {
                                        RemoveSourcesInFolder(r.rel_path, project);
                                    }
                                }
                                if (!r.rel_path.empty()) {
                                    ImGui::Separator();
                                    if (targets.size() <= 1) {
                                        if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this folder...")) {
                                            OpenBuildSettingsForOverride(targets.empty() ? "" : targets[0].name, r.rel_path, "");
                                        }
                                    } else {
                                        if (ImGui::BeginMenu(ICON_FA_FLAG "  Set flags for this folder...")) {
                                            for (const auto& t : targets) {
                                                if (t.kind == core::TargetKind::Interface) continue;
                                                if (ImGui::MenuItem(t.name.c_str())) {
                                                    OpenBuildSettingsForOverride(t.name, r.rel_path, "");
                                                }
                                            }
                                            ImGui::EndMenu();
                                        }
                                    }
                                }
#if defined(_WIN32)
                                if (ImGui::MenuItem("Reveal in File Explorer")) {
                                    std::filesystem::path folder_p = project_root / r.rel_path;
                                    ShellExecuteW(NULL, L"open", folder_p.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }
#endif
                            } else if (r.type == TreeRowType::SourceFile) {
                                if (targets.size() > 1) {
                                    if (ImGui::BeginMenu("Add to target")) {
                                        for (const auto& t : targets) {
                                            if (t.kind == core::TargetKind::Interface) continue;
                                            if (ImGui::MenuItem(t.name.c_str())) {
                                                project.AddFilesToTarget(t.name, { r.rel_path });
                                            }
                                        }
                                        ImGui::EndMenu();
                                    }
                                }
                                if (r.in_build) {
                                    if (ImGui::MenuItem("Remove from build list")) {
                                        project.RemoveFile(r.rel_path);
                                    }
                                } else {
                                    if (ImGui::MenuItem("Add to build list")) {
                                        project.AddFile(r.rel_path);
                                    }
                                }
                                {
                                    auto owners = project.GetTargetsOwningFile(r.rel_path);
                                    if (owners.size() == 1) {
                                        if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this file...")) {
                                            OpenBuildSettingsForOverride(owners[0], "", r.rel_path);
                                        }
                                    } else if (owners.size() > 1) {
                                        if (ImGui::BeginMenu(ICON_FA_FLAG "  Set flags for this file...")) {
                                            for (const auto& oname : owners) {
                                                if (ImGui::MenuItem(oname.c_str())) {
                                                    OpenBuildSettingsForOverride(oname, "", r.rel_path);
                                                }
                                            }
                                            ImGui::EndMenu();
                                        }
                                    } else if (targets.size() <= 1) {
                                        if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this file...")) {
                                            OpenBuildSettingsForOverride(targets.empty() ? "" : targets[0].name, "", r.rel_path);
                                        }
                                    }
                                }
                                if (ImGui::MenuItem("Copy relative path")) {
                                    ImGui::SetClipboardText(r.rel_path.c_str());
                                }
#if defined(_WIN32)
                                if (ImGui::MenuItem("Reveal in File Explorer")) {
                                    std::filesystem::path full_p = project_root / r.rel_path;
                                    std::wstring param = L"/select,\"" + full_p.wstring() + L"\"";
                                    ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOWNORMAL);
                                }
#endif
                            } else if (r.type == TreeRowType::HeaderFile) {
                                if (ImGui::MenuItem("Copy relative path")) {
                                    ImGui::SetClipboardText(r.rel_path.c_str());
                                }
#if defined(_WIN32)
                                if (ImGui::MenuItem("Reveal in File Explorer")) {
                                    std::filesystem::path full_p = project_root / r.rel_path;
                                    std::wstring param = L"/select,\"" + full_p.wstring() + L"\"";
                                    ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOWNORMAL);
                                }
#endif
                            }
                            ImGui::EndPopup();
                        }
                    }
                }
                ImGui::PopStyleVar();
            }
        }
        ImGui::EndChild();

        // Confirm popup modal for root "Remove all" if list has > 20 files (Spec §3.1)
        if (s_ShowExplorerRemoveAllModal) {
            ImGui::OpenPopup("Confirm Remove All##ExplorerModal");
        }
        bool rem_close_x = false;
        if (Theme::BeginModal("Confirm Remove All##ExplorerModal", "Remove All Files", ICON_FA_TRASH, Theme::Col::Warning, 400.0f, &rem_close_x)) {
            ImGui::TextColored(Theme::Col::Text, "Remove all %zu files from the build list?", project.GetFiles().size());
            ImGui::Spacing();
            ImGui::TextColored(Theme::Col::TextDim, "Your source files on disk will not be modified.");

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const float dpi = Theme::DpiScale;
            const float btn_h = Theme::Btn::DefaultH;
            const float rem_w = 104.0f * dpi;
            const float cancel_w = 78.0f * dpi;
            const float right_end = 400.0f * dpi - Theme::Sp::S4;

            ImGui::SetCursorPosX(right_end - rem_w - Theme::Sp::S2 - cancel_w);
            if (Theme::Button("Cancel", "Keep files in build list", false, ImVec2(cancel_w, btn_h)) || rem_close_x) {
                s_ShowExplorerRemoveAllModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0.0f, Theme::Sp::S2);
            if (Theme::Button("Remove all", "Confirm removal of all files from build list", true, ImVec2(rem_w, btn_h))) {
                project.ClearFiles();
                s_ShowExplorerRemoveAllModal = false;
                ImGui::CloseCurrentPopup();
            }
            Theme::EndModal();
        }
    }
    Theme::PaneEnd();
}

} // namespace ui
