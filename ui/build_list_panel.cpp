#include "build_list_panel.h"
#include "build_settings_modal.h"
#include "source_rules.h"
#include "top_bar.h"
#include "theme.h"
#include "icons.h"
#include "tree_view.h"
#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace ui {

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

// -----------------------------------------------------------------------------
// Hierarchical folder/file tree model (reused for v1 Build List and per-target files)
// -----------------------------------------------------------------------------
struct BuildTreeNode {
    std::string name;
    std::string name_lower;
    std::string full_folder_path;
    bool is_folder = false;
    std::string file_rel_path;
    bool is_c_source = false;
    bool is_missing = false;
    bool is_enabled = true;
    int total_files = 0;
    int missing_files = 0;
    int enabled_files = 0;
    std::vector<std::string> all_files;
    std::vector<BuildTreeNode> children;
};

class BuildTreeModel {
public:
    BuildTreeNode root;
    uint64_t last_revision = (uint64_t)-1;

    void RebuildFromTarget(const core::Project& project, const core::Target* target) {
        root = BuildTreeNode{};
        root.is_folder = true;
        root.name = "";
        root.name_lower = "";
        root.full_folder_path = "";

        const std::vector<std::string>& files = target ? target->expandedFiles : project.GetFiles();

        for (const auto& file_path : files) {
            std::string norm = NormalizePath(file_path);
            if (norm.empty()) continue;

            std::vector<std::string> segments;
            size_t start = 0;
            while (start < norm.size()) {
                size_t slash = norm.find('/', start);
                if (slash == std::string::npos) {
                    segments.push_back(norm.substr(start));
                    break;
                } else {
                    segments.push_back(norm.substr(start, slash - start));
                    start = slash + 1;
                }
            }
            if (segments.empty()) continue;

            BuildTreeNode* current = &root;
            std::string cur_path = "";

            for (size_t s = 0; s + 1 < segments.size(); ++s) {
                if (!cur_path.empty()) cur_path += "/";
                cur_path += segments[s];

                BuildTreeNode* found = nullptr;
                for (auto& child : current->children) {
                    if (child.is_folder && child.name == segments[s]) {
                        found = &child;
                        break;
                    }
                }
                if (!found) {
                    BuildTreeNode new_folder;
                    new_folder.is_folder = true;
                    new_folder.name = segments[s];
                    new_folder.name_lower = ToLower(new_folder.name);
                    new_folder.full_folder_path = cur_path;
                    current->children.push_back(std::move(new_folder));
                    found = &current->children.back();
                }
                current = found;
            }

            const std::string& fname = segments.back();
            BuildTreeNode file_node;
            file_node.is_folder = false;
            file_node.name = fname;
            file_node.name_lower = ToLower(fname);
            file_node.file_rel_path = norm;

            std::string ext = "";
            size_t dot_pos = fname.find_last_of('.');
            if (dot_pos != std::string::npos) {
                ext = ToLower(fname.substr(dot_pos));
            }
            file_node.is_c_source = (ext == ".c");
            if (target) {
                file_node.is_missing = (target->missingFilesSet.find(norm) != target->missingFilesSet.end());
                file_node.is_enabled = target->IsFileEnabled(norm);
            } else {
                file_node.is_missing = project.IsFileMissingCached(norm);
                file_node.is_enabled = project.IsFileEnabled(norm);
            }
            file_node.total_files = 1;
            file_node.missing_files = file_node.is_missing ? 1 : 0;
            file_node.enabled_files = file_node.is_enabled ? 1 : 0;
            file_node.all_files.push_back(norm);

            current->children.push_back(std::move(file_node));
        }

        PostProcess(root);
        last_revision = project.Revision();
    }

    void Rebuild(const core::Project& project) {
        RebuildFromTarget(project, nullptr);
    }

private:
    void PostProcess(BuildTreeNode& node) {
        node.total_files = 0;
        node.missing_files = 0;
        node.enabled_files = 0;
        node.all_files.clear();

        if (!node.is_folder) {
            node.total_files = 1;
            node.missing_files = node.is_missing ? 1 : 0;
            node.enabled_files = node.is_enabled ? 1 : 0;
            node.all_files.push_back(node.file_rel_path);
            return;
        }

        std::sort(node.children.begin(), node.children.end(), [](const BuildTreeNode& a, const BuildTreeNode& b) {
            if (a.is_folder != b.is_folder) return a.is_folder > b.is_folder;
            return a.name < b.name;
        });

        for (auto& child : node.children) {
            PostProcess(child);
            node.total_files += child.total_files;
            node.missing_files += child.missing_files;
            node.enabled_files += child.enabled_files;
            node.all_files.insert(node.all_files.end(), child.all_files.begin(), child.all_files.end());
        }
        node.is_missing = (node.missing_files > 0);
    }
};

static BuildTreeModel s_BuildTreeModel;
static std::unordered_map<std::string, BuildTreeModel> s_TargetTrees;
static std::unordered_set<std::string> s_CollapsedFolders;
static std::unordered_set<std::string> s_CollapsedTargets;
static std::string s_SelectedBuildRow;
static int s_CachedSelectedBuildIndex = -1;
static bool s_FocusFilterRequested = false;
static int s_ScrollToIndex = -1;
static bool s_ShowClearAllModal = false;
static size_t s_LastKnownFilesCount = 0;
static std::string s_AddedFeedbackText = "";
static std::chrono::steady_clock::time_point s_AddedFeedbackExpiry;

// Modal states for V2-1 / V2-2
static bool s_ShowNewTargetWizard = false;
static bool s_ShowRuleEditor = false;
static bool s_ShowRenameTargetModal = false;
static bool s_ShowDeleteTargetModal = false;
static bool s_ShowDropFolderModal = false;
static bool s_ShowSimplifyModal = false;

static std::string s_RuleEditTarget = "";
static int s_RuleEditIndex = -1; // -1 = new rule
static char s_RuleIncludeBuf[512] = "";
static char s_RuleCondBuf[256] = "";
static std::vector<std::string> s_RuleExcludeList;
static bool s_RulePreviewDirty = true;
static std::chrono::steady_clock::time_point s_RuleLastEditTime;
static core::RulePreviewResult s_RulePreviewCache;

static std::string s_RenameOldName = "";
static char s_RenameNewBuf[128] = "";

static std::string s_DeleteTargetName = "";

static std::string s_DropTargetName = "";
static std::string s_DropFolderRel = "";

static std::string s_SimplifyTargetName = "";
static core::SimplifySuggestion s_SimplifySug;

void OpenNewTargetWizard() {
    s_ShowNewTargetWizard = true;
}

static void OpenRuleEditor(const std::string& target_name, int rule_idx, const core::SourceRule& rule) {
    s_RuleEditTarget = target_name;
    s_RuleEditIndex = rule_idx;
    std::strncpy(s_RuleIncludeBuf, rule.include.c_str(), sizeof(s_RuleIncludeBuf) - 1);
    s_RuleIncludeBuf[sizeof(s_RuleIncludeBuf) - 1] = '\0';
    std::strncpy(s_RuleCondBuf, rule.condition.c_str(), sizeof(s_RuleCondBuf) - 1);
    s_RuleCondBuf[sizeof(s_RuleCondBuf) - 1] = '\0';
    s_RuleExcludeList = rule.exclude;
    s_RulePreviewDirty = true;
    s_RuleLastEditTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
    s_ShowRuleEditor = true;
}

static void CollectAllFolderPaths(const BuildTreeNode& node, std::unordered_set<std::string>& out_folders) {
    if (node.is_folder && !node.full_folder_path.empty()) {
        out_folders.insert(node.full_folder_path);
    }
    for (const auto& child : node.children) {
        if (child.is_folder) {
            CollectAllFolderPaths(child, out_folders);
        }
    }
}

static std::vector<TreeRow> s_CachedFlatRows;
static bool s_RowsDirty = true;
static uint64_t s_LastProjectRevision = (uint64_t)-1;
static uint64_t s_ExpandRevision = 0;
static uint64_t s_LastRenderedExpandRev = (uint64_t)-1;

static std::string s_DebouncedFilterText = "";
static std::chrono::steady_clock::time_point s_LastFilterEditTime;
static bool s_FilterDebounceActive = false;

static bool NodeMatchesFilter(const BuildTreeNode& node, const std::string& filter_lower) {
    if (filter_lower.empty()) return true;
    if (node.name_lower.find(filter_lower) != std::string::npos) return true;
    if (node.is_folder) {
        for (const auto& child : node.children) {
            if (NodeMatchesFilter(child, filter_lower)) return true;
        }
    }
    return false;
}

static bool BuildOverrideMatchesFolder(const std::string& match_pat, const std::string& folder_rel) {
    if (folder_rel.empty()) return false;
    return match_pat == folder_rel ||
           match_pat == (folder_rel + "/**") ||
           match_pat == (folder_rel + "/*");
}

static void PopulateBuildRowOverride(const core::Target* target, const std::string& active_cfg, TreeRow& row, bool is_folder) {
    if (!target || target->overrides.empty() || row.rel_path.empty()) return;
    std::vector<std::string> lines;
    for (const auto& ov : target->overrides) {
        bool matched = is_folder
            ? BuildOverrideMatchesFolder(ov.match, row.rel_path)
            : core::CompiledGlob::Compile(ov.match).Matches(row.rel_path);
        if (!matched) continue;
        std::string line = ov.match;
        if (!ov.condition.empty()) line += " (if " + ov.condition + ")";
        std::vector<std::string> parts;
        auto add_s = [&](const std::string& opt, const std::string& dbg, bool p, bool u,
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
        add_s(ov.optimize, ov.debugInfo, ov.excludeFromPch, ov.excludeFromUnity,
              ov.settings.defines, ov.settings.compileFlags, ov.settings.includeDirs);
        auto cit = ov.configs.find(active_cfg);
        if (cit != ov.configs.end()) {
            add_s(cit->second.optimize, cit->second.debugInfo, false, false,
                  cit->second.defines, cit->second.compileFlags, {});
        }
        if (!parts.empty()) {
            line += ": ";
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) line += ", ";
                line += parts[i];
            }
        }
        lines.push_back(line);
    }
    if (!lines.empty()) {
        row.has_override = true;
        row.override_tooltip = "Flag overrides:";
        for (const auto& l : lines) row.override_tooltip += "\n  " + l;
    }
}

static void FlattenBuildTree(
    BuildTreeNode& node,
    int depth,
    uint64_t ancestor_has_next,
    bool is_last_child,
    const std::string& filter_lower,
    const std::string& key_prefix,
    const core::Target* target,
    const std::string& active_cfg,
    std::vector<TreeRow>& out_rows
) {
    if (!filter_lower.empty() && !NodeMatchesFilter(node, filter_lower)) {
        return;
    }

    std::string collapse_key = key_prefix + node.full_folder_path;

    TreeRow row;
    row.id_str = node.is_folder ? ("dir:" + collapse_key) : ("file:" + key_prefix + node.file_rel_path);
    row.name = node.name;
    row.rel_path = node.is_folder ? node.full_folder_path : node.file_rel_path;
    row.depth = depth;
    row.ancestor_has_next = ancestor_has_next;
    row.is_last_child = is_last_child;
    row.user_data = &node;

    if (node.is_folder) {
        row.type = TreeRowType::Folder;
        row.show_checkbox = true;
        if (node.enabled_files == node.total_files && node.total_files > 0) row.check_state = 1;
        else if (node.enabled_files == 0) row.check_state = 0;
        else row.check_state = 2;

        row.is_disabled = (node.enabled_files == 0);
        row.file_count = node.total_files;
        row.missing_count = node.missing_files;
        row.is_missing = node.is_missing;

        if (node.missing_files > 0) {
            row.extra_label = " (" + std::to_string(node.total_files) + ", " + std::to_string(node.missing_files) + " missing)";
        } else {
            row.extra_label = " (" + std::to_string(node.total_files) + ")";
        }

        bool is_open = (!filter_lower.empty()) || (s_CollapsedFolders.find(collapse_key) == s_CollapsedFolders.end());
        row.is_open = is_open;
        PopulateBuildRowOverride(target, active_cfg, row, true);
        out_rows.push_back(row);

        if (is_open) {
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

                FlattenBuildTree(
                    node.children[visible_child_indices[i]],
                    depth + 1,
                    next_ancestor_mask,
                    child_is_last,
                    filter_lower,
                    key_prefix,
                    target,
                    active_cfg,
                    out_rows
                );
            }
        }
    } else {
        row.type = TreeRowType::SourceFile;
        row.show_checkbox = true;
        row.is_c = node.is_c_source;
        row.is_missing = node.is_missing;
        row.check_state = node.is_enabled ? 1 : 0;
        row.is_disabled = !node.is_enabled;
        PopulateBuildRowOverride(target, active_cfg, row, false);
        out_rows.push_back(row);
    }
}

static void CollectAllSources(const std::filesystem::path& root_path, std::vector<std::string>& out_sources) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root_path, std::filesystem::directory_options::skip_permission_denied, ec);
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

// Draws crisp vector kind icons (▶ exe, ▣ static, ◈ shared, ◇ interface)
static void DrawTargetKindIcon(ImDrawList* dl, ImVec2 center, float size, core::TargetKind kind, ImU32 col) {
    float r = size * 0.45f;
    if (kind == core::TargetKind::Executable) {
        dl->AddTriangleFilled(
            ImVec2(center.x - r * 0.6f, center.y - r * 0.85f),
            ImVec2(center.x - r * 0.6f, center.y + r * 0.85f),
            ImVec2(center.x + r * 0.95f, center.y),
            col
        );
    } else if (kind == core::TargetKind::Static) {
        dl->AddRect(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), col, 1.5f, 0, 1.2f);
        dl->AddRectFilled(ImVec2(center.x - r * 0.45f, center.y - r * 0.45f), ImVec2(center.x + r * 0.45f, center.y + r * 0.45f), col);
    } else if (kind == core::TargetKind::Shared) {
        ImVec2 pts[4] = {
            ImVec2(center.x, center.y - r),
            ImVec2(center.x + r, center.y),
            ImVec2(center.x, center.y + r),
            ImVec2(center.x - r, center.y)
        };
        dl->AddQuadFilled(pts[0], pts[1], pts[2], pts[3], col);
    } else {
        ImVec2 pts[4] = {
            ImVec2(center.x, center.y - r),
            ImVec2(center.x + r, center.y),
            ImVec2(center.x, center.y + r),
            ImVec2(center.x - r, center.y)
        };
        dl->AddQuad(pts[0], pts[1], pts[2], pts[3], col, 1.3f);
    }
}

// Renders target rows + rules + deps + per-target files when V2-1 / V2-2 features are active
static void RenderTargetsTree(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines
) {
    const float dpi = Theme::DpiScale;
    const auto& targets = project.GetTargets();
    auto multi_target_files = project.GetMultiTargetFilesCopy();

    std::string comp_low = ToLower(project.GetCompiler());
    std::string comp_family = "gcc";
    if (comp_low == "cl" || comp_low == "cl.exe") comp_family = "msvc";
    else if (comp_low.find("clang") != std::string::npos) comp_family = "clang";
#if defined(_WIN32)
    const std::string os_name = "windows";
#elif defined(__APPLE__)
    const std::string os_name = "macos";
#else
    const std::string os_name = "linux";
#endif

    // Rebuild per-target folder trees on project revision change
    static uint64_t s_LastTargetsTreeRev = (uint64_t)-1;
    if (project.Revision() != s_LastTargetsTreeRev) {
        s_LastTargetsTreeRev = project.Revision();
        s_TargetTrees.clear();
        for (const auto& t : targets) {
            s_TargetTrees[t.name].RebuildFromTarget(project, &t);
        }
    }

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));

    for (const auto& t : targets) {
        ImGui::PushID(("tgt_" + t.name).c_str());

        bool is_open = (s_CollapsedTargets.find(t.name) == s_CollapsedTargets.end());
        if (!s_DebouncedFilterText.empty()) is_open = true;

        ImVec2 row_pos = ImGui::GetCursorScreenPos();
        float row_w = ImGui::GetContentRegionAvail().x;
        float row_h = Theme::RowHeight + 2.0f * dpi;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool clicked = ImGui::Selectable("##tgt_row", s_SelectedBuildRow == ("tgt:" + t.name), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row_h));
        ImVec2 next_pos = ImGui::GetCursorPos();

        if (clicked) {
            s_SelectedBuildRow = "tgt:" + t.name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (is_open) s_CollapsedTargets.insert(t.name);
                else s_CollapsedTargets.erase(t.name);
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float mx = ImGui::GetMousePos().x;
            if (mx >= row_pos.x && mx <= row_pos.x + 20.0f * dpi) {
                if (is_open) s_CollapsedTargets.insert(t.name);
                else s_CollapsedTargets.erase(t.name);
            }
        }

        // Drag-and-drop from Explorer onto Target row (V2-2)
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GENIE_EXPLORER_PATH")) {
                std::string data(static_cast<const char*>(payload->Data));
                if (data.rfind("F:", 0) == 0) {
                    project.AddFilesToTarget(t.name, { data.substr(2) });
                } else if (data.rfind("D:", 0) == 0) {
                    s_DropTargetName = t.name;
                    s_DropFolderRel = data.substr(2);
                    s_ShowDropFolderModal = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Draw target header row contents
        float cx = row_pos.x + Theme::Sp::S1;
        float cy = row_pos.y + row_h * 0.5f;
        float mid_text_y = row_pos.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;

        dl->AddText(ImVec2(cx, mid_text_y), ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim), is_open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
        cx += 16.0f * dpi;

        ImVec4 tcol = Theme::GetTargetColor(t.name);
        DrawTargetKindIcon(dl, ImVec2(cx + 5.0f * dpi, cy), 11.0f * dpi, t.kind, ImGui::ColorConvertFloat4ToU32(tcol));
        cx += 16.0f * dpi;

        if (project.GetStartupTarget() == t.name) {
            dl->AddCircleFilled(ImVec2(cx + 2.0f * dpi, cy), 2.5f * dpi, ImGui::ColorConvertFloat4ToU32(Theme::Col::Success));
            cx += 9.0f * dpi;
        }

        dl->AddText(ImVec2(cx, mid_text_y), ImGui::ColorConvertFloat4ToU32(Theme::Col::Text), t.name.c_str());
        cx += ImGui::CalcTextSize(t.name.c_str()).x + 8.0f * dpi;

        const char* kind_short = (t.kind == core::TargetKind::Executable) ? "exe"
            : (t.kind == core::TargetKind::Static) ? "static"
            : (t.kind == core::TargetKind::Shared) ? "dll" : "interface";
        std::string meta = std::string(kind_short) + " · " + std::to_string(t.expandedFiles.size());
        if (!t.missingFiles.empty()) {
            meta += " (" + std::to_string(t.missingFiles.size()) + " missing)";
        }
        ImGui::PushFont(g_Fonts.secondary);
        dl->AddText(
            ImVec2(cx, mid_text_y),
            ImGui::ColorConvertFloat4ToU32(t.missingFiles.empty() ? Theme::Col::TextDim : Theme::Col::Error),
            meta.c_str()
        );
        ImGui::PopFont();

        ImGui::SetCursorPos(next_pos);

        // Target context menu (V2-2)
        if (ImGui::BeginPopupContextItem("##tgt_ctx")) {
            const bool busy = build_runner.IsRunning() || project.IsScanning();
            if (ImGui::MenuItem("Build target", nullptr, false, !busy)) {
                TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, t.name);
            }
            if (ImGui::MenuItem("Rebuild target", nullptr, false, !busy)) {
                TriggerRebuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, t.name);
            }
            if (ImGui::MenuItem("Clean target", nullptr, false, !busy)) {
                std::string clean_msg;
                if (build_runner.Clean(project, clean_msg, t.name)) {
                    core::OutLine ol;
                    ol.kind = core::OutLineKind::Status;
                    ol.text = clean_msg;
                    output_lines.push_back(ol);
                }
            }
            ImGui::Separator();
            if (t.kind == core::TargetKind::Executable) {
                bool is_startup = (project.GetStartupTarget() == t.name);
                if (ImGui::MenuItem("Set as startup", nullptr, is_startup)) {
                    project.SetStartupTarget(t.name);
                }
            }
            if (ImGui::MenuItem("Target settings...")) {
                OpenBuildSettingsModal(t.name);
            }
            if (ImGui::MenuItem("Add source rule...")) {
                core::SourceRule nr;
                nr.include = "src/**/*.{c,cc,cpp,cxx}";
                OpenRuleEditor(t.name, -1, nr);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) {
                s_RenameOldName = t.name;
                std::strncpy(s_RenameNewBuf, t.name.c_str(), sizeof(s_RenameNewBuf) - 1);
                s_RenameNewBuf[sizeof(s_RenameNewBuf) - 1] = '\0';
                s_ShowRenameTargetModal = true;
            }
            if (ImGui::MenuItem("Duplicate")) {
                std::string new_name;
                project.DuplicateTarget(t.name, new_name);
            }
            if (targets.size() > 1) {
                if (ImGui::MenuItem("Delete...")) {
                    s_DeleteTargetName = t.name;
                    s_ShowDeleteTargetModal = true;
                }
            }
            ImGui::EndPopup();
        }

        if (is_open) {
            const float sub_indent = 20.0f * dpi;

            // 1. Deps line (→ core, math)
            if (!t.deps.empty() || !t.publicDeps.empty()) {
                std::string dep_line = "  -> ";
                bool first_d = true;
                for (const auto& d : t.deps) {
                    if (!first_d) dep_line += ", ";
                    dep_line += d;
                    first_d = false;
                }
                for (const auto& pd : t.publicDeps) {
                    if (!first_d) dep_line += ", ";
                    dep_line += pd + " (public)";
                    first_d = false;
                }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sub_indent);
                ImGui::PushFont(g_Fonts.secondary);
                ImGui::TextColored(Theme::Col::TextDim, "%s", dep_line.c_str());
                ImGui::PopFont();
            }

            // 2. Source rules rows (V2-1)
            for (size_t ri = 0; ri < t.sources.size(); ++ri) {
                const auto& rule = t.sources[ri];
                size_t match_cnt = (ri < t.ruleMatchCounts.size()) ? t.ruleMatchCounts[ri] : 0;
                bool active = rule.condition.empty() || core::EvaluateSimpleCondition(rule.condition, os_name, comp_family);

                ImGui::PushID(static_cast<int>(ri));
                ImVec2 rpos = ImGui::GetCursorScreenPos();
                float rh = Theme::RowHeight;
                bool rclicked = ImGui::Selectable("##rule_row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rh));
                ImVec2 rnext = ImGui::GetCursorPos();

                if (rclicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    OpenRuleEditor(t.name, static_cast<int>(ri), rule);
                }

                float rx = rpos.x + sub_indent;
                float rtext_y = rpos.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
                ImVec4 txt_col = active ? Theme::Col::Text : Theme::Col::Inactive;

                dl->AddText(ImVec2(rx, rtext_y), ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent), ICON_FA_COPY);
                rx += 16.0f * dpi;
                dl->AddText(ImVec2(rx, rtext_y), ImGui::ColorConvertFloat4ToU32(txt_col), rule.include.c_str());
                rx += ImGui::CalcTextSize(rule.include.c_str()).x + 8.0f * dpi;

                if (!rule.condition.empty()) {
                    std::string ctag = "[if " + rule.condition + "]";
                    ImGui::PushFont(g_Fonts.secondary);
                    dl->AddText(ImVec2(rx, rtext_y), ImGui::ColorConvertFloat4ToU32(active ? Theme::Col::TextDim : Theme::Col::Inactive), ctag.c_str());
                    rx += ImGui::CalcTextSize(ctag.c_str()).x + 8.0f * dpi;
                    ImGui::PopFont();
                }

                std::string cnt_s = std::to_string(match_cnt);
                ImVec4 cnt_col = (match_cnt == 0 && active) ? Theme::Col::Warning : Theme::Col::TextDim;
                ImGui::PushFont(g_Fonts.secondary);
                dl->AddText(ImVec2(rx, rtext_y), ImGui::ColorConvertFloat4ToU32(cnt_col), cnt_s.c_str());
                ImGui::PopFont();

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    if (!active) {
                        std::string tip = "Inactive rule (condition '" + rule.condition + "' is false)\nDouble-click to edit";
                        Theme::Tooltip(tip.c_str());
                    } else if (match_cnt == 0) {
                        Theme::Tooltip("Rule matches no files\nDouble-click to edit");
                    } else {
                        std::string tip = "Rule: " + rule.include + " (" + cnt_s + " files)\nDouble-click to edit";
                        Theme::Tooltip(tip.c_str());
                    }
                }

                ImGui::SetCursorPos(rnext);

                if (ImGui::BeginPopupContextItem("##rule_ctx")) {
                    if (ImGui::MenuItem("Edit rule...")) {
                        OpenRuleEditor(t.name, static_cast<int>(ri), rule);
                    }
                    if (ImGui::MenuItem("Remove rule")) {
                        core::Target copy = t;
                        copy.sources.erase(copy.sources.begin() + static_cast<ptrdiff_t>(ri));
                        project.UpdateTarget(copy);
                    }
                    ImGui::EndPopup();
                }

                // Rule exclude sub-rows (− pattern)
                for (const auto& ex : rule.exclude) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sub_indent + 16.0f * dpi);
                    ImGui::PushFont(g_Fonts.secondary);
                    ImGui::TextColored(Theme::Col::TextDim, "- %s", ex.c_str());
                    ImGui::PopFont();
                }

                ImGui::PopID();
            }

            // 3. Simplify suggestion (>50 explicit files)
            if (t.files.size() > 50) {
                auto sugs = core::ComputeSimplifySuggestions(project.GetRootPath(), t.files, project.GetDirectoryCache());
                if (!sugs.empty()) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sub_indent);
                    std::string sug_lbl = "Simplify '" + sugs[0].folder_rel + "' (" + std::to_string(sugs[0].replaced_files.size()) + " files -> 1 rule)...";
                    if (Theme::Button(sug_lbl.c_str(), "Replace explicit file list with a folder glob rule", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_SimplifyTargetName = t.name;
                        s_SimplifySug = sugs[0];
                        s_ShowSimplifyModal = true;
                    }
                }
            }

            // 4. Target's folder/file tree
            auto tree_it = s_TargetTrees.find(t.name);
            if (tree_it != s_TargetTrees.end() && !tree_it->second.root.children.empty()) {
                std::vector<TreeRow> tgt_rows;
                std::string key_prefix = t.name + "::";
                std::vector<size_t> visible_children;
                for (size_t i = 0; i < tree_it->second.root.children.size(); ++i) {
                    if (s_DebouncedFilterText.empty() || NodeMatchesFilter(tree_it->second.root.children[i], s_DebouncedFilterText)) {
                        visible_children.push_back(i);
                    }
                }
                for (size_t i = 0; i < visible_children.size(); ++i) {
                    FlattenBuildTree(
                        tree_it->second.root.children[visible_children[i]],
                        1,
                        0,
                        i + 1 == visible_children.size(),
                        s_DebouncedFilterText,
                        key_prefix,
                        &t,
                        project.GetConfiguration(),
                        tgt_rows
                    );
                }

                std::vector<std::string> remove_batch;
                std::string remove_single;

                for (auto& r : tgt_rows) {
                    BuildTreeNode* node = reinterpret_cast<BuildTreeNode*>(r.user_data);
                    TreeRowResult res = RenderTreeRow(r, s_SelectedBuildRow, true, false);
                    if (res.selected_changed) s_SelectedBuildRow = r.id_str;

                    if (ImGui::BeginPopupContextItem()) {
                        if (r.type == TreeRowType::Folder && !r.rel_path.empty()) {
                            if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this folder...")) {
                                OpenBuildSettingsForOverride(t.name, r.rel_path, "");
                            }
                            if (node && ImGui::MenuItem("Remove folder files from target")) {
                                remove_batch = node->all_files;
                            }
                        } else if (r.type == TreeRowType::SourceFile) {
                            if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this file...")) {
                                OpenBuildSettingsForOverride(t.name, "", r.rel_path);
                            }
                            if (ImGui::MenuItem("Remove from target")) {
                                remove_single = r.rel_path;
                            }
                        }
                        ImGui::EndPopup();
                    }

                    if (res.checkbox_clicked) {
                        if (r.type == TreeRowType::Folder && node) {
                            bool en = (node->enabled_files != node->total_files);
                            project.SetTargetFolderEnabled(t.name, r.rel_path, en);
                        } else if (r.type == TreeRowType::SourceFile) {
                            project.SetTargetFileEnabled(t.name, r.rel_path, r.is_disabled);
                        }
                    }

                    if ((res.toggle_expanded || res.double_clicked) && r.type == TreeRowType::Folder) {
                        std::string ckey = key_prefix + r.rel_path;
                        if (s_CollapsedFolders.find(ckey) != s_CollapsedFolders.end()) s_CollapsedFolders.erase(ckey);
                        else s_CollapsedFolders.insert(ckey);
                    }
                    if (res.remove_clicked) {
                        if (r.type == TreeRowType::Folder && node) remove_batch = node->all_files;
                        else if (r.type == TreeRowType::SourceFile) remove_single = r.rel_path;
                    }

                    if (res.tooltip_hovered) {
                        std::string ov_suffix = (r.has_override && !r.override_tooltip.empty()) ? ("\n\n" + r.override_tooltip) : "";
                        if (r.type == TreeRowType::Folder) {
                            if (!ov_suffix.empty()) {
                                std::string msg = "Folder: " + r.rel_path + ov_suffix;
                                Theme::Tooltip(msg.c_str());
                            }
                        } else if (r.type == TreeRowType::SourceFile) {
                            auto mt_it = multi_target_files.find(r.rel_path);
                            if (mt_it != multi_target_files.end()) {
                                std::string msg = "Warning: compiled in " + std::to_string(mt_it->second.size()) + " targets (";
                                for (size_t mi = 0; mi < mt_it->second.size(); ++mi) {
                                    if (mi > 0) msg += ", ";
                                    msg += mt_it->second[mi];
                                }
                                msg += ")\n" + r.rel_path + ov_suffix;
                                Theme::Tooltip(msg.c_str());
                            } else {
                                std::string msg = r.rel_path + ov_suffix;
                                Theme::Tooltip(msg.c_str());
                            }
                        }
                    }
                }

                if (!remove_single.empty()) project.RemoveFileFromTarget(t.name, remove_single);
                if (!remove_batch.empty()) project.RemoveFilesFromTarget(t.name, remove_batch);
            }
        }

        ImGui::PopID();
    }

    ImGui::PopStyleVar();
}

void RenderBuildListPanel(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    bool embedded
) {
    static char s_BuildFilter[128] = "";
    const bool is_v2_ui = project.IsMultiTargetOrRulesUI();

    if (Theme::PaneBegin("BuildListCard", Theme::Col::BgBuildList, Theme::Col::Border, ImVec2(0, 0), ImVec2(0, 0), embedded)) {
        const bool is_scanning = project.IsScanning();
        const auto& files = project.GetFiles();
        const auto& missing_files = project.GetMissingFiles();
        const bool has_missing = !missing_files.empty();
        const bool is_large_build = (files.size() > 500);

        if (files.size() > s_LastKnownFilesCount) {
            size_t added = files.size() - s_LastKnownFilesCount;
            s_AddedFeedbackText = "+ Added " + std::to_string(added) + (added == 1 ? " file" : " files");
            s_AddedFeedbackExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        s_LastKnownFilesCount = files.size();

        bool show_added_feedback = (std::chrono::steady_clock::now() < s_AddedFeedbackExpiry);

        const float pane_w = ImGui::GetContentRegionAvail().x;
        const bool compact_header = (pane_w < 275.0f * Theme::DpiScale);

        std::string badge_str;
        ImVec4 badge_color = Theme::Col::Text;
        const size_t enabled_count = project.GetEnabledFilesCount();

        if (is_scanning) {
            badge_str = std::string(ICON_FA_SPINNER) + " SCANNING...";
            badge_color = Theme::Col::Warning;
        } else if (show_added_feedback) {
            badge_str = s_AddedFeedbackText;
            badge_color = Theme::Col::Success;
        } else if (has_missing) {
            badge_str = compact_header
                ? (std::to_string(missing_files.size()) + " missing")
                : (std::to_string(files.size()) + " files (" + std::to_string(missing_files.size()) + " missing)");
            badge_color = Theme::Col::Error;
        } else if (is_large_build) {
            badge_str = std::string(ICON_FA_TRIANGLE_EXCLAMATION) + " " + std::to_string(enabled_count) + "/" + std::to_string(files.size()) + (compact_header ? "" : " files");
            badge_color = Theme::Col::Warning;
        } else if (is_v2_ui) {
            badge_str = std::to_string(project.GetTargets().size()) + " targets · " + std::to_string(files.size());
            badge_color = Theme::Col::TextDim;
        } else {
            badge_str = compact_header
                ? (std::to_string(enabled_count) + "/" + std::to_string(files.size()))
                : (std::to_string(enabled_count) + " / " + std::to_string(files.size()) + " compiled");
            badge_color = (enabled_count > 0) ? Theme::Col::Text : Theme::Col::TextDim;
        }

        const float menu_btn_w = Theme::Btn::IconW;
        const float clear_btn_w = is_v2_ui ? menu_btn_w : (compact_header ? Theme::Btn::IconW : Theme::Btn::ClearW);
        const float badge_w = ImGui::CalcTextSize(badge_str.c_str()).x;
        const float total_right_w = clear_btn_w + (is_v2_ui ? 0.0f : (Theme::Sp::S2 + menu_btn_w)) + Theme::Sp::S2 + badge_w;

        Theme::BeginPaneHeader(is_v2_ui ? "TARGETS" : "BUILD LIST", total_right_w, embedded);

        if (!is_v2_ui) {
            const bool can_clear = !files.empty() && !is_scanning;
            if (!can_clear) ImGui::BeginDisabled(true);
            if (compact_header) {
                if (Theme::IconButton("##ClearAllBtn", ICON_FA_TRASH, "Clear all: remove all files from the build list", false, ImVec2(clear_btn_w, Theme::Btn::SmallH))) {
                    if (files.size() > 20) s_ShowClearAllModal = true;
                    else project.ClearFiles();
                }
            } else {
                if (Theme::Button("Clear all", "Remove all files from the build list", false, ImVec2(clear_btn_w, Theme::Btn::SmallH))) {
                    if (files.size() > 20) s_ShowClearAllModal = true;
                    else project.ClearFiles();
                }
            }
            if (!can_clear) ImGui::EndDisabled();
            ImGui::SameLine(0.0f, Theme::Sp::S2);
        }

        if (!project.HasProject()) ImGui::BeginDisabled(true);
        if (Theme::IconButton("##TargetsMenuBtn", "+", "New Target (Ctrl+Shift+T) / Add Rule", false, ImVec2(menu_btn_w, Theme::Btn::SmallH))) {
            ImGui::OpenPopup("TargetsActionMenu");
        }
        if (!project.HasProject()) ImGui::EndDisabled();

        if (ImGui::BeginPopup("TargetsActionMenu")) {
            if (ImGui::MenuItem("New Target... (Ctrl+Shift+T)")) {
                s_ShowNewTargetWizard = true;
            }
            if (const core::Target* pt = project.GetPrimaryTarget()) {
                std::string lbl = "Add source rule to '" + pt->name + "'...";
                if (ImGui::MenuItem(lbl.c_str())) {
                    core::SourceRule nr;
                    nr.include = "src/**/*.{c,cc,cpp,cxx}";
                    OpenRuleEditor(pt->name, -1, nr);
                }
            }
            if (!files.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem("Clear all files & rules")) {
                    if (files.size() > 20) s_ShowClearAllModal = true;
                    else project.ClearFiles();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(badge_color, "%s", badge_str.c_str());

        Theme::EndPaneHeader();

        // Filter box
        const float content_w = ImGui::GetContentRegionAvail().x - Theme::Sp::S3;
        bool has_filter = (s_BuildFilter[0] != '\0');
        const float clear_btn_size = ImGui::GetFrameHeight();
        const float filter_w = has_filter ? (content_w - clear_btn_size - Theme::Sp::S1) : content_w;

        ImGui::SetNextItemWidth(filter_w);
        if (s_FocusFilterRequested) {
            ImGui::SetKeyboardFocusHere();
            s_FocusFilterRequested = false;
        }
        if (ImGui::InputTextWithHint("##BuildFilter", is_v2_ui ? ICON_FA_SEARCH " Filter targets & files..." : ICON_FA_SEARCH " Filter build queue...", s_BuildFilter, sizeof(s_BuildFilter))) {
            s_FilterDebounceActive = true;
            s_LastFilterEditTime = std::chrono::steady_clock::now();
        }

        if (has_filter) {
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (Theme::IconButton("##ClearBuildFilter", ICON_FA_XMARK, "Clear filter (Esc)", false, ImVec2(clear_btn_size, clear_btn_size))) {
                s_BuildFilter[0] = '\0';
                s_DebouncedFilterText = "";
                s_FilterDebounceActive = false;
                s_RowsDirty = true;
            }
        }
        if (has_filter && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_BuildFilter[0] = '\0';
            s_DebouncedFilterText = "";
            s_FilterDebounceActive = false;
            s_RowsDirty = true;
        }

        ImGui::Spacing();

        if (s_FilterDebounceActive) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_LastFilterEditTime).count() >= 150) {
                s_FilterDebounceActive = false;
                std::string new_filter = ToLower(s_BuildFilter);
                if (new_filter != s_DebouncedFilterText) {
                    s_DebouncedFilterText = new_filter;
                    s_RowsDirty = true;
                }
            }
        }

        if (!is_v2_ui) {
            if (project.Revision() != s_LastProjectRevision) {
                s_LastProjectRevision = project.Revision();
                s_BuildTreeModel.Rebuild(project);
                s_RowsDirty = true;
            }
            if (s_ExpandRevision != s_LastRenderedExpandRev) {
                s_LastRenderedExpandRev = s_ExpandRevision;
                s_RowsDirty = true;
            }
            if (s_RowsDirty) {
                s_CachedFlatRows.clear();
                if (!s_BuildTreeModel.root.children.empty()) {
                    std::vector<size_t> visible_root_children;
                    for (size_t i = 0; i < s_BuildTreeModel.root.children.size(); ++i) {
                        if (s_DebouncedFilterText.empty() || NodeMatchesFilter(s_BuildTreeModel.root.children[i], s_DebouncedFilterText)) {
                            visible_root_children.push_back(i);
                        }
                    }
                    const size_t count = visible_root_children.size();
                    for (size_t i = 0; i < count; ++i) {
                        FlattenBuildTree(
                            s_BuildTreeModel.root.children[visible_root_children[i]],
                            0, 0, i == count - 1, s_DebouncedFilterText, "",
                            project.GetPrimaryTarget(), project.GetConfiguration(), s_CachedFlatRows
                        );
                    }
                }
                s_CachedSelectedBuildIndex = -1;
                if (!s_SelectedBuildRow.empty()) {
                    for (int i = 0; i < (int)s_CachedFlatRows.size(); ++i) {
                        if (s_CachedFlatRows[i].id_str == s_SelectedBuildRow) {
                            s_CachedSelectedBuildIndex = i;
                            break;
                        }
                    }
                }
                s_RowsDirty = false;
            }
        }

        const float scroll_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - Theme::Sp::S1);
        if (ImGui::BeginChild("BuildListInnerScroll", ImVec2(0, scroll_h), false)) {
            if (is_scanning) {
                const float pad_x = Theme::Sp::S4;
                ImGui::SetCursorPos(ImVec2(pad_x, std::max(Theme::Sp::S4, ImGui::GetContentRegionAvail().y * 0.28f)));
                ImGui::TextColored(Theme::Col::Warning, "Scanning project directory...");
            } else if (!project.HasProject()) {
                const float pad_x = Theme::Sp::S4;
                ImGui::SetCursorPos(ImVec2(pad_x, std::max(Theme::Sp::S4, ImGui::GetContentRegionAvail().y * 0.30f)));
                ImGui::TextColored(Theme::Col::Text, "No project open.");
            } else if (is_v2_ui) {
                RenderTargetsTree(project, compiler_manager, build_runner, build_start_error, output_lines);
            } else if (files.empty()) {
                const float pad_x = Theme::Sp::S4;
                const float avail_w = ImGui::GetContentRegionAvail().x;
                const float box_w = std::max(60.0f, avail_w - pad_x * 2.0f);
                ImGui::SetCursorPos(ImVec2(pad_x, std::max(Theme::Sp::S4, ImGui::GetContentRegionAvail().y * 0.28f)));
                ImGui::PushTextWrapPos(pad_x + box_w);
                ImGui::TextColored(Theme::Col::Text, "Build queue is empty.");
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                ImGui::TextColored(Theme::Col::TextDim, "Add source files (.cpp, .c) from the Project Explorer, or queue all discovered sources:");
                ImGui::PopTextWrapPos();

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::SetCursorPosX(pad_x);
                if (Theme::Button(ICON_FA_FOLDER_OPEN " Add all sources", "Scan and add all C/C++ files from project folder into build list", false, ImVec2(std::min(box_w, 168.0f * Theme::DpiScale), Theme::Btn::DefaultH))) {
                    std::vector<std::string> all_src;
                    CollectAllSources(project.GetRootPath(), all_src);
                    if (!all_src.empty()) {
                        project.AddFiles(all_src);
                    }
                }
            } else if (!s_DebouncedFilterText.empty() && s_CachedFlatRows.empty()) {
                const float pad_x = Theme::Sp::S4;
                ImGui::SetCursorPos(ImVec2(pad_x, Theme::Sp::S4));
                ImGui::TextColored(Theme::Col::TextDim, "No files match filter: '%s'", s_BuildFilter);
            } else {
                // v1 single-target clipper view
                std::vector<std::string> files_to_remove;
                std::string single_file_to_remove;

                ImGuiStyle& style = ImGui::GetStyle();
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
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
                        BuildTreeNode* node = reinterpret_cast<BuildTreeNode*>(r.user_data);
                        TreeRowResult res = RenderTreeRow(r, s_SelectedBuildRow, true, false);
                        if (ImGui::BeginPopupContextItem()) {
                            const core::Target* pt = project.GetPrimaryTarget();
                            std::string tname = pt ? pt->name : "";
                            if (r.type == TreeRowType::Folder && !r.rel_path.empty()) {
                                if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this folder...")) {
                                    OpenBuildSettingsForOverride(tname, r.rel_path, "");
                                }
                                if (node && ImGui::MenuItem("Remove folder files")) {
                                    files_to_remove = node->all_files;
                                }
                            } else if (r.type == TreeRowType::SourceFile) {
                                if (ImGui::MenuItem(ICON_FA_FLAG "  Set flags for this file...")) {
                                    OpenBuildSettingsForOverride(tname, "", r.rel_path);
                                }
                                if (ImGui::MenuItem("Remove from build list")) {
                                    single_file_to_remove = r.rel_path;
                                }
                            }
                            ImGui::EndPopup();
                        }
                        if (res.tooltip_hovered && r.has_override && !r.override_tooltip.empty()) {
                            std::string tip = r.rel_path + "\n\n" + r.override_tooltip;
                            Theme::Tooltip(tip.c_str());
                        }
                        if (res.selected_changed) {
                            s_SelectedBuildRow = r.id_str;
                            s_CachedSelectedBuildIndex = i;
                        }
                        if (res.checkbox_clicked) {
                            if (r.type == TreeRowType::Folder && node) {
                                project.SetFolderEnabled(r.rel_path, node->enabled_files != node->total_files);
                            } else if (r.type == TreeRowType::SourceFile) {
                                project.ToggleFileEnabled(r.rel_path);
                            }
                        }
                        if ((res.toggle_expanded || res.double_clicked) && r.type == TreeRowType::Folder) {
                            if (s_CollapsedFolders.find(r.rel_path) != s_CollapsedFolders.end()) s_CollapsedFolders.erase(r.rel_path);
                            else s_CollapsedFolders.insert(r.rel_path);
                            s_ExpandRevision++;
                            s_RowsDirty = true;
                        } else if (res.double_clicked && r.type == TreeRowType::SourceFile) {
                            single_file_to_remove = r.rel_path;
                        }
                        if (res.remove_clicked) {
                            if (r.type == TreeRowType::Folder && node) files_to_remove = node->all_files;
                            else if (r.type == TreeRowType::SourceFile) single_file_to_remove = r.rel_path;
                        }
                    }
                }
                clipper.End();
                ImGui::PopStyleVar();

                if (!single_file_to_remove.empty()) project.RemoveFile(single_file_to_remove);
                if (!files_to_remove.empty()) project.RemoveFiles(files_to_remove);
            }
        }
        ImGui::EndChild();

        // ---------------------------------------------------------------------
        // Modals: Clear All, New Target Wizard, Rule Editor, Rename, Delete, Drop Folder, Simplify
        // ---------------------------------------------------------------------
        if (s_ShowClearAllModal) ImGui::OpenPopup("Confirm Clear All##Modal");
        bool clr_close_x = false;
        if (Theme::BeginModal("Confirm Clear All##Modal", "Clear Build List", ICON_FA_TRASH, Theme::Col::Warning, 400.0f, &clr_close_x)) {
            ImGui::TextColored(Theme::Col::Text, "Remove all %zu files from the build list?", files.size());
            ImGui::Spacing();
            const float dpi = Theme::DpiScale;
            if (Theme::Button("Cancel", "Keep files", false, ImVec2(78.0f * dpi, Theme::Btn::DefaultH)) || clr_close_x) {
                s_ShowClearAllModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::Button("Clear all", "Confirm", true, ImVec2(92.0f * dpi, Theme::Btn::DefaultH))) {
                project.ClearFiles();
                s_ShowClearAllModal = false;
                ImGui::CloseCurrentPopup();
            }
            Theme::EndModal();
        }

        // New Target Wizard Modal (Ctrl+Shift+T)
        if (s_ShowNewTargetWizard) ImGui::OpenPopup("New Target Wizard##Modal");
        bool wiz_close_x = false;
        if (Theme::BeginModal("New Target Wizard##Modal", "New Target", ICON_FA_CODE, Theme::Col::Accent, 480.0f, &wiz_close_x)) {
            static int s_WizTemplate = 0; // 0=exe, 1=static, 2=shared, 3=interface, 4=from folder
            static char s_WizName[128] = "my_target";
            static char s_WizFolder[256] = "";
            static std::string s_WizError = "";
            static std::unordered_set<std::string> s_WizDeps;

            const char* templates[] = { "Executable", "Static library", "Shared library", "Header-only (interface)", "From folder..." };
            ImGui::TextColored(Theme::Col::Text, "Template:");
            ImGui::SameLine(140.0f * Theme::DpiScale);
            ImGui::SetNextItemWidth(240.0f * Theme::DpiScale);
            if (ImGui::Combo("##WizTemplate", &s_WizTemplate, templates, 5)) {
                if (s_WizTemplate == 4) {
                    nfdchar_t* out_dir = nullptr;
                    std::string root_s = project.GetRootPath().generic_u8string();
                    if (NFD_PickFolder(&out_dir, root_s.empty() ? nullptr : root_s.c_str()) == NFD_OKAY && out_dir) {
                        std::filesystem::path fp = std::filesystem::u8path(out_dir);
                        std::error_code ec;
                        std::filesystem::path rel = std::filesystem::relative(fp, project.GetRootPath(), ec);
                        std::string rel_s = (!ec) ? NormalizePath(rel.generic_u8string()) : NormalizePath(fp.generic_u8string());
                        std::strncpy(s_WizFolder, rel_s.c_str(), sizeof(s_WizFolder) - 1);
                        std::string fname = fp.filename().generic_u8string();
                        if (!fname.empty()) std::strncpy(s_WizName, fname.c_str(), sizeof(s_WizName) - 1);
                        NFD_FreePath(out_dir);
                    }
                }
            }

            ImGui::TextColored(Theme::Col::Text, "Target Name:");
            ImGui::SameLine(140.0f * Theme::DpiScale);
            ImGui::SetNextItemWidth(240.0f * Theme::DpiScale);
            ImGui::InputText("##WizName", s_WizName, sizeof(s_WizName));

            if (s_WizTemplate == 4) {
                ImGui::TextColored(Theme::Col::Text, "Folder Rule:");
                ImGui::SameLine(140.0f * Theme::DpiScale);
                ImGui::SetNextItemWidth(240.0f * Theme::DpiScale);
                ImGui::InputText("##WizFolder", s_WizFolder, sizeof(s_WizFolder));
            }

            if (!project.GetTargets().empty()) {
                ImGui::Spacing();
                ImGui::TextColored(Theme::Col::TextDim, "Dependencies:");
                for (const auto& existing : project.GetTargets()) {
                    bool checked = s_WizDeps.find(existing.name) != s_WizDeps.end();
                    if (ImGui::Checkbox(existing.name.c_str(), &checked)) {
                        if (checked) s_WizDeps.insert(existing.name);
                        else s_WizDeps.erase(existing.name);
                    }
                }
            }

            if (!s_WizError.empty()) {
                ImGui::TextColored(Theme::Col::Error, "%s", s_WizError.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (Theme::Button("Cancel", nullptr, false, ImVec2(80.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || wiz_close_x) {
                s_WizError.clear();
                s_ShowNewTargetWizard = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::SuccessButton("Create Target", nullptr, ImVec2(120.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                core::Target nt;
                nt.name = s_WizName;
                if (s_WizTemplate == 1) nt.kind = core::TargetKind::Static;
                else if (s_WizTemplate == 2) nt.kind = core::TargetKind::Shared;
                else if (s_WizTemplate == 3) nt.kind = core::TargetKind::Interface;
                else nt.kind = core::TargetKind::Executable;

                if (s_WizTemplate == 4 && s_WizFolder[0] != '\0') {
                    core::SourceRule r;
                    r.include = std::string(s_WizFolder) + "/**/*.{c,cc,cpp,cxx}";
                    nt.sources.push_back(std::move(r));
                }
                for (const auto& d : s_WizDeps) nt.deps.push_back(d);

                if (project.AddTarget(nt, s_WizError)) {
                    s_WizDeps.clear();
                    s_WizError.clear();
                    s_ShowNewTargetWizard = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            Theme::EndModal();
        }

        // Rule Editor Modal with 150ms debounced live preview (V2-1)
        if (s_ShowRuleEditor) ImGui::OpenPopup("Source Rule Editor##Modal");
        bool rule_close_x = false;
        if (Theme::BeginModal("Source Rule Editor##Modal", "Edit Source Rule", ICON_FA_COPY, Theme::Col::Accent, 520.0f, &rule_close_x)) {
            ImGui::TextColored(Theme::Col::Text, "Include Glob:");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##RuleInc", s_RuleIncludeBuf, sizeof(s_RuleIncludeBuf))) {
                s_RulePreviewDirty = true;
                s_RuleLastEditTime = std::chrono::steady_clock::now();
            }

            ImGui::TextColored(Theme::Col::Text, "Condition (optional):");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##RuleCond", "e.g. windows && !clang", s_RuleCondBuf, sizeof(s_RuleCondBuf));

            ImGui::TextColored(Theme::Col::TextDim, "Exclude Globs:");
            int del_ex = -1;
            for (size_t ei = 0; ei < s_RuleExcludeList.size(); ++ei) {
                ImGui::PushID(static_cast<int>(ei));
                char ebuf[256];
                std::strncpy(ebuf, s_RuleExcludeList[ei].c_str(), sizeof(ebuf) - 1);
                ebuf[sizeof(ebuf) - 1] = '\0';
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f * Theme::DpiScale);
                if (ImGui::InputText("##ex", ebuf, sizeof(ebuf))) {
                    s_RuleExcludeList[ei] = ebuf;
                    s_RulePreviewDirty = true;
                    s_RuleLastEditTime = std::chrono::steady_clock::now();
                }
                ImGui::SameLine();
                if (Theme::DangerIconButton("##dex", ICON_FA_XMARK, "Remove exclude")) {
                    del_ex = static_cast<int>(ei);
                }
                ImGui::PopID();
            }
            if (del_ex >= 0) {
                s_RuleExcludeList.erase(s_RuleExcludeList.begin() + del_ex);
                s_RulePreviewDirty = true;
                s_RuleLastEditTime = std::chrono::steady_clock::now();
            }
            if (Theme::Button("+ Add Exclude", nullptr, false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                s_RuleExcludeList.push_back("");
            }

            if (s_RulePreviewDirty) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s_RuleLastEditTime
                ).count();
                if (elapsed >= 150) {
                    s_RulePreviewDirty = false;
                    core::SourceRule tmp;
                    tmp.include = s_RuleIncludeBuf;
                    tmp.exclude = s_RuleExcludeList;
                    std::vector<std::string> tgt_ex;
                    if (const core::Target* pt = project.FindTarget(s_RuleEditTarget)) {
                        tgt_ex = pt->exclude;
                    }
                    s_RulePreviewCache = core::PreviewRule(project.GetRootPath(), tmp, tgt_ex, project.GetDirectoryCache(), 200);
                }
            }

            ImGui::Spacing();
            ImGui::TextColored(Theme::Col::TextDim, "Live Preview (%zu matched files, showing up to 200):", s_RulePreviewCache.total_count);
            ImGui::BeginChild("RulePreviewList", ImVec2(0.0f, 140.0f * Theme::DpiScale), true);
            for (const auto& pf : s_RulePreviewCache.preview_files) {
                ImGui::TextUnformatted(pf.c_str());
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (Theme::Button("Cancel", nullptr, false, ImVec2(80.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || rule_close_x) {
                s_ShowRuleEditor = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::SuccessButton("Save Rule", nullptr, ImVec2(100.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                if (const core::Target* cur_t = project.FindTarget(s_RuleEditTarget)) {
                    core::Target updated = *cur_t;
                    core::SourceRule r;
                    r.include = s_RuleIncludeBuf;
                    r.condition = s_RuleCondBuf;
                    for (const auto& ex : s_RuleExcludeList) {
                        if (!ex.empty()) r.exclude.push_back(ex);
                    }
                    if (!r.include.empty()) {
                        if (s_RuleEditIndex >= 0 && s_RuleEditIndex < static_cast<int>(updated.sources.size())) {
                            updated.sources[static_cast<size_t>(s_RuleEditIndex)] = std::move(r);
                        } else {
                            updated.sources.push_back(std::move(r));
                        }
                        project.UpdateTarget(updated);
                    }
                }
                s_ShowRuleEditor = false;
                ImGui::CloseCurrentPopup();
            }
            Theme::EndModal();
        }

        // Rename Target Modal
        if (s_ShowRenameTargetModal) ImGui::OpenPopup("Rename Target##Modal");
        bool ren_close_x = false;
        if (Theme::BeginModal("Rename Target##Modal", "Rename Target", ICON_FA_CODE, Theme::Col::Accent, 380.0f, &ren_close_x)) {
            static std::string s_RenErr;
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##RenTgt", s_RenameNewBuf, sizeof(s_RenameNewBuf));
            if (!s_RenErr.empty()) ImGui::TextColored(Theme::Col::Error, "%s", s_RenErr.c_str());
            ImGui::Spacing();
            if (Theme::Button("Cancel", nullptr, false, ImVec2(80.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || ren_close_x) {
                s_RenErr.clear();
                s_ShowRenameTargetModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::SuccessButton("Rename", nullptr, ImVec2(90.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                if (project.RenameTarget(s_RenameOldName, s_RenameNewBuf, s_RenErr)) {
                    s_RenErr.clear();
                    s_ShowRenameTargetModal = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            Theme::EndModal();
        }

        // Delete Target Confirmation Modal (shows dependents)
        if (s_ShowDeleteTargetModal) ImGui::OpenPopup("Delete Target##Modal");
        bool del_close_x = false;
        if (Theme::BeginModal("Delete Target##Modal", "Delete Target", ICON_FA_TRASH, Theme::Col::Warning, 420.0f, &del_close_x)) {
            ImGui::TextColored(Theme::Col::Text, "Delete target '%s'?", s_DeleteTargetName.c_str());
            std::vector<std::string> dependents;
            for (const auto& other : project.GetTargets()) {
                if (other.name == s_DeleteTargetName) continue;
                if (std::find(other.deps.begin(), other.deps.end(), s_DeleteTargetName) != other.deps.end() ||
                    std::find(other.publicDeps.begin(), other.publicDeps.end(), s_DeleteTargetName) != other.publicDeps.end()) {
                    dependents.push_back(other.name);
                }
            }
            if (!dependents.empty()) {
                std::string dep_list;
                for (size_t i = 0; i < dependents.size(); ++i) {
                    if (i > 0) dep_list += ", ";
                    dep_list += dependents[i];
                }
                ImGui::TextColored(Theme::Col::Warning, "Note: these targets depend on it (references will be removed): %s", dep_list.c_str());
            }
            ImGui::Spacing();
            if (Theme::Button("Cancel", nullptr, false, ImVec2(80.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || del_close_x) {
                s_ShowDeleteTargetModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::Button("Delete", nullptr, true, ImVec2(90.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                std::string err;
                project.RemoveTarget(s_DeleteTargetName, err);
                s_ShowDeleteTargetModal = false;
                ImGui::CloseCurrentPopup();
            }
            Theme::EndModal();
        }

        // Drag-and-drop Folder Choice Modal
        if (s_ShowDropFolderModal) ImGui::OpenPopup("Add Folder to Target##Modal");
        bool drop_close_x = false;
        if (Theme::BeginModal("Add Folder to Target##Modal", "Add Folder to Target", ICON_FA_FOLDER_OPEN, Theme::Col::Accent, 420.0f, &drop_close_x)) {
            ImGui::TextColored(Theme::Col::Text, "Add '%s' to target '%s':", s_DropFolderRel.c_str(), s_DropTargetName.c_str());
            ImGui::Spacing();
            if (Theme::SuccessButton("Add as rule", nullptr, ImVec2(120.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                core::SourceRule r;
                r.include = s_DropFolderRel.empty() ? "**/*.{c,cc,cpp,cxx}" : (s_DropFolderRel + "/**/*.{c,cc,cpp,cxx}");
                project.AddSourceRuleToTarget(s_DropTargetName, r);
                s_ShowDropFolderModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::Button("Add files", nullptr, false, ImVec2(100.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                std::vector<std::string> found;
                CollectAllSources(project.ResolvePath(s_DropFolderRel), found);
                std::vector<std::string> rels;
                for (const auto& f : found) {
                    rels.push_back(s_DropFolderRel.empty() ? f : (s_DropFolderRel + "/" + f));
                }
                project.AddFilesToTarget(s_DropTargetName, rels);
                s_ShowDropFolderModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::Button("Cancel", nullptr, false, ImVec2(80.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || drop_close_x) {
                s_ShowDropFolderModal = false;
                ImGui::CloseCurrentPopup();
            }
            Theme::EndModal();
        }

        // Simplify Suggestion Preview Modal (V2-1)
        if (s_ShowSimplifyModal) ImGui::OpenPopup("Simplify Files to Rule##Modal");
        bool simp_close_x = false;
        if (Theme::BeginModal("Simplify Files to Rule##Modal", "Simplify Explicit Files", ICON_FA_COPY, Theme::Col::Accent, 480.0f, &simp_close_x)) {
            ImGui::TextColored(Theme::Col::Text, "Replace %zu explicit files with rule:", s_SimplifySug.replaced_files.size());
            ImGui::TextColored(Theme::Col::Accent, "%s", s_SimplifySug.suggested_include.c_str());
            ImGui::TextColored(Theme::Col::Success, "Before and after file sets are identical.");
            ImGui::Spacing();
            if (Theme::Button("Cancel", nullptr, false, ImVec2(80.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || simp_close_x) {
                s_ShowSimplifyModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Theme::SuccessButton("Apply Rule", nullptr, ImVec2(110.0f * Theme::DpiScale, Theme::Btn::DefaultH))) {
                if (const core::Target* cur_t = project.FindTarget(s_SimplifyTargetName)) {
                    core::Target updated = *cur_t;
                    std::unordered_set<std::string> rep(s_SimplifySug.replaced_files.begin(), s_SimplifySug.replaced_files.end());
                    updated.files.erase(
                        std::remove_if(updated.files.begin(), updated.files.end(), [&](const std::string& f) { return rep.find(f) != rep.end(); }),
                        updated.files.end()
                    );
                    core::SourceRule r;
                    r.include = s_SimplifySug.suggested_include;
                    updated.sources.push_back(std::move(r));
                    project.UpdateTarget(updated);
                }
                s_ShowSimplifyModal = false;
                ImGui::CloseCurrentPopup();
            }
            Theme::EndModal();
        }
    }
    Theme::PaneEnd();
}

} // namespace ui
