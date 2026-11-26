#include "build_settings_modal.h"
#include "settings_resolver.h"
#include "condition.h"
#include "build_steps.h"
#include "tool_registry.h"
#include "toolchain.h"
#include "msvc_env.h"
#include "compdb.h"
#include "fs_util.h"
#include "hints.h"
#include "pch.h"
#include "unity.h"
#include "compiler_cache.h"
#include "dep_log.h"
#include "scheduler.h"
#include "theme.h"
#include "icons.h"

#include <imgui.h>
#include <nfd.h>
#include <reproc++/reproc.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cmath>

namespace ui {

static bool s_ShowSettingsModal = false;
static bool s_ShowManageConfigsModal = false;
static bool s_ShowToolchainManagerModal = false;
static std::string s_SelectedTargetScope = ""; // "" = Project (Global), else target name
static std::string s_SelectedFolderScope = ""; // e.g. "src/simd"
static std::string s_SelectedFileScope = "";   // e.g. "src/hot/mathlib.cpp"
static std::string s_EffectiveFlagsFile = "";
static bool s_FocusStepsTab = false;
static bool s_FocusPchTab = false;
static bool s_FocusUnityTab = false;
static int s_SelectedStepIndex = 0;

void OpenBuildSettingsModal(const std::string& target_name) {
    s_ShowSettingsModal = true;
    s_SelectedTargetScope = target_name;
    s_SelectedFolderScope.clear();
    s_SelectedFileScope.clear();
    s_FocusStepsTab = false;
    s_FocusPchTab = false;
    s_FocusUnityTab = false;
}

void OpenBuildSettingsForOverride(
    const std::string& target_name,
    const std::string& folder_rel,
    const std::string& file_rel
) {
    s_ShowSettingsModal = true;
    s_SelectedTargetScope = target_name;
    s_SelectedFolderScope = folder_rel;
    s_SelectedFileScope = file_rel;
    s_FocusStepsTab = false;
    s_FocusPchTab = false;
    s_FocusUnityTab = false;
    if (!file_rel.empty()) {
        s_EffectiveFlagsFile = file_rel;
    }
}

void OpenBuildSettingsForSteps(
    const std::string& target_name,
    int step_index
) {
    s_ShowSettingsModal = true;
    s_SelectedTargetScope = target_name;
    s_SelectedFolderScope.clear();
    s_SelectedFileScope.clear();
    s_FocusStepsTab = true;
    s_FocusPchTab = false;
    s_FocusUnityTab = false;
    s_SelectedStepIndex = step_index;
}

void OpenBuildSettingsForPch(const std::string& target_name) {
    s_ShowSettingsModal = true;
    s_SelectedTargetScope = target_name;
    s_SelectedFolderScope.clear();
    s_SelectedFileScope.clear();
    s_FocusStepsTab = false;
    s_FocusPchTab = true;
    s_FocusUnityTab = false;
}

void OpenBuildSettingsForUnity(const std::string& target_name) {
    s_ShowSettingsModal = true;
    s_SelectedTargetScope = target_name;
    s_SelectedFolderScope.clear();
    s_SelectedFileScope.clear();
    s_FocusStepsTab = false;
    s_FocusPchTab = false;
    s_FocusUnityTab = true;
}

void OpenManageConfigsModal() {
    s_ShowManageConfigsModal = true;
}

void OpenToolchainManagerModal() {
    s_ShowToolchainManagerModal = true;
}

bool IsBuildSettingsModalOpen() {
    return s_ShowSettingsModal;
}

// Converts a chosen path to relative if it resides within the project root
static std::string MakeRelativeIfInside(const std::filesystem::path& chosen_path, const std::filesystem::path& root_path) {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(chosen_path, root_path, ec);
    if (!ec) {
        std::string rel_str = rel.generic_u8string();
        if (rel_str.rfind("..", 0) != 0) {
            return rel_str;
        }
    }
    return chosen_path.generic_u8string();
}

// Reusable EditableList widget (Spec §5.1.1):
// rows of InputText, a hover × button, an "Add" button, and optional "Browse..." button
static bool RenderEditableList(
    const char* label_id,
    std::vector<std::string>& items,
    bool is_path,
    bool is_folder,
    const core::Project& project,
    const char* add_hint = "Add item"
) {
    bool modified = false;
    const float row_h = Theme::Btn::DefaultH;
    const float del_btn_w = Theme::Btn::DelW;

    ImGui::PushID(label_id);

    int delete_idx = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        char buf[512];
        std::strncpy(buf, items[i].c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float input_w = avail_w - del_btn_w - Theme::Sp::S2;

        ImGui::SetNextItemWidth(input_w);
        if (ImGui::InputText("##item", buf, sizeof(buf))) {
            items[i] = buf;
            modified = true;
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);

        // Delete button
        if (Theme::DangerIconButton("##del", ICON_FA_XMARK, "Remove item", ImVec2(del_btn_w, row_h))) {
            delete_idx = static_cast<int>(i);
        }

        ImGui::PopID();
    }

    if (delete_idx >= 0 && delete_idx < static_cast<int>(items.size())) {
        items.erase(items.begin() + delete_idx);
        modified = true;
    }

    // Action row: + Add and optional Browse...
    ImGui::Spacing();
    if (Theme::Button("+ Add", add_hint, false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
        items.push_back("");
        modified = true;
    }

    if (is_path) {
        ImGui::SameLine(0.0f, Theme::Sp::S2);
        const char* browse_title = is_folder ? ICON_FA_FOLDER_OPEN " Browse Folder...##browse" : ICON_FA_FILE " Browse File...##browse";
        if (Theme::Button(browse_title, is_folder ? "Choose folder from disk" : "Choose file from disk", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
            nfdchar_t* out_path = nullptr;
            std::string root_str = project.GetRootPath().generic_u8string();
            nfdresult_t res = NFD_CANCEL;

            if (is_folder) {
                res = NFD_PickFolder(&out_path, root_str.empty() ? nullptr : root_str.c_str());
            } else {
                nfdu8filteritem_t dll_filter[1] = { { "Dynamic Libraries", "dll" } };
                if (std::strcmp(label_id, "CopyDllsList") == 0) {
                    res = NFD_OpenDialog(&out_path, dll_filter, 1, root_str.empty() ? nullptr : root_str.c_str());
                } else {
                    res = NFD_OpenDialog(&out_path, nullptr, 0, root_str.empty() ? nullptr : root_str.c_str());
                }
            }

            if (res == NFD_OKAY && out_path != nullptr) {
                std::filesystem::path p = std::filesystem::u8path(out_path);
                std::string rel_or_abs = MakeRelativeIfInside(p, project.GetRootPath());
                items.push_back(rel_or_abs);
                modified = true;
                NFD_FreePath(out_path);
            }
        }
    }

    ImGui::PopID();
    return modified;
}

static bool RenderKeyValueMap(
    const char* label_id,
    std::map<std::string, std::string>& kv_map,
    const char* key_hint,
    const char* val_hint
) {
    bool modified = false;
    const float dpi = Theme::DpiScale;
    const float row_h = Theme::Btn::DefaultH;
    const float del_btn_w = Theme::Btn::DelW;
    ImGui::PushID(label_id);

    std::vector<std::pair<std::string, std::string>> rows(kv_map.begin(), kv_map.end());
    int delete_idx = -1;
    bool rows_changed = false;

    for (size_t i = 0; i < rows.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float key_w = std::clamp(avail_w * 0.34f, 80.0f * dpi, 180.0f * dpi);
        const float val_w = std::max(80.0f * dpi, avail_w - key_w - del_btn_w - Theme::Sp::S2 * 2.0f);

        char kbuf[256];
        std::strncpy(kbuf, rows[i].first.c_str(), sizeof(kbuf) - 1);
        kbuf[sizeof(kbuf) - 1] = '\0';
        ImGui::SetNextItemWidth(key_w);
        if (ImGui::InputTextWithHint("##k", key_hint, kbuf, sizeof(kbuf))) {
            rows[i].first = kbuf;
            rows_changed = true;
        }
        ImGui::SameLine(0.0f, Theme::Sp::S2);

        char vbuf[512];
        std::strncpy(vbuf, rows[i].second.c_str(), sizeof(vbuf) - 1);
        vbuf[sizeof(vbuf) - 1] = '\0';
        ImGui::SetNextItemWidth(val_w);
        if (ImGui::InputTextWithHint("##v", val_hint, vbuf, sizeof(vbuf))) {
            rows[i].second = vbuf;
            rows_changed = true;
        }
        ImGui::SameLine(0.0f, Theme::Sp::S2);

        if (Theme::DangerIconButton("##del", ICON_FA_XMARK, "Remove entry", ImVec2(del_btn_w, row_h))) {
            delete_idx = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    if (delete_idx >= 0 && delete_idx < static_cast<int>(rows.size())) {
        rows.erase(rows.begin() + delete_idx);
        rows_changed = true;
    }

    static std::map<std::string, std::string> s_NewKeys;
    static std::map<std::string, std::string> s_NewVals;
    std::string id_str(label_id);
    char nk[256];
    char nv[512];
    std::strncpy(nk, s_NewKeys[id_str].c_str(), sizeof(nk) - 1);
    nk[sizeof(nk) - 1] = '\0';
    std::strncpy(nv, s_NewVals[id_str].c_str(), sizeof(nv) - 1);
    nv[sizeof(nv) - 1] = '\0';

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float add_w = 72.0f * dpi;
    const float key_w = std::clamp(avail_w * 0.34f, 80.0f * dpi, 180.0f * dpi);
    const float val_w = std::max(80.0f * dpi, avail_w - key_w - add_w - Theme::Sp::S2 * 2.0f);
    ImGui::SetNextItemWidth(key_w);
    if (ImGui::InputTextWithHint("##newk", key_hint, nk, sizeof(nk))) s_NewKeys[id_str] = nk;
    ImGui::SameLine(0.0f, Theme::Sp::S2);
    ImGui::SetNextItemWidth(val_w);
    if (ImGui::InputTextWithHint("##newv", val_hint, nv, sizeof(nv))) s_NewVals[id_str] = nv;
    ImGui::SameLine(0.0f, Theme::Sp::S2);
    if (Theme::Button("+ Add##addkv", "Add environment variable", false, ImVec2(add_w, row_h))) {
        if (!s_NewKeys[id_str].empty()) {
            rows.emplace_back(s_NewKeys[id_str], s_NewVals[id_str]);
            s_NewKeys[id_str].clear();
            s_NewVals[id_str].clear();
            rows_changed = true;
        }
    }

    if (rows_changed) {
        kv_map.clear();
        for (const auto& kv : rows) {
            if (!kv.first.empty()) kv_map[kv.first] = kv.second;
        }
        modified = true;
    }

    ImGui::PopID();
    return modified;
}

// Editable library list supporting optional V2-5 "if" condition per entry
static bool RenderConditionalLibsList(
    const char* label_id,
    std::vector<std::string>& libs,
    std::map<std::string, std::string>& lib_conds,
    const core::Project& project,
    const char* add_hint = "Add library"
) {
    bool modified = false;
    const float dpi = Theme::DpiScale;
    const float row_h = Theme::Btn::DefaultH;
    const float del_btn_w = Theme::Btn::DelW;
    const float cond_w = 155.0f * dpi;
    const core::ConditionContext cond_ctx = core::BuildConditionContext(project);

    ImGui::PushID(label_id);
    int delete_idx = -1;

    for (size_t i = 0; i < libs.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        std::string old_name = libs[i];
        std::string cur_cond = lib_conds.count(old_name) ? lib_conds[old_name] : "";
        core::ConditionEvalResult ev = core::EvaluateCondition(cur_cond, cond_ctx);

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float name_w = std::max(100.0f * dpi, avail_w - cond_w - del_btn_w - Theme::Sp::S2 * 2);

        char nbuf[256];
        std::strncpy(nbuf, old_name.c_str(), sizeof(nbuf) - 1);
        nbuf[sizeof(nbuf) - 1] = '\0';

        if (!ev.active) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Col::Inactive);
        }
        ImGui::SetNextItemWidth(name_w);
        if (ImGui::InputText("##libname", nbuf, sizeof(nbuf))) {
            std::string new_name = nbuf;
            if (!cur_cond.empty()) {
                lib_conds.erase(old_name);
                if (!new_name.empty()) lib_conds[new_name] = cur_cond;
            }
            libs[i] = new_name;
            modified = true;
        }
        if (!ev.active) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                Theme::Tooltip(ev.reason.c_str());
            }
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);

        char cbuf[256];
        std::strncpy(cbuf, cur_cond.c_str(), sizeof(cbuf) - 1);
        cbuf[sizeof(cbuf) - 1] = '\0';
        ImGui::SetNextItemWidth(cond_w);
        if (!ev.valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Col::Error);
        }
        if (ImGui::InputTextWithHint("##libcond", "if (optional)", cbuf, sizeof(cbuf))) {
            if (!libs[i].empty()) {
                if (cbuf[0] == '\0') lib_conds.erase(libs[i]);
                else lib_conds[libs[i]] = cbuf;
            }
            modified = true;
        }
        if (!ev.valid) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                Theme::Tooltip(ev.error.c_str());
            }
        } else if (!ev.active && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            Theme::Tooltip(ev.reason.c_str());
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);
        if (Theme::DangerIconButton("##del", ICON_FA_XMARK, "Remove library", ImVec2(del_btn_w, row_h))) {
            delete_idx = static_cast<int>(i);
        }

        if (!ev.valid) {
            ImGui::TextColored(Theme::Col::Error, "  %s %s", ICON_FA_CIRCLE_EXCLAMATION, ev.error.c_str());
        }
        ImGui::PopID();
    }

    if (delete_idx >= 0 && delete_idx < static_cast<int>(libs.size())) {
        lib_conds.erase(libs[static_cast<size_t>(delete_idx)]);
        libs.erase(libs.begin() + delete_idx);
        modified = true;
    }

    ImGui::Spacing();
    if (Theme::Button("+ Add", add_hint, false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
        libs.push_back("");
        modified = true;
    }
    ImGui::PopID();
    return modified;
}

// Executes pkg-config --cflags --libs <pkg> and adds flags to project settings (Spec §5.1.2)
static bool RunPkgConfig(const std::string& package_name, core::Project& project, std::string& out_msg) {
    if (package_name.empty()) return false;

    reproc::process proc;
    reproc::options opts;
    opts.redirect.out.type = reproc::redirect::pipe;
    opts.redirect.err.type = reproc::redirect::pipe;

    std::vector<std::string> args = { "pkg-config", "--cflags", "--libs", package_name };
    std::error_code ec = proc.start(args, opts);
    if (ec) {
        out_msg = "Failed to run pkg-config: " + ec.message() + " (ensure pkg-config is on PATH)";
        return false;
    }

    std::string output_str;
    uint8_t buf[2048];
    while (true) {
        auto [bytes_read, read_ec] = proc.read(reproc::stream::out, buf, sizeof(buf));
        if (read_ec) break;
        if (bytes_read > 0) {
            output_str.append(reinterpret_cast<char*>(buf), bytes_read);
        }
    }
    auto [status, wait_ec] = proc.wait(reproc::infinite);
    if (status != 0) {
        out_msg = "pkg-config returned exit code " + std::to_string(status) + " for package '" + package_name + "'.";
        return false;
    }

    // Parse tokens
    auto tokens = core::CompilerManager::SplitFlags(output_str);
    auto inc_dirs = project.GetIncludeDirs();
    auto lib_dirs = project.GetLibDirs();
    auto libs = project.GetLibs();
    auto defines = project.GetDefines();

    int added_count = 0;
    for (const auto& tok : tokens) {
        if (tok.rfind("-I", 0) == 0 && tok.size() > 2) {
            std::string dir = tok.substr(2);
            if (std::find(inc_dirs.begin(), inc_dirs.end(), dir) == inc_dirs.end()) {
                inc_dirs.push_back(dir);
                added_count++;
            }
        } else if (tok.rfind("-L", 0) == 0 && tok.size() > 2) {
            std::string dir = tok.substr(2);
            if (std::find(lib_dirs.begin(), lib_dirs.end(), dir) == lib_dirs.end()) {
                lib_dirs.push_back(dir);
                added_count++;
            }
        } else if (tok.rfind("-l", 0) == 0 && tok.size() > 2) {
            std::string lib = tok.substr(2);
            if (std::find(libs.begin(), libs.end(), lib) == libs.end()) {
                libs.push_back(lib);
                added_count++;
            }
        } else if (tok.rfind("-D", 0) == 0 && tok.size() > 2) {
            std::string def = tok.substr(2);
            if (std::find(defines.begin(), defines.end(), def) == defines.end()) {
                defines.push_back(def);
                added_count++;
            }
        }
    }

    project.SetIncludeDirs(inc_dirs);
    project.SetLibDirs(lib_dirs);
    project.SetLibs(libs);
    project.SetDefines(defines);

    out_msg = "Imported " + std::to_string(added_count) + " flags from package '" + package_name + "'.";
    return true;
}

void RenderBuildSettingsModal(core::Project& project, core::CompilerManager& compiler_manager) {
    if (s_ShowSettingsModal) {
        ImGui::OpenPopup("Build Settings##Modal");
    }

    ImGui::SetNextWindowSize(ImVec2(Theme::Dim::ModalW, Theme::Dim::ModalH), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::CardRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Col::BgExplorer);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Col::Border);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    bool modal_open = ImGui::BeginPopupModal("Build Settings##Modal", nullptr, flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (modal_open) {
        const float dpi = Theme::DpiScale;
        const float hdr_h = Theme::HeaderHeight + 4.0f * dpi; // 38px * dpi consistent header bar
        const float modal_w = ImGui::GetWindowWidth();
        const ImVec2 win_pos = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Top Monochrome Header Bar matching Main Window TitleBar + Card style
        dl->AddRectFilled(
            win_pos,
            ImVec2(win_pos.x + modal_w, win_pos.y + hdr_h),
            ImGui::ColorConvertFloat4ToU32(Theme::Col::BgChrome),
            Theme::CardRounding,
            ImDrawFlags_RoundCornersTop
        );
        dl->AddLine(
            ImVec2(win_pos.x, std::floor(win_pos.y + hdr_h) + 0.5f),
            ImVec2(win_pos.x + modal_w, std::floor(win_pos.y + hdr_h) + 0.5f),
            ImGui::ColorConvertFloat4ToU32(Theme::Col::Border),
            1.0f
        );

        ImGui::PushFont(g_Fonts.title);
        const float title_y = (hdr_h - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPos(ImVec2(Theme::Sp::S4, title_y));
        ImGui::TextColored(Theme::Col::Text, "%s  Build Settings", ICON_FA_GEAR);
        ImGui::PopFont();

        std::string proj_name = project.GetRootPath().filename().generic_u8string();
        if (!proj_name.empty()) {
            ImGui::SameLine(0.0f, Theme::Sp::S2);
            ImGui::SetCursorPosY((hdr_h - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(Theme::Col::TextDim, "·  %s", proj_name.c_str());
        }

        // Top-right close X button
        ImGui::SetCursorPos(ImVec2(modal_w - Theme::Btn::IconW - Theme::Sp::S3, (hdr_h - Theme::Btn::SmallH) * 0.5f));
        if (Theme::IconButton("##SettingsCloseX", ICON_FA_XMARK, "Save & close", false, ImVec2(Theme::Btn::IconW, Theme::Btn::SmallH))) {
            project.Save();
            s_ShowSettingsModal = false;
            ImGui::CloseCurrentPopup();
        }

        // Padded Main Modal Body
        ImGui::SetCursorPos(ImVec2(0.0f, hdr_h + 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Sp::S4, Theme::Sp::S3));
        ImGui::BeginChild("SettingsModalBody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        static char s_PkgConfigInput[128] = "";
        static std::string s_PkgConfigStatus = "";

        const auto& targets = project.GetTargets();
        const bool has_targets = !targets.empty();
        if (!s_SelectedTargetScope.empty() && !project.FindTarget(s_SelectedTargetScope)) {
            s_SelectedTargetScope.clear();
            s_SelectedFolderScope.clear();
            s_SelectedFileScope.clear();
        }
        if ((!s_SelectedFolderScope.empty() || !s_SelectedFileScope.empty()) && s_SelectedTargetScope.empty() && has_targets) {
            if (const core::Target* pt = project.GetPrimaryTarget()) {
                s_SelectedTargetScope = pt->name;
            }
        }

        // Scope Breadcrumb Bar: Project ▸ Target ▸ [Folder] ▸ [File]  +  Config ▾ [Manage...] (V2-3 & V2-4)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Theme::Col::TextDim, "Scope:");
            ImGui::SameLine(0.0f, Theme::Sp::S1);

            if (Theme::Button("Project", "Edit global project settings", s_SelectedTargetScope.empty(), ImVec2(0.0f, Theme::Btn::SmallH))) {
                s_SelectedTargetScope.clear();
                s_SelectedFolderScope.clear();
                s_SelectedFileScope.clear();
            }

            if (project.IsMultiTargetOrRulesUI() || targets.size() > 1 || !s_SelectedTargetScope.empty() || !s_SelectedFolderScope.empty() || !s_SelectedFileScope.empty()) {
                ImGui::SameLine(0.0f, Theme::Sp::S1);
                ImGui::TextColored(Theme::Col::TextDim, ">");
                ImGui::SameLine(0.0f, Theme::Sp::S1);

                std::string tgt_lbl = s_SelectedTargetScope.empty() ? "Target..." : s_SelectedTargetScope;
                ImGui::SetNextItemWidth(135.0f * dpi);
                if (ImGui::BeginCombo("##ScopeTargetCombo", tgt_lbl.c_str())) {
                    for (const auto& t : targets) {
                        std::string item = t.name + " (" + core::TargetKindToString(t.kind) + ")";
                        if (ImGui::Selectable(item.c_str(), s_SelectedTargetScope == t.name)) {
                            s_SelectedTargetScope = t.name;
                            s_SelectedFolderScope.clear();
                            s_SelectedFileScope.clear();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (!s_SelectedFolderScope.empty()) {
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    ImGui::TextColored(Theme::Col::TextDim, ">");
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button((ICON_FA_FOLDER " " + s_SelectedFolderScope).c_str(), "Folder override scope (click to stay on folder)", s_SelectedFileScope.empty(), ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_SelectedFileScope.clear();
                    }
                }

                if (!s_SelectedFileScope.empty()) {
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    ImGui::TextColored(Theme::Col::TextDim, ">");
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    std::string fname = std::filesystem::path(s_SelectedFileScope).filename().generic_u8string();
                    Theme::Button((ICON_FA_FILE_CODE " " + fname).c_str(), s_SelectedFileScope.c_str(), true, ImVec2(0.0f, Theme::Btn::SmallH));
                }
            }

            // Right-side Config selector + Manage...
            const float cfg_combo_w = 110.0f * dpi;
            const float manage_w = 68.0f * dpi;
            float right_start = ImGui::GetWindowContentRegionMax().x - cfg_combo_w - manage_w - Theme::Sp::S2;
            if (right_start > ImGui::GetCursorPosX() + 10.0f * dpi) {
                ImGui::SameLine(right_start);
            } else {
                ImGui::SameLine();
            }
            std::string cur_cfg = project.GetConfiguration();
            ImGui::SetNextItemWidth(cfg_combo_w);
            if (ImGui::BeginCombo("##ScopeCfgCombo", cur_cfg.c_str())) {
                for (const auto& cname : project.GetConfigNames()) {
                    if (ImGui::Selectable(cname.c_str(), cname == cur_cfg)) {
                        project.SetConfiguration(cname);
                    }
                }
                ImGui::Separator();
                if (ImGui::Selectable("Manage configurations...")) {
                    s_ShowManageConfigsModal = true;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (Theme::Button("Manage", "Manage build configurations (add, duplicate, rename, presets)", false, ImVec2(manage_w, Theme::Btn::SmallH))) {
                s_ShowManageConfigsModal = true;
            }
            ImGui::Spacing();
        }

        const float preview_footer_reserve = Theme::Dim::CmdBoxH + Theme::Btn::DefaultH + Theme::Sp::S4 * 3.5f;
        const float tab_scroll_h = std::max(140.0f * dpi, ImGui::GetContentRegionAvail().y - preview_footer_reserve);

        // V2-5: Show clear error banner in Settings if any condition expression is invalid
        {
            std::string cond_banner_err;
            if (!core::ValidateAllTargetConditions(targets, cond_banner_err)) {
                ImGui::TextColored(Theme::Col::Error, "%s %s", ICON_FA_CIRCLE_EXCLAMATION, cond_banner_err.c_str());
                ImGui::Spacing();
            } else {
                for (const auto& [ln, lc] : project.GetLibConditions()) {
                    std::string cerr;
                    if (!lc.empty() && !core::ValidateCondition(lc, cerr)) {
                        ImGui::TextColored(Theme::Col::Error, "%s Project library '%s' invalid condition '%s': %s",
                            ICON_FA_CIRCLE_EXCLAMATION, ln.c_str(), lc.c_str(), cerr.c_str());
                        ImGui::Spacing();
                        break;
                    }
                }
            }
        }

        if (!s_SelectedTargetScope.empty() && (!s_SelectedFolderScope.empty() || !s_SelectedFileScope.empty())) {
            // -----------------------------------------------------------------
            // V2-3 Folder / File Override Scope Editor
            // -----------------------------------------------------------------
            const core::Target* cur_target_ptr = project.FindTarget(s_SelectedTargetScope);
            if (cur_target_ptr) {
                core::Target t_edit = *cur_target_ptr;
                std::string match_pat = !s_SelectedFileScope.empty()
                    ? s_SelectedFileScope
                    : (s_SelectedFolderScope + "/**");

                int ov_idx = -1;
                for (size_t i = 0; i < t_edit.overrides.size(); ++i) {
                    if (t_edit.overrides[i].match == match_pat) {
                        ov_idx = static_cast<int>(i);
                        break;
                    }
                }

                core::FlagOverride ov_work;
                bool exists_in_target = (ov_idx >= 0);
                if (exists_in_target) {
                    ov_work = t_edit.overrides[static_cast<size_t>(ov_idx)];
                } else {
                    ov_work.match = match_pat;
                }

                bool ov_modified = false;
                const std::string cur_cfg = project.GetConfiguration();

                // Compute inherited baseline for dimmed preview
                std::unordered_map<std::string, const core::Target*> tmap;
                for (const auto& t : targets) tmap[t.name] = &t;
                std::string comp_low = project.GetCompiler();
                std::transform(comp_low.begin(), comp_low.end(), comp_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
                core::EffectiveTargetSettings inh = core::ResolveEffectiveTargetSettings(project, t_edit, tmap, cur_cfg, is_msvc);
                core::ResolvedConfigParams cfg_p = core::ResolveConfigParams(project, cur_cfg);

                if (ImGui::BeginTabBar("OverrideScopeTabs", ImGuiTabBarFlags_None)) {
                    if (ImGui::BeginTabItem("Flag Override")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("OvEditScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                        ImGui::TextColored(Theme::Col::Accent, "Match Pattern: %s", ov_work.match.c_str());
                        if (!exists_in_target) {
                            ImGui::SameLine(0.0f, Theme::Sp::S2);
                            ImGui::TextColored(Theme::Col::TextDim, "(not yet saved — edit any field below to create override)");
                        }
                        ImGui::Spacing();

                        // Optimization override (with ● dot + ↺ reset)
                        {
                            bool is_ov = !ov_work.optimize.empty();
                            ImGui::TextColored(is_ov ? Theme::Col::Accent : Theme::Col::TextDim, is_ov ? "* Optimize:" : "  Optimize:");
                            Theme::Tooltip(("Inherited from config '" + cur_cfg + "': " + cfg_p.optimize).c_str());
                            ImGui::SameLine(Theme::Dim::FormLabelW);
                            const char* opts[] = { "(inherit)", "O0", "O1", "O2", "O3", "Os" };
                            std::string preview = is_ov ? ov_work.optimize : ("inherit (" + cfg_p.optimize + ")");
                            ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                            if (ImGui::BeginCombo("##OvOpt", preview.c_str())) {
                                for (const char* o : opts) {
                                    std::string val = (std::strcmp(o, "(inherit)") == 0) ? "" : o;
                                    if (ImGui::Selectable(o, ov_work.optimize == val)) {
                                        ov_work.optimize = val;
                                        ov_modified = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if (is_ov) {
                                ImGui::SameLine(0.0f, Theme::Sp::S1);
                                if (Theme::Button("Reset##opt", "Reset to inherited optimization level", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                                    ov_work.optimize.clear();
                                    ov_modified = true;
                                }
                            }
                        }

                        // Warnings override
                        {
                            bool is_ov = !ov_work.settings.warnings.empty();
                            ImGui::TextColored(is_ov ? Theme::Col::Accent : Theme::Col::TextDim, is_ov ? "* Warnings:" : "  Warnings:");
                            Theme::Tooltip(("Inherited: " + inh.warnings).c_str());
                            ImGui::SameLine(Theme::Dim::FormLabelW);
                            const char* wopts[] = { "(inherit)", "Off", "-Wall", "-Wall -Wextra" };
                            std::string preview = is_ov ? ov_work.settings.warnings : ("inherit (" + inh.warnings + ")");
                            ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                            if (ImGui::BeginCombo("##OvWarn", preview.c_str())) {
                                for (const char* w : wopts) {
                                    std::string val = (std::strcmp(w, "(inherit)") == 0) ? "" : w;
                                    if (ImGui::Selectable(w, ov_work.settings.warnings == val)) {
                                        ov_work.settings.warnings = val;
                                        ov_modified = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if (is_ov) {
                                ImGui::SameLine(0.0f, Theme::Sp::S1);
                                if (Theme::Button("Reset##warn", "Reset warnings to inherited value", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                                    ov_work.settings.warnings.clear();
                                    ov_modified = true;
                                }
                            }
                        }

                        // C++ / C standard override
                        {
                            bool is_ov = !ov_work.settings.cxxStd.empty();
                            ImGui::TextColored(is_ov ? Theme::Col::Accent : Theme::Col::TextDim, is_ov ? "* C++ Std:" : "  C++ Std:");
                            Theme::Tooltip(("Inherited: " + inh.cxxStd).c_str());
                            ImGui::SameLine(Theme::Dim::FormLabelW);
                            const char* stds[] = { "(inherit)", "c++11", "c++14", "c++17", "c++20", "c++23" };
                            std::string preview = is_ov ? ov_work.settings.cxxStd : ("inherit (" + inh.cxxStd + ")");
                            ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                            if (ImGui::BeginCombo("##OvCxxStd", preview.c_str())) {
                                for (const char* s : stds) {
                                    std::string val = (std::strcmp(s, "(inherit)") == 0) ? "" : s;
                                    if (ImGui::Selectable(s, ov_work.settings.cxxStd == val)) {
                                        ov_work.settings.cxxStd = val;
                                        ov_modified = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if (is_ov) {
                                ImGui::SameLine(0.0f, Theme::Sp::S1);
                                if (Theme::Button("Reset##cxx", "Reset C++ standard", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                                    ov_work.settings.cxxStd.clear();
                                    ov_modified = true;
                                }
                            }
                        }

                        // Condition (V2-5 live validation)
                        {
                            bool is_ov = !ov_work.condition.empty();
                            ImGui::TextColored(is_ov ? Theme::Col::Accent : Theme::Col::TextDim, is_ov ? "* Condition (if):" : "  Condition (if):");
                            ImGui::SameLine(Theme::Dim::FormLabelW);
                            char cbuf[256];
                            std::strncpy(cbuf, ov_work.condition.c_str(), sizeof(cbuf) - 1);
                            cbuf[sizeof(cbuf) - 1] = '\0';
                            ImGui::SetNextItemWidth(220.0f * dpi);
                            if (ImGui::InputTextWithHint("##OvCond", "e.g. gcc || clang", cbuf, sizeof(cbuf))) {
                                ov_work.condition = cbuf;
                                ov_modified = true;
                            }
                            if (!ov_work.condition.empty()) {
                                core::ConditionEvalResult ev = core::EvaluateCondition(ov_work.condition, core::BuildConditionContext(project));
                                ImGui::SameLine();
                                if (!ev.valid) {
                                    ImGui::TextColored(Theme::Col::Error, "%s %s", ICON_FA_CIRCLE_EXCLAMATION, ev.error.c_str());
                                } else if (ev.active) {
                                    ImGui::TextColored(Theme::Col::Success, "active");
                                } else {
                                    ImGui::TextColored(Theme::Col::Warning, "%s", ev.reason.c_str());
                                }
                            }
                        }

                        if (ImGui::Checkbox("Exclude from PCH", &ov_work.excludeFromPch)) ov_modified = true;
                        ImGui::SameLine(0.0f, Theme::Sp::S3);
                        if (ImGui::Checkbox("Exclude from Unity", &ov_work.excludeFromUnity)) ov_modified = true;

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ov_work.settings.compileFlags.empty() ? Theme::Col::TextDim : Theme::Col::Accent,
                            ov_work.settings.compileFlags.empty() ? "Compile Flags:" : "* Compile Flags (overridden):");
                        if (RenderEditableList("OvCompFlags", ov_work.settings.compileFlags, false, false, project, "Add flag (e.g. -mavx2)")) {
                            ov_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(ov_work.settings.defines.empty() ? Theme::Col::TextDim : Theme::Col::Accent,
                            ov_work.settings.defines.empty() ? "Defines:" : "* Defines (overridden):");
                        if (RenderEditableList("OvDefines", ov_work.settings.defines, false, false, project, "Add define")) {
                            ov_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(ov_work.settings.includeDirs.empty() ? Theme::Col::TextDim : Theme::Col::Accent,
                            ov_work.settings.includeDirs.empty() ? "Include Dirs:" : "* Include Dirs (overridden):");
                        if (RenderEditableList("OvIncDirs", ov_work.settings.includeDirs, true, true, project, "Add include dir")) {
                            ov_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        auto& ocfg = ov_work.configs[cur_cfg];
                        bool has_cfg_opt = !ocfg.optimize.empty();
                        ImGui::TextColored(has_cfg_opt ? Theme::Col::Accent : Theme::Col::TextDim,
                            "Per-Config (%s) Optimize:", cur_cfg.c_str());
                        ImGui::SameLine(Theme::Dim::FormLabelW);
                        const char* copts[] = { "(inherit)", "O0", "O1", "O2", "O3", "Os" };
                        std::string cprev = has_cfg_opt ? ocfg.optimize : "(inherit)";
                        ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                        if (ImGui::BeginCombo("##OvCfgOpt", cprev.c_str())) {
                            for (const char* o : copts) {
                                std::string val = (std::strcmp(o, "(inherit)") == 0) ? "" : o;
                                if (ImGui::Selectable(o, ocfg.optimize == val)) {
                                    ocfg.optimize = val;
                                    ov_modified = true;
                                }
                            }
                            ImGui::EndCombo();
                        }

                        if (exists_in_target) {
                            ImGui::Spacing();
                            if (Theme::Button("Remove Entire Override", "Delete this override rule from target", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                                t_edit.overrides.erase(t_edit.overrides.begin() + ov_idx);
                                project.UpdateTarget(t_edit);
                                s_SelectedFolderScope.clear();
                                s_SelectedFileScope.clear();
                                ov_modified = false;
                            }
                        }

                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Effective Flags")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("OvEffScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                        std::string eff_file = !s_SelectedFileScope.empty() ? s_SelectedFileScope
                            : (!t_edit.expandedFiles.empty() ? t_edit.expandedFiles[0] : "src/main.cpp");
                        core::EffectiveFileSettings efs = core::ResolveFileSettings(
                            project, t_edit, inh, eff_file, cur_cfg, project.GetCompiler(), false
                        );
                        ImGui::TextColored(Theme::Col::TextDim, "File: %s | Preset: %s | Optimize: %s | Debug: %s | Runtime: %s",
                            eff_file.c_str(), efs.basePreset.c_str(), efs.optimize.c_str(), efs.debugInfo.c_str(), efs.runtime.c_str());
                        ImGui::Spacing();
                        if (ImGui::BeginTable("EffFlagsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("Flag", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                            ImGui::TableSetupColumn("Origin", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                            ImGui::TableHeadersRow();
                            for (const auto& fo : efs.effectiveFlags) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextColored(fo.overridden ? Theme::Col::TextDim : Theme::Col::Text, "%s", fo.flag.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextColored(Theme::Col::TextDim, "%s", fo.origin.c_str());
                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextColored(fo.overridden ? Theme::Col::Warning : Theme::Col::Success, "%s",
                                    fo.overridden ? fo.reason.c_str() : "active");
                            }
                            ImGui::EndTable();
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                if (ov_modified) {
                    // Clean empty config maps before saving
                    for (auto it = ov_work.configs.begin(); it != ov_work.configs.end(); ) {
                        if (it->second.IsEmpty()) it = ov_work.configs.erase(it);
                        else ++it;
                    }
                    if (exists_in_target) {
                        t_edit.overrides[static_cast<size_t>(ov_idx)] = std::move(ov_work);
                    } else {
                        t_edit.overrides.push_back(std::move(ov_work));
                    }
                    project.UpdateTarget(t_edit);
                }
            }
        } else if (!s_SelectedTargetScope.empty()) {
            const core::Target* cur_target_ptr = project.FindTarget(s_SelectedTargetScope);
            if (cur_target_ptr) {
                core::Target t_edit = *cur_target_ptr;
                bool t_modified = false;

                if (ImGui::BeginTabBar("TargetSettingsTabs", ImGuiTabBarFlags_None)) {
                    if (ImGui::BeginTabItem("Target")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetMainScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                        ImGui::TextColored(Theme::Col::Text, "Target Kind:");
                        ImGui::SameLine(Theme::Dim::FormLabelW);
                        const char* kinds[] = { "executable", "static", "shared", "interface" };
                        std::string cur_kind_str = core::TargetKindToString(t_edit.kind);
                        ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                        if (ImGui::BeginCombo("##TargetKindCombo", cur_kind_str.c_str())) {
                            for (const char* k : kinds) {
                                if (ImGui::Selectable(k, cur_kind_str == k)) {
                                    t_edit.kind = core::TargetKindFromString(k);
                                    t_modified = true;
                                }
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::Text, "Output Name:");
                        ImGui::SameLine(Theme::Dim::FormLabelW);
                        char out_buf[128];
                        std::strncpy(out_buf, t_edit.outputName.c_str(), sizeof(out_buf) - 1);
                        out_buf[sizeof(out_buf) - 1] = '\0';
                        ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                        if (ImGui::InputTextWithHint("##TargetOutName", t_edit.name.c_str(), out_buf, sizeof(out_buf))) {
                            t_edit.outputName = out_buf;
                            t_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::Text, "Condition (if):");
                        ImGui::SameLine(Theme::Dim::FormLabelW);
                        char tcond_buf[256];
                        std::strncpy(tcond_buf, t_edit.condition.c_str(), sizeof(tcond_buf) - 1);
                        tcond_buf[sizeof(tcond_buf) - 1] = '\0';
                        ImGui::SetNextItemWidth(240.0f * dpi);
                        if (ImGui::InputTextWithHint("##TargetCond", "e.g. windows && !config:Shipping", tcond_buf, sizeof(tcond_buf))) {
                            t_edit.condition = tcond_buf;
                            t_modified = true;
                        }
                        if (!t_edit.condition.empty()) {
                            core::ConditionEvalResult tev = core::EvaluateCondition(t_edit.condition, core::BuildConditionContext(project));
                            ImGui::SameLine();
                            if (!tev.valid) {
                                ImGui::TextColored(Theme::Col::Error, "%s %s", ICON_FA_CIRCLE_EXCLAMATION, tev.error.c_str());
                            } else if (tev.active) {
                                ImGui::TextColored(Theme::Col::Success, "active");
                            } else {
                                ImGui::TextColored(Theme::Col::Warning, "%s", tev.reason.c_str());
                            }
                        }

                        if (t_edit.kind == core::TargetKind::Shared) {
                            ImGui::Spacing();
                            if (ImGui::Checkbox("Export all symbols (GCC/MinGW -Wl,--export-all-symbols)", &t_edit.exportAll)) {
                                t_modified = true;
                            }
                            std::string macro = t_edit.GetExportsMacro();
                            std::string snippet =
                                "#ifdef _WIN32\n"
                                "#  ifdef " + macro + "\n"
                                "#    define API __declspec(dllexport)\n"
                                "#  else\n"
                                "#    define API __declspec(dllimport)\n"
                                "#  endif\n"
                                "#else\n"
                                "#  define API\n"
                                "#endif";
                            ImGui::TextColored(Theme::Col::TextDim, "Auto-defined macro: %s", macro.c_str());
                            ImGui::SameLine(0.0f, Theme::Sp::S2);
                            if (Theme::Button("Copy DLL Macro", "Copy __declspec(dllexport/dllimport) macro snippet", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                                ImGui::SetClipboardText(snippet.c_str());
                            }
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::Text, "Freestanding / Kernel Target (V2-15):");
                        if (ImGui::Checkbox("Freestanding / Kernel binary (no OS host runtime)", &t_edit.freestanding)) {
                            t_modified = true;
                        }
                        if (t_edit.freestanding) {
                            ImGui::Indent(12.0f * dpi);
                            char ls_buf[512];
                            std::strncpy(ls_buf, t_edit.linkerScript.c_str(), sizeof(ls_buf) - 1);
                            ls_buf[sizeof(ls_buf) - 1] = '\0';
                            ImGui::TextColored(Theme::Col::TextDim, "Linker script:");
                            ImGui::SetNextItemWidth(std::max(140.0f * dpi, ImGui::GetContentRegionAvail().x - 95.0f * dpi));
                            if (ImGui::InputTextWithHint("##TgtLinkerScript", "e.g. linker.ld", ls_buf, sizeof(ls_buf))) {
                                t_edit.linkerScript = ls_buf;
                                t_modified = true;
                            }
                            ImGui::SameLine(0.0f, Theme::Sp::S1);
                            if (Theme::Button("Browse...##LsBrowse", "Pick linker script (.ld / .lds)", false, ImVec2(85.0f * dpi, Theme::Btn::DefaultH))) {
                                nfdchar_t* out_ls = nullptr;
                                std::string root_s = project.GetRootPath().generic_u8string();
                                nfdu8filteritem_t ls_filter[1] = { { "Linker Scripts", "ld,lds" } };
                                if (NFD_OpenDialog(&out_ls, ls_filter, 1, root_s.empty() ? nullptr : root_s.c_str()) == NFD_OKAY && out_ls) {
                                    t_edit.linkerScript = MakeRelativeIfInside(std::filesystem::u8path(out_ls), project.GetRootPath());
                                    NFD_FreePath(out_ls);
                                    t_modified = true;
                                }
                            }

                            char entry_buf[128];
                            std::strncpy(entry_buf, t_edit.entry.c_str(), sizeof(entry_buf) - 1);
                            entry_buf[sizeof(entry_buf) - 1] = '\0';
                            ImGui::TextColored(Theme::Col::TextDim, "Entry symbol:");
                            ImGui::SetNextItemWidth(220.0f * dpi);
                            if (ImGui::InputTextWithHint("##TgtEntry", "_start", entry_buf, sizeof(entry_buf))) {
                                t_edit.entry = entry_buf;
                                t_modified = true;
                            }
                            ImGui::SameLine(0.0f, Theme::Sp::S2);
                            if (ImGui::Checkbox("Link libgcc (-lgcc)", &t_edit.linkLibgcc)) {
                                t_modified = true;
                            }

                            ImGui::TextColored(Theme::Col::TextDim, "C++-only flags (cxxFlags):");
                            if (RenderEditableList("TargetCxxFlags", t_edit.cxxFlags, false, false, project, "Add C++-only flag (e.g. -fno-exceptions)")) {
                                t_modified = true;
                            }
                            ImGui::Unindent(12.0f * dpi);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::Text, "Dependencies:");
                        for (const auto& other : targets) {
                            if (other.name == t_edit.name) continue;
                            bool is_priv_dep = std::find(t_edit.deps.begin(), t_edit.deps.end(), other.name) != t_edit.deps.end();
                            bool is_pub_dep = std::find(t_edit.publicDeps.begin(), t_edit.publicDeps.end(), other.name) != t_edit.publicDeps.end();
                            bool has_dep = is_priv_dep || is_pub_dep;

                            ImGui::PushID(other.name.c_str());
                            if (ImGui::Checkbox(other.name.c_str(), &has_dep)) {
                                t_edit.deps.erase(std::remove(t_edit.deps.begin(), t_edit.deps.end(), other.name), t_edit.deps.end());
                                t_edit.publicDeps.erase(std::remove(t_edit.publicDeps.begin(), t_edit.publicDeps.end(), other.name), t_edit.publicDeps.end());
                                if (has_dep) {
                                    t_edit.deps.push_back(other.name);
                                }
                                t_modified = true;
                            }
                            ImGui::SameLine();
                            ImGui::TextColored(Theme::Col::TextDim, "(%s)", core::TargetKindToString(other.kind).c_str());
                            if (has_dep) {
                                ImGui::SameLine(0.0f, Theme::Sp::S3);
                                bool pub_flag = is_pub_dep;
                                if (ImGui::Checkbox("public", &pub_flag)) {
                                    t_edit.deps.erase(std::remove(t_edit.deps.begin(), t_edit.deps.end(), other.name), t_edit.deps.end());
                                    t_edit.publicDeps.erase(std::remove(t_edit.publicDeps.begin(), t_edit.publicDeps.end(), other.name), t_edit.publicDeps.end());
                                    if (pub_flag) t_edit.publicDeps.push_back(other.name);
                                    else t_edit.deps.push_back(other.name);
                                    t_modified = true;
                                }
                            }
                            ImGui::PopID();
                        }

                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    auto render_block_tabs = [&](core::SettingsBlock& blk, const char* id_prefix) {
                        ImGui::TextColored(Theme::Col::TextDim, "Include Directories (-I / /I):");
                        if (RenderEditableList((std::string(id_prefix) + "_inc").c_str(), blk.includeDirs, true, true, project, "Add include dir")) t_modified = true;
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::TextDim, "Defines (-D / /D):");
                        if (RenderEditableList((std::string(id_prefix) + "_def").c_str(), blk.defines, false, false, project, "Add define")) t_modified = true;
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::TextDim, "Library Search Dirs (-L / /LIBPATH):");
                        if (RenderEditableList((std::string(id_prefix) + "_ldir").c_str(), blk.libDirs, true, true, project, "Add lib dir")) t_modified = true;
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::TextDim, "Libraries:");
                        if (RenderConditionalLibsList((std::string(id_prefix) + "_libs").c_str(), blk.libs, blk.libConditions, project, "Add library")) t_modified = true;
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::TextDim, "Compile Flags:");
                        if (RenderEditableList((std::string(id_prefix) + "_cf").c_str(), blk.compileFlags, false, false, project, "Add compile flag")) t_modified = true;
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::TextDim, "Link Flags:");
                        if (RenderEditableList((std::string(id_prefix) + "_lf").c_str(), blk.linkFlags, false, false, project, "Add link flag")) t_modified = true;
                    };

                    if (ImGui::BeginTabItem("Private")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetPrivScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                        render_block_tabs(t_edit.priv, "priv");
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Public (Propagated)")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetPubScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                        render_block_tabs(t_edit.pub, "pub");
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Target Excludes")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetExclScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                        ImGui::TextColored(Theme::Col::TextDim, "Target-level exclude globs (files matching these are excluded from all rules):");
                        if (RenderEditableList("TargetExcludeList", t_edit.exclude, false, false, project, "Add exclude glob (e.g. **/*_disabled.cpp)")) {
                            t_modified = true;
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Overrides")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetOverridesScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                        for (const auto& w : core::ValidateTargetOverrides(t_edit)) {
                            ImGui::TextColored(Theme::Col::Warning, "%s %s", ICON_FA_TRIANGLE_EXCLAMATION, w.c_str());
                        }
                        for (const auto& sw : project.GetSchemaWarnings()) {
                            ImGui::TextColored(Theme::Col::Warning, "%s %s", ICON_FA_TRIANGLE_EXCLAMATION, sw.c_str());
                        }
                        if (t_edit.overrides.empty()) {
                            ImGui::TextColored(Theme::Col::TextDim, "No per-folder or per-file overrides yet.\nRight-click any folder or file and choose 'Set flags for this folder/file...'.");
                        } else {
                            int del_ov = -1;
                            const core::ConditionContext ov_ctx = core::BuildConditionContext(project);
                            for (size_t oi = 0; oi < t_edit.overrides.size(); ++oi) {
                                const auto& ov = t_edit.overrides[oi];
                                core::ConditionEvalResult oev = core::EvaluateCondition(ov.condition, ov_ctx);
                                ImGui::PushID(static_cast<int>(oi));
                                ImGui::TextColored(oev.active ? Theme::Col::Override : Theme::Col::Inactive, "%s", ICON_FA_FLAG);
                                ImGui::SameLine(0.0f, Theme::Sp::S1);
                                ImGui::TextColored(oev.active ? Theme::Col::Text : Theme::Col::Inactive, "%s", ov.match.c_str());
                                if (!ov.condition.empty()) {
                                    ImGui::SameLine();
                                    ImVec4 ccol = !oev.valid ? Theme::Col::Error : (oev.active ? Theme::Col::TextDim : Theme::Col::Inactive);
                                    ImGui::TextColored(ccol, "[if %s]", ov.condition.c_str());
                                    if (!oev.valid && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                        Theme::Tooltip(oev.error.c_str());
                                    } else if (!oev.active && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                        Theme::Tooltip(oev.reason.c_str());
                                    }
                                }
                                ImGui::SameLine();
                                if (Theme::Button("Edit", "Open override editor", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                                    std::string pat = ov.match;
                                    if (pat.size() > 3 && pat.substr(pat.size() - 3) == "/**") {
                                        s_SelectedFolderScope = pat.substr(0, pat.size() - 3);
                                        s_SelectedFileScope.clear();
                                    } else {
                                        s_SelectedFileScope = pat;
                                        s_SelectedFolderScope = std::filesystem::path(pat).parent_path().generic_u8string();
                                    }
                                }
                                ImGui::SameLine();
                                if (Theme::DangerIconButton("##delov", ICON_FA_XMARK, "Delete override", ImVec2(Theme::Btn::DelW, Theme::Btn::SmallH))) {
                                    del_ov = static_cast<int>(oi);
                                }
                                ImGui::PopID();
                            }
                            if (del_ov >= 0) {
                                t_edit.overrides.erase(t_edit.overrides.begin() + del_ov);
                                t_modified = true;
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Effective Flags")) {
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetEffFlagsScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                        if (t_edit.expandedFiles.empty()) {
                            ImGui::TextColored(Theme::Col::TextDim, "No source files in this target.");
                        } else {
                            if (s_EffectiveFlagsFile.empty() || t_edit.expandedFilesSet.find(s_EffectiveFlagsFile) == t_edit.expandedFilesSet.end()) {
                                s_EffectiveFlagsFile = t_edit.expandedFiles[0];
                            }
                            ImGui::TextColored(Theme::Col::Text, "Inspect File:");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(320.0f * dpi);
                            if (ImGui::BeginCombo("##EffFileCombo", s_EffectiveFlagsFile.c_str())) {
                                size_t limit = std::min<size_t>(t_edit.expandedFiles.size(), 300);
                                for (size_t fi = 0; fi < limit; ++fi) {
                                    const std::string& f = t_edit.expandedFiles[fi];
                                    if (ImGui::Selectable(f.c_str(), s_EffectiveFlagsFile == f)) {
                                        s_EffectiveFlagsFile = f;
                                    }
                                }
                                ImGui::EndCombo();
                            }

                            std::unordered_map<std::string, const core::Target*> tmap;
                            for (const auto& t : targets) tmap[t.name] = &t;
                            std::string comp_low = project.GetCompiler();
                            std::transform(comp_low.begin(), comp_low.end(), comp_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
                            core::EffectiveTargetSettings inh = core::ResolveEffectiveTargetSettings(project, t_edit, tmap, project.GetConfiguration(), is_msvc);
                            core::EffectiveFileSettings efs = core::ResolveFileSettings(
                                project, t_edit, inh, s_EffectiveFlagsFile, project.GetConfiguration(), project.GetCompiler(), false
                            );

                            ImGui::Spacing();
                            ImGui::TextColored(Theme::Col::TextDim, "Preset: %s | Optimize: %s | Debug: %s | Runtime: %s | LTO: %s",
                                efs.basePreset.c_str(), efs.optimize.c_str(), efs.debugInfo.c_str(), efs.runtime.c_str(), efs.lto.c_str());
                            ImGui::Spacing();
                            if (ImGui::BeginTable("TgtEffFlagsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                                ImGui::TableSetupColumn("Flag", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                ImGui::TableSetupColumn("Origin", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                                ImGui::TableHeadersRow();
                                for (const auto& fo : efs.effectiveFlags) {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::TextColored(fo.overridden ? Theme::Col::TextDim : Theme::Col::Text, "%s", fo.flag.c_str());
                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::TextColored(Theme::Col::TextDim, "%s", fo.origin.c_str());
                                    ImGui::TableSetColumnIndex(2);
                                    ImGui::TextColored(fo.overridden ? Theme::Col::Warning : Theme::Col::Success, "%s",
                                        fo.overridden ? fo.reason.c_str() : "active");
                                }
                                ImGui::EndTable();
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    // V2-6: Pre-build and post-build Steps Editor
                    ImGuiTabItemFlags steps_tab_flags = s_FocusStepsTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                    if (ImGui::BeginTabItem("Steps", nullptr, steps_tab_flags)) {
                        s_FocusStepsTab = false;
                        ImGui::Spacing();
                        const float steps_avail_h = tab_scroll_h - ImGui::GetFrameHeightWithSpacing();
                        static std::vector<core::StepDryRunEntry> s_DryRunResults;
                        static std::string s_DryRunTargetName;

                        if (s_SelectedStepIndex < 0 && !t_edit.steps.empty()) {
                            s_SelectedStepIndex = 0;
                        }
                        if (s_SelectedStepIndex >= static_cast<int>(t_edit.steps.size())) {
                            s_SelectedStepIndex = static_cast<int>(t_edit.steps.size()) - 1;
                        }

                        // Left pane: ordered list of steps (drag-to-reorder) + Add / Remove / Dry Run
                        ImGui::BeginChild("StepsOrderPane", ImVec2(210.0f * dpi, steps_avail_h), true);
                        const core::ConditionContext st_ctx = core::BuildConditionContext(project);
                        int del_step_idx = -1;
                        int swap_from = -1, swap_to = -1;

                        for (size_t si = 0; si < t_edit.steps.size(); ++si) {
                            const auto& st = t_edit.steps[si];
                            core::ConditionEvalResult sev = core::EvaluateCondition(st.condition, st_ctx);
                            ImGui::PushID(static_cast<int>(si));
                            std::string label = std::string(ICON_FA_GEAR " ") + (st.name.empty() ? "(unnamed)" : st.name) +
                                                "  [" + (st.phase == core::StepPhase::PostBuild ? "post" : "pre") + "]";
                            if (!sev.active) {
                                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Col::Inactive);
                            } else if (!sev.valid) {
                                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Col::Error);
                            }
                            if (ImGui::Selectable(label.c_str(), s_SelectedStepIndex == static_cast<int>(si))) {
                                s_SelectedStepIndex = static_cast<int>(si);
                            }
                            if (!sev.active || !sev.valid) {
                                ImGui::PopStyleColor();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                    Theme::Tooltip(sev.valid ? sev.reason.c_str() : sev.error.c_str());
                                }
                            }

                            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                int idx_payload = static_cast<int>(si);
                                ImGui::SetDragDropPayload("GENIE_STEP_REORDER", &idx_payload, sizeof(int));
                                ImGui::TextUnformatted(label.c_str());
                                ImGui::EndDragDropSource();
                            }
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GENIE_STEP_REORDER")) {
                                    int src_idx = *static_cast<const int*>(payload->Data);
                                    if (src_idx >= 0 && src_idx < static_cast<int>(t_edit.steps.size()) && src_idx != static_cast<int>(si)) {
                                        swap_from = src_idx;
                                        swap_to = static_cast<int>(si);
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            ImGui::PopID();
                        }

                        if (swap_from >= 0 && swap_to >= 0) {
                            core::BuildStep moved = std::move(t_edit.steps[static_cast<size_t>(swap_from)]);
                            t_edit.steps.erase(t_edit.steps.begin() + swap_from);
                            t_edit.steps.insert(t_edit.steps.begin() + swap_to, std::move(moved));
                            s_SelectedStepIndex = swap_to;
                            t_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        if (Theme::Button("+ Add Step", "Add a pre-build or post-build step", false, ImVec2(-1, Theme::Btn::SmallH))) {
                            core::BuildStep ns;
                            ns.name = "step " + std::to_string(t_edit.steps.size() + 1);
                            ns.phase = core::StepPhase::PreBuild;
                            t_edit.steps.push_back(std::move(ns));
                            s_SelectedStepIndex = static_cast<int>(t_edit.steps.size()) - 1;
                            t_modified = true;
                        }
                        if (Theme::Button("Add Preset...", "Insert a built-in OS/image post-build step preset (V2-16)", false, ImVec2(-1, Theme::Btn::SmallH))) {
                            ImGui::OpenPopup("StepPresetsPopup");
                        }
                        if (ImGui::BeginPopup("StepPresetsPopup")) {
                            auto presets = core::GetBuiltInStepPresets();
                            auto tc_res = compiler_manager.ResolveProjectToolchain(project);
                            for (const auto& ps : presets) {
                                bool tool_ok = true;
                                if (!ps.requiredTool.empty()) {
                                    if (ps.requiredTool == "objcopy") {
                                        tool_ok = !tc_res.profile.objcopy.empty() || !core::ToolRegistry::ResolveToolExecutable("objcopy", project.GetToolOverrides()).empty();
                                    } else if (ps.requiredTool == "ld") {
                                        tool_ok = !tc_res.profile.ld.empty() || !core::ToolRegistry::ResolveToolExecutable("ld", project.GetToolOverrides()).empty();
                                    } else {
                                        tool_ok = !core::ToolRegistry::ResolveToolExecutable(ps.requiredTool, project.GetToolOverrides()).empty();
                                    }
                                }
                                std::string item_lbl = ps.label + (tool_ok ? "" : "  [tool missing]");
                                if (ImGui::MenuItem(item_lbl.c_str())) {
                                    t_edit.steps.push_back(ps.step);
                                    s_SelectedStepIndex = static_cast<int>(t_edit.steps.size()) - 1;
                                    t_modified = true;
                                }
                                if (!tool_ok && !ps.missingHint.empty()) {
                                    Theme::Tooltip(ps.missingHint.c_str());
                                }
                            }
                            ImGui::EndPopup();
                        }
                        if (s_SelectedStepIndex >= 0 && s_SelectedStepIndex < static_cast<int>(t_edit.steps.size())) {
                            if (Theme::Button("Move Up", nullptr, false, ImVec2(92.0f * dpi, Theme::Btn::SmallH)) && s_SelectedStepIndex > 0) {
                                std::swap(t_edit.steps[static_cast<size_t>(s_SelectedStepIndex)], t_edit.steps[static_cast<size_t>(s_SelectedStepIndex - 1)]);
                                s_SelectedStepIndex--;
                                t_modified = true;
                            }
                            ImGui::SameLine();
                            if (Theme::Button("Move Down", nullptr, false, ImVec2(92.0f * dpi, Theme::Btn::SmallH)) &&
                                s_SelectedStepIndex + 1 < static_cast<int>(t_edit.steps.size())) {
                                std::swap(t_edit.steps[static_cast<size_t>(s_SelectedStepIndex)], t_edit.steps[static_cast<size_t>(s_SelectedStepIndex + 1)]);
                                s_SelectedStepIndex++;
                                t_modified = true;
                            }
                            if (Theme::Button("Remove Step", nullptr, false, ImVec2(-1, Theme::Btn::SmallH))) {
                                del_step_idx = s_SelectedStepIndex;
                            }
                        }
                        if (del_step_idx >= 0) {
                            t_edit.steps.erase(t_edit.steps.begin() + del_step_idx);
                            if (s_SelectedStepIndex >= static_cast<int>(t_edit.steps.size())) {
                                s_SelectedStepIndex = static_cast<int>(t_edit.steps.size()) - 1;
                            }
                            t_modified = true;
                        }

                        ImGui::Spacing();
                        if (Theme::SuccessButton("Dry Run Steps", "Evaluate which steps would run and why without executing", ImVec2(-1, Theme::Btn::SmallH))) {
                            s_DryRunResults = core::DryRunTargetSteps(project, t_edit, targets, project.GetDirectoryCache());
                            s_DryRunTargetName = t_edit.name;
                        }
                        ImGui::EndChild();

                        ImGui::SameLine(0.0f, Theme::Sp::S3);

                        // Right pane: step fields + Preview + Dry run results
                        ImGui::BeginChild("StepsDetailPane", ImVec2(0.0f, steps_avail_h), true);
                        if (s_SelectedStepIndex >= 0 && s_SelectedStepIndex < static_cast<int>(t_edit.steps.size())) {
                            core::BuildStep& st = t_edit.steps[static_cast<size_t>(s_SelectedStepIndex)];
                            ImGui::PushID(s_SelectedStepIndex);

                            auto edit_text = [&](const char* lbl, const char* id, std::string& val, const char* hint) {
                                ImGui::TextColored(Theme::Col::Text, "%s", lbl);
                                ImGui::SameLine(115.0f * dpi);
                                char buf[512];
                                std::strncpy(buf, val.c_str(), sizeof(buf) - 1);
                                buf[sizeof(buf) - 1] = '\0';
                                ImGui::SetNextItemWidth(-1);
                                if (ImGui::InputTextWithHint(id, hint, buf, sizeof(buf))) {
                                    val = buf;
                                    t_modified = true;
                                }
                            };

                            edit_text("Name:", "##StName", st.name, "e.g. shaders");

                            ImGui::TextColored(Theme::Col::Text, "Phase:");
                            ImGui::SameLine(115.0f * dpi);
                            const char* phases[] = { "preBuild", "postBuild" };
                            std::string cur_ph = core::StepPhaseToString(st.phase);
                            ImGui::SetNextItemWidth(160.0f * dpi);
                            if (ImGui::BeginCombo("##StPhase", cur_ph.c_str())) {
                                for (const char* p : phases) {
                                    if (ImGui::Selectable(p, cur_ph == p)) {
                                        st.phase = core::StepPhaseFromString(p);
                                        t_modified = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }

                            edit_text("Condition (if):", "##StCond", st.condition, "optional, e.g. windows && !config:Shipping");
                            if (!st.condition.empty()) {
                                core::ConditionEvalResult sev = core::EvaluateCondition(st.condition, st_ctx);
                                if (!sev.valid) {
                                    ImGui::TextColored(Theme::Col::Error, "  %s %s", ICON_FA_CIRCLE_EXCLAMATION, sev.error.c_str());
                                } else if (!sev.active) {
                                    ImGui::TextColored(Theme::Col::Warning, "  %s", sev.reason.c_str());
                                }
                            }

                            edit_text("Foreach glob:", "##StForeach", st.foreachPattern, "e.g. shaders/**/*.{vert,frag,comp}");
                            if (st.foreachPattern.empty()) {
                                ImGui::TextColored(Theme::Col::TextDim, "Inputs (when not foreach):");
                                if (RenderEditableList("StInputs", st.inputs, true, false, project, "Add input glob or path")) {
                                    t_modified = true;
                                }
                            }

                            edit_text("Command:", "##StCmd", st.command, "e.g. glslc -MD -MF {out}.d {in} -o {out}");
                            Theme::Tooltip("Placeholders: {in} {out} {stem} {name} {dir} {gen} {bindir} {libdir} {objdir} {bin} {bin:target} {root} {config} {target} {objcopy} {ld}");

                            ImGui::TextColored(Theme::Col::TextDim, "Outputs:");
                            if (RenderEditableList("StOutputs", st.outputs, false, false, project, "Add output template (e.g. {gen}/{stem}.pb.cc)")) {
                                t_modified = true;
                            }

                            edit_text("Depfile:", "##StDepfile", st.depfile, "optional, e.g. {out}.d");

                            if (ImGui::Checkbox("Generates C/C++ sources", &st.generatesSources)) t_modified = true;
                            ImGui::SameLine(0.0f, Theme::Sp::S2);
                            if (ImGui::Checkbox("Use shell", &st.shell)) t_modified = true;
                            ImGui::SameLine(0.0f, Theme::Sp::S2);
                            if (ImGui::Checkbox("Always run", &st.always)) t_modified = true;

                            // Live Preview for first input
                            ImGui::Spacing();
                            ImGui::Separator();
                            std::vector<std::string> prev_outs;
                            std::string prev_cmd = core::PreviewStepCommand(project, t_edit, st, project.GetDirectoryCache(), &prev_outs);
                            ImGui::TextColored(Theme::Col::TextDim, "Preview (first input):");
                            ImGui::PushFont(g_Fonts.mono);
                            ImGui::TextWrapped("%s", prev_cmd.empty() ? "(empty command)" : prev_cmd.c_str());
                            if (!prev_outs.empty()) {
                                std::string outs_joined;
                                for (size_t oi = 0; oi < prev_outs.size(); ++oi) {
                                    if (oi > 0) outs_joined += ", ";
                                    outs_joined += prev_outs[oi];
                                }
                                ImGui::TextColored(Theme::Col::TextDim, "Outputs: %s", outs_joined.c_str());
                            }
                            ImGui::PopFont();
                            ImGui::PopID();
                        } else {
                            ImGui::TextColored(Theme::Col::TextDim, "No build steps yet. Click '+ Add Step' to add a pre-build or post-build command.");
                        }

                        if (!s_DryRunResults.empty() && s_DryRunTargetName == t_edit.name) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::TextColored(Theme::Col::Accent, "Dry Run Results:");
                            for (const auto& dr : s_DryRunResults) {
                                std::string tag = !dr.active ? "[INACTIVE]" : (dr.wouldRun ? "[RUN]" : "[SKIP]");
                                ImVec4 col = !dr.active ? Theme::Col::Inactive : (dr.wouldRun ? Theme::Col::Warning : Theme::Col::Success);
                                ImGui::TextColored(col, "%s %s%s — %s",
                                    tag.c_str(),
                                    dr.stepName.c_str(),
                                    dr.label.empty() ? "" : (" (" + dr.label + ")").c_str(),
                                    dr.reason.c_str());
                            }
                        }

                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    // V2-8: Precompiled Headers (PCH) Tab
                    ImGuiTabItemFlags pch_tab_flags = s_FocusPchTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                    if (ImGui::BeginTabItem("PCH", nullptr, pch_tab_flags)) {
                        s_FocusPchTab = false;
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetPchScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                        ImGui::TextColored(Theme::Col::TextDim, "Precompiled header shared across compatible translation units in '%s'.", t_edit.name.c_str());
                        ImGui::Spacing();

                        auto render_file_input = [&](const char* lbl, const char* id, std::string& val, const char* tip) {
                            ImGui::TextColored(Theme::Col::Text, "%s", lbl);
                            ImGui::SameLine(Theme::Dim::FormLabelW);
                            char buf[512];
                            std::strncpy(buf, val.c_str(), sizeof(buf) - 1);
                            buf[sizeof(buf) - 1] = '\0';
                            const float browse_w = 80.0f * dpi;
                            ImGui::SetNextItemWidth(std::max(120.0f * dpi, ImGui::GetContentRegionAvail().x - browse_w - Theme::Sp::S2));
                            if (ImGui::InputText(id, buf, sizeof(buf))) {
                                val = buf;
                                t_modified = true;
                            }
                            Theme::Tooltip(tip);
                            ImGui::SameLine(0.0f, Theme::Sp::S2);
                            std::string b_id = std::string("Browse##") + id;
                            if (Theme::Button(b_id.c_str(), "Pick header/source file", false, ImVec2(browse_w, Theme::Btn::DefaultH))) {
                                nfdchar_t* outPath = nullptr;
                                std::string init_dir = project.GetRootPath().generic_u8string();
                                if (NFD_OpenDialog(&outPath, nullptr, 0, init_dir.empty() ? nullptr : init_dir.c_str()) == NFD_OKAY && outPath) {
                                    val = MakeRelativeIfInside(std::filesystem::u8path(outPath), project.GetRootPath());
                                    t_modified = true;
                                    NFD_FreePath(outPath);
                                }
                            }
                        };

                        render_file_input("C++ PCH Header:", "##PchHeader", t_edit.pch.header, "e.g. src/core/pch.h");
                        ImGui::Spacing();
                        render_file_input("C PCH Header:", "##PchCHeader", t_edit.pch.cHeader, "Optional separate PCH for .c files");
                        ImGui::Spacing();
                        render_file_input("MSVC PCH Source:", "##PchSource", t_edit.pch.source, "Optional explicit .cpp for MSVC /Yc (auto-generated if empty)");
                        ImGui::Spacing();

                        ImGui::TextColored(Theme::Col::TextDim, "Exclude Glob Patterns:");
                        if (RenderEditableList("PchExcludeList", t_edit.pch.exclude, false, false, project, "Add exclude glob (e.g. src/vendor/**)")) {
                            t_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Suggest PCH from deps.bin
                        static std::string s_SuggestTarget;
                        static std::vector<core::PchHeaderCandidate> s_SuggestedHeaders;
                        static bool s_SuggestQueried = false;
                        static std::string s_SuggestMsg;
                        if (Theme::Button("Suggest PCH...", "Scan .geniebuild/obj/<Config>/deps.bin for headers used by >= 50% of TUs", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                            s_SuggestTarget = t_edit.name;
                            s_SuggestedHeaders.clear();
                            s_SuggestMsg.clear();
                            s_SuggestQueried = true;
                            std::filesystem::path deps_path = project.GetRootPath() / ".geniebuild" / "obj" / project.GetConfiguration() / "deps.bin";
                            if (!std::filesystem::exists(deps_path)) {
                                s_SuggestMsg = "No dependency log yet (" + deps_path.generic_u8string() + "). Run at least one build first.";
                            } else {
                                core::DepLog dlog;
                                if (dlog.Open(deps_path)) {
                                    std::filesystem::path obj_dir = t_edit.GetObjectDir(project.GetRootPath(), project.GetConfiguration());
                                    bool is_msvc = (project.GetCompiler() == "cl");
                                    std::vector<std::string> outs;
                                    for (const auto& sf : t_edit.expandedFiles) {
                                        if (!t_edit.IsFileEnabled(sf)) continue;
                                        outs.push_back((obj_dir / (sf + (is_msvc ? ".obj" : ".o"))).generic_u8string());
                                    }
                                    s_SuggestedHeaders = dlog.SuggestPchHeaders(outs, project.GetRootPath(), 0.5, 20);
                                    if (s_SuggestedHeaders.empty()) {
                                        s_SuggestMsg = "No common headers found across >= 50% of target TUs.";
                                    }
                                    dlog.Close();
                                }
                            }
                        }

                        if (s_SuggestQueried && s_SuggestTarget == t_edit.name) {
                            if (!s_SuggestMsg.empty()) {
                                ImGui::TextColored(Theme::Col::Warning, "%s", s_SuggestMsg.c_str());
                            } else if (!s_SuggestedHeaders.empty()) {
                                if (Theme::Button("Generate umbrella header & enable", "Writes .geniebuild/pch/<target>/suggested_pch.h with these includes", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                                    std::filesystem::path sug_rel = std::filesystem::path(".geniebuild") / "pch" / t_edit.name / "suggested_pch.h";
                                    std::filesystem::path sug_abs = project.GetRootPath() / sug_rel;
                                    std::filesystem::create_directories(sug_abs.parent_path());
                                    std::ofstream out(sug_abs, std::ios::binary);
                                    out << "#pragma once\n";
                                    for (size_t hi = 0; hi < s_SuggestedHeaders.size() && hi < 12; ++hi) {
                                        out << "#include \"" << s_SuggestedHeaders[hi].path << "\"\n";
                                    }
                                    t_edit.pch.header = sug_rel.generic_u8string();
                                    t_modified = true;
                                }
                                ImGui::Spacing();
                                for (const auto& cand : s_SuggestedHeaders) {
                                    std::string c_lbl = cand.path + "  (" + std::to_string(cand.tuCount) + " TUs)";
                                    if (ImGui::Selectable(c_lbl.c_str(), t_edit.pch.header == cand.path)) {
                                        t_edit.pch.header = cand.path;
                                        t_modified = true;
                                    }
                                }
                            }
                        }

                        if (!t_edit.pch.IsEmpty()) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                            const core::CompilerInfo* comp_info = compiler_manager.FindCompiler(project.GetCompiler());
                            if (comp_info) {
                                bool is_msvc = (comp_info->name == "cl");
                                bool is_clang = (comp_info->name.find("clang") != std::string::npos);
                                std::unordered_map<std::string, const core::Target*> tmap;
                                for (const auto& t : targets) tmap[t.name] = &t;
                                core::EffectiveTargetSettings eff = core::ResolveEffectiveTargetSettings(project, t_edit, tmap, project.GetConfiguration(), is_msvc);
                                std::vector<std::string> srcs;
                                std::unordered_map<std::string, std::shared_ptr<const core::EffectiveFileSettings>> feffs;
                                for (const auto& sf : t_edit.expandedFiles) {
                                    if (!t_edit.IsFileEnabled(sf)) continue;
                                    srcs.push_back(sf);
                                    feffs[sf] = std::make_shared<const core::EffectiveFileSettings>(
                                        core::ResolveFileSettings(project, t_edit, eff, sf, project.GetConfiguration(), project.GetCompiler(), false)
                                    );
                                }
                                auto an = core::PchManager::AnalyzeAndPlanTargetPch(
                                    project, t_edit, eff, srcs, feffs, *comp_info, *comp_info,
                                    project.GetConfiguration(), is_msvc, is_clang, false, false
                                );
                                ImGui::TextColored(Theme::Col::Accent, "Compatibility: %zu / %zu files will use PCH", an.includedCount, an.totalCount);
                                if (ImGui::BeginTable("PchUsageTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                                    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.48f);
                                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                                    ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 0.36f);
                                    ImGui::TableHeadersRow();
                                    size_t limit = std::min<size_t>(an.fileUsages.size(), 150);
                                    for (size_t i = 0; i < limit; ++i) {
                                        const auto& u = an.fileUsages[i];
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(u.file.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextColored(u.usesPch ? Theme::Col::Success : Theme::Col::TextDim, u.usesPch ? "PCH" : "Excluded");
                                        ImGui::TableSetColumnIndex(2);
                                        ImGui::TextColored(Theme::Col::TextDim, "%s", u.excludeReason.c_str());
                                    }
                                    ImGui::EndTable();
                                }
                            }
                        }

                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    // V2-9: Unity / Jumbo Builds Tab
                    ImGuiTabItemFlags unity_tab_flags = s_FocusUnityTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                    if (ImGui::BeginTabItem("Unity", nullptr, unity_tab_flags)) {
                        s_FocusUnityTab = false;
                        ImGui::Spacing();
                        ImGui::BeginChild("TargetUnityScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                        if (ImGui::Checkbox("Enable unity / jumbo builds for this target", &t_edit.unity.enabled)) {
                            t_modified = true;
                        }
                        Theme::Tooltip("Groups compatible C/C++ translation units into batched unity files under .geniebuild/unity/");
                        ImGui::Spacing();

                        int bsz = static_cast<int>(t_edit.unity.batchSize);
                        ImGui::TextColored(Theme::Col::Text, "Batch Size:");
                        ImGui::SameLine(Theme::Dim::FormLabelW);
                        ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                        if (ImGui::InputInt("##UnityBatchSize", &bsz, 1, 4)) {
                            if (bsz < 2) bsz = 2;
                            if (bsz > 128) bsz = 128;
                            t_edit.unity.batchSize = static_cast<size_t>(bsz);
                            t_modified = true;
                        }
                        Theme::Tooltip("Maximum source files combined into a single generated unity TU (default 16)");
                        ImGui::Spacing();

                        if (ImGui::Checkbox("Adaptive hot-file isolation", &t_edit.unity.adaptive)) {
                            t_modified = true;
                        }
                        Theme::Tooltip("Frequently edited files compile standalone instead of rebuilding the whole unity batch; graduates back after one quiet build");
                        ImGui::Spacing();

                        ImGui::TextColored(Theme::Col::TextDim, "Exclude Glob Patterns:");
                        if (RenderEditableList("UnityExcludeList", t_edit.unity.exclude, false, false, project, "Add exclude glob (e.g. src/generated/**)")) {
                            t_modified = true;
                        }

                        if (t_edit.unity.enabled) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                            std::unordered_map<std::string, const core::Target*> tmap;
                            for (const auto& t : targets) tmap[t.name] = &t;
                            bool is_msvc = (project.GetCompiler() == "cl");
                            core::EffectiveTargetSettings eff = core::ResolveEffectiveTargetSettings(project, t_edit, tmap, project.GetConfiguration(), is_msvc);
                            std::vector<std::string> srcs;
                            std::unordered_map<std::string, std::shared_ptr<const core::EffectiveFileSettings>> feffs;
                            std::unordered_map<std::string, bool> uses_pch;
                            for (const auto& sf : t_edit.expandedFiles) {
                                if (!t_edit.IsFileEnabled(sf)) continue;
                                srcs.push_back(sf);
                                feffs[sf] = std::make_shared<const core::EffectiveFileSettings>(
                                    core::ResolveFileSettings(project, t_edit, eff, sf, project.GetConfiguration(), project.GetCompiler(), false)
                                );
                            }
                            auto uplan = core::UnityManager::PlanTargetUnity(
                                project, t_edit, srcs, feffs, uses_pch, project.GetConfiguration(),
                                project.GetCompiler(), is_msvc, nullptr, false
                            );
                            ImGui::TextColored(Theme::Col::Accent, "Planned Batches: %zu · Standalone: %zu · Hot isolated: %zu",
                                uplan.batches.size(), uplan.standaloneFiles.size(), uplan.hotFiles.size());
                            for (const auto& b : uplan.batches) {
                                ImGui::BulletText("%s (%zu files%s)", b.batchName.c_str(), b.memberFiles.size(), b.isC ? ", C" : ", C++");
                            }
                        }

                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Run / Debug")) {
                        ImGui::BeginChild("TargetRunTabScroll", ImVec2(0.0f, 0.0f), false);
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::Text, "Run Command (Ctrl+R / Toolbar Run Button):");

                        char rcmd_buf[512];
                        std::strncpy(rcmd_buf, t_edit.run.command.c_str(), sizeof(rcmd_buf) - 1);
                        rcmd_buf[sizeof(rcmd_buf) - 1] = '\0';
                        ImGui::TextColored(Theme::Col::TextDim, "Command:");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputTextWithHint("##TgtRunCmd", "{bin} or qemu-system-x86_64", rcmd_buf, sizeof(rcmd_buf))) {
                            t_edit.run.command = rcmd_buf;
                            t_modified = true;
                        }
                        Theme::Tooltip("Placeholders: {bin} {bindir} {libdir} {objdir} {gen} {root} {config} {target} {objcopy} {ld}");

                        ImGui::TextColored(Theme::Col::TextDim, "Arguments:");
                        if (RenderEditableList("TargetRunArgs", t_edit.run.args, false, false, project, "Add run argument (e.g. -cdrom {bindir}/myos.iso)")) {
                            t_modified = true;
                        }

                        char rcwd_buf[512];
                        std::strncpy(rcwd_buf, t_edit.run.cwd.c_str(), sizeof(rcwd_buf) - 1);
                        rcwd_buf[sizeof(rcwd_buf) - 1] = '\0';
                        ImGui::TextColored(Theme::Col::TextDim, "Working directory:");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputTextWithHint("##TgtRunCwd", "(defaults to project root)", rcwd_buf, sizeof(rcwd_buf))) {
                            t_edit.run.cwd = rcwd_buf;
                            t_modified = true;
                        }

                        ImGui::TextColored(Theme::Col::TextDim, "Environment overrides (KEY / VALUE, supports PATH+):");
                        if (RenderKeyValueMap("TargetRunEnv", t_edit.run.env, "KEY or PATH+", "value")) {
                            t_modified = true;
                        }

                        if (ImGui::Checkbox("Build before run", &t_edit.run.buildBeforeRun)) {
                            t_modified = true;
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::TextColored(Theme::Col::Text, "Optional Debugger Command (Toolbar Debug Button):");

                        char dcmd_buf[512];
                        std::strncpy(dcmd_buf, t_edit.debug.command.c_str(), sizeof(dcmd_buf) - 1);
                        dcmd_buf[sizeof(dcmd_buf) - 1] = '\0';
                        ImGui::TextColored(Theme::Col::TextDim, "Debug command:");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputTextWithHint("##TgtDebugCmd", "e.g. qemu-system-x86_64 or gdb", dcmd_buf, sizeof(dcmd_buf))) {
                            t_edit.debug.command = dcmd_buf;
                            t_modified = true;
                        }

                        ImGui::TextColored(Theme::Col::TextDim, "Debug arguments:");
                        if (RenderEditableList("TargetDebugArgs", t_edit.debug.args, false, false, project, "Add debug argument (e.g. -s -S)")) {
                            t_modified = true;
                        }

                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                if (t_modified) {
                    project.UpdateTarget(t_edit);
                }
            }
        } else {
            // Tab bar with 5 tabs (Spec §5.1.1)
            if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
            // -----------------------------------------------------------------
            // Tab 1: General (Spec §5.1.1 & §5.1.4)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("General")) {
                ImGui::Spacing();
                ImGui::BeginChild("GeneralTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                // Configuration: Debug | Release
                ImGui::TextColored(Theme::Col::Text, "Configuration:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                std::string cur_cfg = project.GetConfiguration();
                if (ImGui::RadioButton("Debug", cur_cfg == "Debug")) {
                    project.SetConfiguration("Debug");
                }
                ImGui::SameLine(0.0f, Theme::Sp::S3);
                if (ImGui::RadioButton("Release", cur_cfg == "Release")) {
                    project.SetConfiguration("Release");
                }
                Theme::Tooltip("Debug (-O0 -g / /Od /Zi) or Release (-O2 -DNDEBUG / /O2 /MD)");

                ImGui::Spacing();

                // C++ Standard
                ImGui::TextColored(Theme::Col::Text, "C++ Standard:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* cxx_stds[] = { "c++11", "c++14", "c++17", "c++20", "c++23" };
                std::string cur_cxx = project.GetCxxStd();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##CxxStdCombo", cur_cxx.c_str())) {
                    for (const char* std_opt : cxx_stds) {
                        bool sel = (cur_cxx == std_opt);
                        if (ImGui::Selectable(std_opt, sel)) {
                            project.SetCxxStd(std_opt);
                        }
                    }
                    ImGui::EndCombo();
                }
                Theme::Tooltip("C++ language dialect standard flag (-std= or /std:)");

                ImGui::Spacing();

                // C Standard
                ImGui::TextColored(Theme::Col::Text, "C Standard:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* c_stds[] = { "c99", "c11", "c17" };
                std::string cur_c = project.GetCStd();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##CStdCombo", cur_c.c_str())) {
                    for (const char* std_opt : c_stds) {
                        bool sel = (cur_c == std_opt);
                        if (ImGui::Selectable(std_opt, sel)) {
                            project.SetCStd(std_opt);
                        }
                    }
                    ImGui::EndCombo();
                }
                Theme::Tooltip("C language dialect standard flag for .c files");

                ImGui::Spacing();

                // Warning Level
                ImGui::TextColored(Theme::Col::Text, "Warnings:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* warn_opts[] = { "Off", "-Wall", "-Wall -Wextra" };
                std::string cur_warn = project.GetWarnings();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##WarnCombo", cur_warn.c_str())) {
                    for (const char* w_opt : warn_opts) {
                        bool sel = (cur_warn == w_opt);
                        if (ImGui::Selectable(w_opt, sel)) {
                            project.SetWarnings(w_opt);
                        }
                    }
                    ImGui::EndCombo();
                }
                Theme::Tooltip("Compiler diagnostics and warning flags");

                ImGui::Spacing();

                // Parallel Jobs & RAM/Scheduler controls (V2-13)
                ImGui::TextColored(Theme::Col::Text, "Compile Jobs:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                int jobs = project.GetJobs();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::InputInt("##JobsInput", &jobs, 1, 2)) {
                    if (jobs < 0) jobs = 0;
                    if (jobs > 64) jobs = 64;
                    project.SetJobs(jobs);
                }
                Theme::Tooltip("Number of parallel worker compilation jobs (0 = auto-detect CPU cores, capped by RAM budget)");

                ImGui::Spacing();

                ImGui::TextColored(Theme::Col::Text, "RAM / Job (MB):");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                int mem_mb = project.GetMemPerJobMb();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::InputInt("##MemPerJobInput", &mem_mb, 128, 512)) {
                    if (mem_mb < 0) mem_mb = 0;
                    if (mem_mb > 16384) mem_mb = 16384;
                    project.SetMemPerJobMb(mem_mb);
                }
                Theme::Tooltip("Estimated RAM per compiler job in MB (default 512 MB; used when Compile Jobs = 0 to prevent OOM)");

                core::WorkerBudget budget = core::Scheduler::ComputeWorkerBudget(jobs, mem_mb);
                ImGui::SameLine(0.0f, Theme::Sp::S2);
                ImGui::TextColored(budget.ramCapped ? Theme::Col::Warning : Theme::Col::TextDim,
                    "%d active (%d cores, %llu MB free)%s",
                    budget.workers, budget.cores,
                    static_cast<unsigned long long>(budget.availRamMb),
                    budget.ramCapped ? " [RAM-capped]" : "");

                ImGui::Spacing();

                ImGui::TextColored(Theme::Col::Text, "Max Link Jobs:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                int link_jobs = project.GetLinkJobs();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::InputInt("##LinkJobsInput", &link_jobs, 1, 1)) {
                    if (link_jobs < 1) link_jobs = 1;
                    if (link_jobs > 16) link_jobs = 16;
                    project.SetLinkJobs(link_jobs);
                }
                Theme::Tooltip("Concurrent heavy link steps allowed (default 2)");

                ImGui::Spacing();

                bool keep_going = project.GetKeepGoing();
                if (ImGui::Checkbox("Keep going on errors", &keep_going)) {
                    project.SetKeepGoing(keep_going);
                }
                Theme::Tooltip("Continue building unrelated files/targets after an error instead of cancelling immediately");
                ImGui::SameLine(0.0f, Theme::Sp::S4);
                bool low_prio = project.GetLowPriority();
                if (ImGui::Checkbox("Low CPU priority", &low_prio)) {
                    project.SetLowPriority(low_prio);
                }
                Theme::Tooltip("Run worker subprocesses at below-normal OS priority to keep UI responsive during huge builds");

                ImGui::Spacing();

                // Paired C Compiler
                ImGui::TextColored(Theme::Col::Text, "Paired C Compiler:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                std::string cur_c_comp = project.GetCCompiler();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##CCompCombo", cur_c_comp.c_str())) {
                    for (const auto& comp : compiler_manager.GetDetectedCompilers()) {
                        if (comp.IsCCompatible()) {
                            bool sel = (cur_c_comp == comp.name);
                            if (ImGui::Selectable(comp.name.c_str(), sel)) {
                                project.SetCCompiler(comp.name);
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                Theme::Tooltip("C compiler used for .c source files in mixed-language projects (§5.1.6)");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // V2-19 compile_commands.json settings
                ImGui::TextColored(Theme::Col::Text, "compile_commands.json (clangd / IDE Integration):");
                core::CompileCommandsConfig cc_cfg = project.GetCompileCommandsConfig();
                bool cc_mod = false;
                if (ImGui::Checkbox("Auto-export compile_commands.json after build plan", &cc_cfg.enabled)) {
                    cc_mod = true;
                }
                if (cc_cfg.enabled) {
                    char ccp_buf[260];
                    std::strncpy(ccp_buf, cc_cfg.path.c_str(), sizeof(ccp_buf) - 1);
                    ccp_buf[sizeof(ccp_buf) - 1] = '\0';
                    ImGui::TextColored(Theme::Col::TextDim, "Output path:");
                    ImGui::SameLine(Theme::Dim::FormLabelW);
                    ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                    if (ImGui::InputText("##CompDbPath", ccp_buf, sizeof(ccp_buf))) {
                        cc_cfg.path = ccp_buf;
                        cc_mod = true;
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (ImGui::Checkbox("All configs", &cc_cfg.allConfigs)) {
                        cc_mod = true;
                    }
                }
                if (cc_mod) {
                    project.SetCompileCommandsConfig(cc_cfg);
                }
                static std::string s_GenCompDbStatus;
                if (Theme::Button("Export compile_commands.json now", "Write compile_commands.json without building", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                    auto er = core::CompDbManager::ExportCompileCommands(project, compiler_manager);
                    if (er.success) {
                        s_GenCompDbStatus = "Exported " + std::to_string(er.entryCount) + " entries" + (er.fileModified ? "." : " (up to date).");
                    } else {
                        s_GenCompDbStatus = "Failed: " + er.error;
                    }
                }
                if (!s_GenCompDbStatus.empty()) {
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    ImGui::TextColored(Theme::Col::Accent, "%s", s_GenCompDbStatus.c_str());
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // V2-21 / V2-22 / V2-23 IDE Preferences & System Status
                ImGui::TextColored(Theme::Col::Text, "IDE & Scale Preferences:");
                static core::UserSettings s_UserPrefs;
                static bool s_UserPrefsLoaded = false;
                if (!s_UserPrefsLoaded) {
                    s_UserPrefs = core::HintsManager::LoadUserSettings();
                    s_UserPrefsLoaded = true;
                }
                bool up_changed = false;
                char ed_buf[260];
                std::strncpy(ed_buf, s_UserPrefs.editorCommand.c_str(), sizeof(ed_buf) - 1);
                ed_buf[sizeof(ed_buf) - 1] = '\0';
                ImGui::TextColored(Theme::Col::TextDim, "Editor command:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                ImGui::SetNextItemWidth(260.0f * dpi);
                if (ImGui::InputTextWithHint("##EditorCmdInput", "code -g {file}:{line}:{col}", ed_buf, sizeof(ed_buf))) {
                    s_UserPrefs.editorCommand = ed_buf;
                    up_changed = true;
                }
                Theme::Tooltip("Launched when clicking error/warning diagnostics (e.g. code -g {file}:{line}:{col} or notepad++ -n{line} {file})");
                if (ImGui::Checkbox("Show performance hints (Defender / Dev Drive)", &s_UserPrefs.showPerformanceHints)) {
                    up_changed = true;
                }
                if (up_changed) {
                    core::HintsManager::SaveUserSettings(s_UserPrefs);
                }

                bool lp_enabled = core::fs_util::IsWindowsLongPathsEnabled();
                ImGui::TextColored(lp_enabled ? Theme::Col::Success : Theme::Col::Warning,
                    "Windows Long Paths (>260 chars): %s",
                    lp_enabled ? "Enabled (LongPathsEnabled=1)" : "Disabled (short object hashing active)");

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 2: Include Dirs (Spec §5.1.1 & §5.1.3)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Include Dirs")) {
                ImGui::Spacing();
                ImGui::BeginChild("IncludeTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                ImGui::TextColored(Theme::Col::TextDim, "Additional header include search directories (-I or /I):");
                ImGui::TextColored(Theme::Col::Text, "Note: '.' (project root) and 'include/' are automatically included.");
                ImGui::Spacing();

                auto incs = project.GetIncludeDirs();
                if (RenderEditableList("IncludeDirsList", incs, true, true, project, "Add include directory")) {
                    project.SetIncludeDirs(incs);
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 3: Libraries (Spec §5.1.1 & §5.1.2)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Libraries")) {
                ImGui::Spacing();
                ImGui::BeginChild("LibsTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                // Presets Dropdown (§5.1.2)
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Theme::Col::Text, "Library Presets:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Theme::Dim::FormLabelW);
                if (ImGui::BeginCombo("##PresetsCombo", "Apply Preset... ▾")) {
                    // OpenGL Preset
                    if (ImGui::Selectable("OpenGL (opengl32)")) {
                        auto libs = project.GetLibs();
                        if (std::find(libs.begin(), libs.end(), "opengl32") == libs.end()) {
                            libs.push_back("opengl32");
                            project.SetLibs(libs);
                        }
                    }
                    Theme::Tooltip("Adds opengl32 system library");

                    // SDL2 / SDL3 Preset
                    if (ImGui::Selectable("SDL2 / SDL3...")) {
                        nfdchar_t* sdl_folder = nullptr;
                        if (NFD_PickFolder(&sdl_folder, nullptr) == NFD_OKAY && sdl_folder != nullptr) {
                            std::filesystem::path sdl_p(sdl_folder);
                            std::error_code ec;

                            // 1. Include dir: <root>/include
                            std::filesystem::path inc_candidate = sdl_p / "include";
                            if (std::filesystem::exists(inc_candidate, ec)) {
                                auto incs = project.GetIncludeDirs();
                                std::string inc_s = MakeRelativeIfInside(inc_candidate, project.GetRootPath());
                                if (std::find(incs.begin(), incs.end(), inc_s) == incs.end()) {
                                    incs.push_back(inc_s);
                                    project.SetIncludeDirs(incs);
                                }
                            }

                            // 2. Lib dir: <root>/lib/x64 or <root>/lib
                            std::filesystem::path lib_candidate = sdl_p / "lib" / "x64";
                            if (!std::filesystem::exists(lib_candidate, ec)) {
                                lib_candidate = sdl_p / "lib";
                            }
                            if (std::filesystem::exists(lib_candidate, ec)) {
                                auto lib_dirs = project.GetLibDirs();
                                std::string lib_s = MakeRelativeIfInside(lib_candidate, project.GetRootPath());
                                if (std::find(lib_dirs.begin(), lib_dirs.end(), lib_s) == lib_dirs.end()) {
                                    lib_dirs.push_back(lib_s);
                                    project.SetLibDirs(lib_dirs);
                                }
                            }

                            // 3. Libs: mingw32, SDL2main, SDL2 (or SDL2main, SDL2 for MSVC)
                            auto libs = project.GetLibs();
                            std::string comp_name = project.GetCompiler();
                            if (comp_name == "cl" || comp_name == "cl.exe") {
                                for (const char* l : { "SDL2main", "SDL2" }) {
                                    if (std::find(libs.begin(), libs.end(), l) == libs.end()) libs.push_back(l);
                                }
                            } else {
                                for (const char* l : { "mingw32", "SDL2main", "SDL2" }) {
                                    if (std::find(libs.begin(), libs.end(), l) == libs.end()) libs.push_back(l);
                                }
                            }
                            project.SetLibs(libs);

                            // 4. Copy DLL: search for SDL2.dll
                            std::filesystem::path dll_cand = sdl_p / "bin" / "SDL2.dll";
                            if (!std::filesystem::exists(dll_cand, ec)) {
                                dll_cand = sdl_p / "lib" / "x64" / "SDL2.dll";
                            }
                            if (std::filesystem::exists(dll_cand, ec)) {
                                auto dlls = project.GetCopyDlls();
                                std::string dll_s = MakeRelativeIfInside(dll_cand, project.GetRootPath());
                                if (std::find(dlls.begin(), dlls.end(), dll_s) == dlls.end()) {
                                    dlls.push_back(dll_s);
                                    project.SetCopyDlls(dlls);
                                }
                            }
                            NFD_FreePath(sdl_folder);
                        }
                    }
                    Theme::Tooltip("Pick SDL root folder to automatically configure include, lib, and DLL copying");

                    // Boost Preset
                    if (ImGui::Selectable("Boost...")) {
                        nfdchar_t* boost_folder = nullptr;
                        if (NFD_PickFolder(&boost_folder, nullptr) == NFD_OKAY && boost_folder != nullptr) {
                            std::filesystem::path boost_p(boost_folder);
                            std::error_code ec;
                            auto incs = project.GetIncludeDirs();
                            std::string inc_s = MakeRelativeIfInside(boost_p, project.GetRootPath());
                            if (std::find(incs.begin(), incs.end(), inc_s) == incs.end()) {
                                incs.push_back(inc_s);
                                project.SetIncludeDirs(incs);
                            }
                            std::filesystem::path stage_lib = boost_p / "stage" / "lib";
                            if (std::filesystem::exists(stage_lib, ec)) {
                                auto lib_dirs = project.GetLibDirs();
                                std::string stage_s = MakeRelativeIfInside(stage_lib, project.GetRootPath());
                                if (std::find(lib_dirs.begin(), lib_dirs.end(), stage_s) == lib_dirs.end()) {
                                    lib_dirs.push_back(stage_s);
                                    project.SetLibDirs(lib_dirs);
                                }
                            }
                            NFD_FreePath(boost_folder);
                        }
                    }
                    Theme::Tooltip("Pick Boost root folder to configure header includes and stage library directory");

                    ImGui::EndCombo();
                }

                // pkg-config Import Row (§5.1.2)
                ImGui::SameLine(0.0f, Theme::Sp::S3);
                ImGui::SetNextItemWidth(Theme::Dim::FormSmallInputW);
                ImGui::InputTextWithHint("##PkgConfigInput", "pkg-config name...", s_PkgConfigInput, sizeof(s_PkgConfigInput));

                ImGui::SameLine(0.0f, Theme::Sp::S1);
                if (Theme::Button("Import##pkg", "Query pkg-config and add its flags")) {
                    RunPkgConfig(s_PkgConfigInput, project, s_PkgConfigStatus);
                }

                if (!s_PkgConfigStatus.empty()) {
                    ImGui::TextColored(Theme::Col::Accent, "%s", s_PkgConfigStatus.c_str());
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // 1. Library Search Dirs
                ImGui::TextColored(Theme::Col::TextDim, "Library Search Directories (-L or /LIBPATH:):");
                auto lib_dirs = project.GetLibDirs();
                if (RenderEditableList("LibDirsList", lib_dirs, true, true, project, "Add library search directory")) {
                    project.SetLibDirs(lib_dirs);
                }

                ImGui::Spacing();

                // 2. Library Names (with optional V2-5 condition)
                ImGui::TextColored(Theme::Col::TextDim, "Libraries (-l<name> or <name>.lib) & optional 'if' conditions:");
                auto libs = project.GetLibs();
                auto lib_conds = project.GetLibConditions();
                if (RenderConditionalLibsList("LibsList", libs, lib_conds, project, "Add library name (e.g. SDL2, opengl32)")) {
                    project.SetLibs(libs);
                    project.SetLibConditions(lib_conds);
                }

                ImGui::Spacing();

                // 3. DLLs to Copy
                ImGui::TextColored(Theme::Col::TextDim, "DLLs to copy next to executable after link (Spec §5.1.2):");
                auto dlls = project.GetCopyDlls();
                if (RenderEditableList("CopyDllsList", dlls, true, false, project, "Add DLL file to copy")) {
                    project.SetCopyDlls(dlls);
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 4: Defines (Spec §5.1.1 & §5.1.3)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Defines")) {
                ImGui::Spacing();
                ImGui::BeginChild("DefinesTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                ImGui::TextColored(Theme::Col::TextDim, "Preprocessor Definitions (NAME or NAME=VALUE):");
                ImGui::Spacing();

                auto defs = project.GetDefines();
                if (RenderEditableList("DefinesList", defs, false, false, project, "Add define (e.g. SDL_MAIN_HANDLED or DEBUG=1)")) {
                    project.SetDefines(defs);
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 5: Advanced (Spec §5.1.1 & §5.1.3)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Advanced")) {
                ImGui::Spacing();
                ImGui::BeginChild("AdvTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);
                std::string cur_cfg = project.GetConfiguration();
                core::ConfigOverrides cfg_overrides = project.GetConfigOverrides(cur_cfg);

                // Raw Compile Flags
                ImGui::TextColored(Theme::Col::TextDim, "Global Raw Compile Flags:");
                auto comp_flags = project.GetCompileFlags();
                if (RenderEditableList("CompFlagsList", comp_flags, false, false, project, "Add raw compiler flag")) {
                    project.SetCompileFlags(comp_flags);
                }

                ImGui::Spacing();

                // Per-Configuration Compile Flags
                ImGui::TextColored(Theme::Col::TextDim, "Per-Configuration (%s) Compile Flags:", cur_cfg.c_str());
                if (RenderEditableList("CfgCompFlagsList", cfg_overrides.compileFlags, false, false, project, "Add config compiler flag")) {
                    project.SetConfigOverrides(cur_cfg, cfg_overrides);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Raw Link Flags
                ImGui::TextColored(Theme::Col::TextDim, "Global Raw Linker Flags:");
                auto link_flags = project.GetLinkFlags();
                if (RenderEditableList("LinkFlagsList", link_flags, false, false, project, "Add raw linker flag")) {
                    project.SetLinkFlags(link_flags);
                }

                ImGui::Spacing();

                // Per-Configuration Link Flags
                ImGui::TextColored(Theme::Col::TextDim, "Per-Configuration (%s) Linker Flags:", cur_cfg.c_str());
                if (RenderEditableList("CfgLinkFlagsList", cfg_overrides.linkFlags, false, false, project, "Add config linker flag")) {
                    project.SetConfigOverrides(cur_cfg, cfg_overrides);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Theme::Col::TextDim, "Binary Dependency Log (deps.bin):");
                bool ign_sys = project.GetIgnoreSystemHeaders();
                if (ImGui::Checkbox("Ignore system / toolchain headers in dependency tracking", &ign_sys)) {
                    project.SetIgnoreSystemHeaders(ign_sys);
                }
                Theme::Tooltip("Skip stat() checks on standard library / SDK headers (faster no-op builds)");
                bool keep_d = project.GetKeepDepFiles();
                if (ImGui::Checkbox("Keep raw .d depfiles after importing into deps.bin", &keep_d)) {
                    project.SetKeepDepFiles(keep_d);
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 6: Tools (V2-7)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Tools")) {
                ImGui::Spacing();
                ImGui::BeginChild("ToolsTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                ImGui::TextColored(Theme::Col::Text, "Assembler (.asm):");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* asm_modes[] = { "auto", "nasm", "ml64" };
                std::string cur_asm = project.GetAsmMode();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##AsmModeCombo", cur_asm.c_str())) {
                    for (const char* am : asm_modes) {
                        if (ImGui::Selectable(am, cur_asm == am)) {
                            project.SetAsmMode(am);
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                auto tool_overrides = project.GetToolOverrides();
                auto detected_tools = core::ToolRegistry::DetectTools(tool_overrides, false);
                ImGui::TextColored(Theme::Col::TextDim, "Built-in Extra Language Tools (.rc, .S/.asm, .cu, .ispc):");
                if (ImGui::BeginTable("ToolsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                    ImGui::TableSetupColumn("Detected Path", ImGuiTableColumnFlags_WidthStretch, 0.44f);
                    ImGui::TableSetupColumn("Override Executable Path", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                    ImGui::TableHeadersRow();
                    for (const auto& dt : detected_tools) {
                        ImGui::TableNextRow();
                        ImGui::PushID(dt.id.c_str());
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(dt.found ? Theme::Col::Success : Theme::Col::TextDim, "%s", dt.id.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextColored(dt.found ? Theme::Col::Text : Theme::Col::Warning, "%s",
                            dt.found ? dt.path.c_str() : "not found on PATH");
                        ImGui::TableSetColumnIndex(2);
                        char obuf[512];
                        std::string ov_val = (tool_overrides.count(dt.id) ? tool_overrides[dt.id] : "");
                        std::strncpy(obuf, ov_val.c_str(), sizeof(obuf) - 1);
                        obuf[sizeof(obuf) - 1] = '\0';
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##ToolOv", obuf, sizeof(obuf))) {
                            if (obuf[0] == '\0') tool_overrides.erase(dt.id);
                            else tool_overrides[dt.id] = obuf;
                            project.SetToolOverrides(tool_overrides);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Theme::Col::TextDim, "Custom Extension Tools:");
                auto ctools = project.GetCustomTools();
                bool ct_changed = false;
                int remove_ct = -1;
                for (size_t ci = 0; ci < ctools.size(); ++ci) {
                    ImGui::PushID(static_cast<int>(ci));
                    auto& ct = ctools[ci];
                    char ebuf[64], pbuf[256], abuf[256], obuf[256];
                    std::strncpy(ebuf, ct.ext.c_str(), sizeof(ebuf) - 1); ebuf[sizeof(ebuf) - 1] = '\0';
                    std::strncpy(pbuf, ct.program.c_str(), sizeof(pbuf) - 1); pbuf[sizeof(pbuf) - 1] = '\0';
                    std::strncpy(abuf, ct.args.c_str(), sizeof(abuf) - 1); abuf[sizeof(abuf) - 1] = '\0';
                    std::strncpy(obuf, ct.output.c_str(), sizeof(obuf) - 1); obuf[sizeof(obuf) - 1] = '\0';
                    ImGui::SetNextItemWidth(70.0f * dpi);
                    if (ImGui::InputText("##ct_ext", ebuf, sizeof(ebuf))) { ct.ext = ebuf; ct_changed = true; }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f * dpi);
                    if (ImGui::InputText("##ct_prog", pbuf, sizeof(pbuf))) { ct.program = pbuf; ct_changed = true; }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(170.0f * dpi);
                    if (ImGui::InputText("##ct_args", abuf, sizeof(abuf))) { ct.args = abuf; ct_changed = true; }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f * dpi);
                    if (ImGui::InputText("##ct_out", obuf, sizeof(obuf))) { ct.output = obuf; ct_changed = true; }
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Link", &ct.link)) { ct_changed = true; }
                    ImGui::SameLine();
                    if (Theme::DangerIconButton("##ct_del", ICON_FA_XMARK, "Remove custom tool", ImVec2(Theme::Btn::DelW, Theme::Btn::DefaultH))) {
                        remove_ct = static_cast<int>(ci);
                    }
                    ImGui::PopID();
                }
                if (remove_ct >= 0) {
                    ctools.erase(ctools.begin() + remove_ct);
                    ct_changed = true;
                }
                if (Theme::Button("+ Add Custom Tool", "Add extension -> custom compiler command rule", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                    core::CustomToolSpec nt;
                    nt.ext = ".custom";
                    nt.program = "tool";
                    nt.args = "{in} -o {out}";
                    nt.output = "{objdir}/{name}.out";
                    nt.link = false;
                    ctools.push_back(std::move(nt));
                    ct_changed = true;
                }
                if (ct_changed) project.SetCustomTools(ctools);

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 7: Linker (V2-12)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Linker")) {
                ImGui::Spacing();
                ImGui::BeginChild("LinkerTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                std::string comp_low = project.GetCompiler();
                std::transform(comp_low.begin(), comp_low.end(), comp_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");

                ImGui::TextColored(Theme::Col::Text, "Linker:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* linkers[] = { "auto", "lld", "mold", "gold", "default" };
                std::string cur_lnk = project.GetLinker();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##LinkerCombo", cur_lnk.c_str())) {
                    for (const char* lm : linkers) {
                        bool avail = core::CompilerManager::IsLinkerAvailable(lm, is_msvc);
                        std::string lbl = std::string(lm) + (avail ? "  (available)" : "  (not on PATH)");
                        if (ImGui::Selectable(lbl.c_str(), cur_lnk == lm)) {
                            project.SetLinker(lm);
                        }
                    }
                    ImGui::EndCombo();
                }
                bool cur_avail = core::CompilerManager::IsLinkerAvailable(cur_lnk, is_msvc);
                ImGui::SameLine(0.0f, Theme::Sp::S2);
                ImGui::TextColored(cur_avail ? Theme::Col::Success : Theme::Col::Warning,
                    cur_avail ? ICON_FA_CHECK " Ready" : ICON_FA_TRIANGLE_EXCLAMATION " Not detected on PATH");

                ImGui::Spacing();
                bool inc_link = project.GetIncrementalLink();
                if (ImGui::Checkbox("Incremental linking (MSVC Debug /INCREMENTAL)", &inc_link)) {
                    project.SetIncrementalLink(inc_link);
                }

                ImGui::Spacing();
                ImGui::TextColored(Theme::Col::Text, "Debug Link Mode:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* dbg_modes[] = { "default", "fastlink", "split-dwarf" };
                std::string cur_dlink = project.GetDebugLink();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##DebugLinkCombo", cur_dlink.c_str())) {
                    for (const char* dm : dbg_modes) {
                        if (ImGui::Selectable(dm, cur_dlink == dm)) {
                            project.SetDebugLink(dm);
                            if (std::string(dm) == "split-dwarf") project.SetSplitDwarf(true);
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::Spacing();
                bool split_dw = project.GetSplitDwarf();
                if (ImGui::Checkbox("Split DWARF (-gsplit-dwarf + --gdb-index on GCC/Clang)", &split_dw)) {
                    project.SetSplitDwarf(split_dw);
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // -----------------------------------------------------------------
            // Tab 8: Compiler Cache (V2-10, user-level ~/.AppData/GenieBuild/cache.json)
            // -----------------------------------------------------------------
            if (ImGui::BeginTabItem("Cache")) {
                ImGui::Spacing();
                ImGui::BeginChild("CacheTabScroll", ImVec2(0.0f, tab_scroll_h - ImGui::GetFrameHeightWithSpacing()), false);

                static bool s_CachePrefsLoaded = false;
                static core::CompilerCachePrefs s_CachePrefs;
                static std::string s_CacheClearMsg;
                static bool s_ConfirmCacheClear = false;
                if (!s_CachePrefsLoaded) {
                    s_CachePrefs = core::CompilerCacheManager::LoadPrefs();
                    s_CachePrefsLoaded = true;
                }

                std::string comp_low = project.GetCompiler();
                std::transform(comp_low.begin(), comp_low.end(), comp_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
                core::DetectedCompilerCache detected = core::CompilerCacheManager::DetectCache(s_CachePrefs, is_msvc, false);

                ImGui::TextColored(Theme::Col::TextDim, "Saved per-user in %s",
                    core::CompilerCacheManager::GetPrefsFilePath().generic_u8string().c_str());
                ImGui::Spacing();

                ImGui::TextColored(Theme::Col::Text, "Launcher:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                const char* cache_opts[] = { "auto", "ccache", "sccache", "none" };
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::BeginCombo("##CacheLauncherCombo", s_CachePrefs.launcher.c_str())) {
                    for (const char* co : cache_opts) {
                        if (ImGui::Selectable(co, s_CachePrefs.launcher == co)) {
                            s_CachePrefs.launcher = co;
                            core::CompilerCacheManager::SavePrefs(s_CachePrefs);
                            s_CacheClearMsg.clear();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::Spacing();
                ImGui::TextColored(Theme::Col::Text, "Max Cache Size:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                char sz_buf[64];
                std::strncpy(sz_buf, s_CachePrefs.maxSize.c_str(), sizeof(sz_buf) - 1);
                sz_buf[sizeof(sz_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::InputText("##CacheMaxSize", sz_buf, sizeof(sz_buf))) {
                    s_CachePrefs.maxSize = sz_buf;
                    core::CompilerCacheManager::SavePrefs(s_CachePrefs);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (detected.active) {
                    ImGui::TextColored(Theme::Col::Success, ICON_FA_CHECK " Active: %s (%s)", detected.name.c_str(), detected.executablePath.c_str());
                } else {
                    ImGui::TextColored(Theme::Col::TextDim, "Status: inactive (mode=%s; install ccache or sccache on PATH to enable)", s_CachePrefs.launcher.c_str());
                }

                ImGui::Spacing();
                if (!s_ConfirmCacheClear) {
                    if (Theme::Button("Clear Cache...", "Clear compiler cache entries/stats", false, ImVec2(0.0f, Theme::Btn::DefaultH)) && detected.active) {
                        s_ConfirmCacheClear = true;
                        s_CacheClearMsg.clear();
                    }
                } else {
                    ImGui::TextColored(Theme::Col::Warning, "Confirm clearing %s cache?", detected.name.c_str());
                    ImGui::SameLine();
                    if (Theme::Button("Confirm Clear", "Execute cache clear", true, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        core::CompilerCacheManager::ClearCache(detected, s_CacheClearMsg);
                        s_ConfirmCacheClear = false;
                    }
                    ImGui::SameLine();
                    if (Theme::Button("Cancel", "Abort", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_ConfirmCacheClear = false;
                    }
                }
                if (!s_CacheClearMsg.empty()) {
                    ImGui::TextColored(Theme::Col::TextDim, "%s", s_CacheClearMsg.c_str());
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        } // end scope else

        // ---------------------------------------------------------------------
        // Read-only Live Command Preview at Bottom of Modal (Spec §5.1.1)
        // ---------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(Theme::Col::Text, "Live Command Preview:");

        const core::CompilerInfo* comp_info = compiler_manager.FindCompiler(project.GetCompiler());
        if (comp_info) {
            const core::Target* preview_target = !s_SelectedTargetScope.empty()
                ? project.FindTarget(s_SelectedTargetScope)
                : project.GetPrimaryTarget();
            std::string sample_src = "src/main.cpp";
            if (!s_SelectedFileScope.empty()) {
                sample_src = s_SelectedFileScope;
            } else if (preview_target && !preview_target->expandedFiles.empty()) {
                sample_src = preview_target->expandedFiles[0];
            } else if (!project.GetFiles().empty()) {
                sample_src = project.GetFiles()[0];
            }
            std::string config = project.GetConfiguration();
            std::filesystem::path obj_path = preview_target
                ? (preview_target->GetObjectDir(project.GetRootPath(), config) / (sample_src + ".o"))
                : (project.GetObjectDir(config) / (sample_src + ".o"));

            std::string comp_low = project.GetCompiler();
            std::transform(comp_low.begin(), comp_low.end(), comp_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");

            core::EffectiveTargetSettings eff;
            core::EffectiveFileSettings file_eff;
            if (preview_target) {
                std::unordered_map<std::string, const core::Target*> tmap;
                for (const auto& t : project.GetTargets()) tmap[t.name] = &t;
                eff = core::ResolveEffectiveTargetSettings(project, *preview_target, tmap, config, is_msvc);
                file_eff = core::ResolveFileSettings(project, *preview_target, eff, sample_src, config, project.GetCompiler(), false);
            }

            core::BuildCommand compile_cmd = preview_target
                ? core::CompilerManager::FormulateCompileCommand(*comp_info, sample_src, obj_path, project, file_eff)
                : core::CompilerManager::FormulateCompileCommand(*comp_info, sample_src, obj_path, project);

            std::filesystem::path bin_path = preview_target
                ? preview_target->GetBinaryPath(project.GetAbsoluteOutputDir(), config, is_msvc)
                : project.GetBinaryPath(config);
            core::BuildCommand link_cmd;
            if (preview_target && preview_target->kind == core::TargetKind::Interface) {
                link_cmd.display_command = "(interface target: header-only, no archive/link step)";
            } else if (preview_target) {
                link_cmd = core::CompilerManager::FormulateLinkCommand(*comp_info, *preview_target, eff, { obj_path }, bin_path, project, false);
            } else {
                link_cmd = core::CompilerManager::FormulateLinkCommand(*comp_info, { obj_path }, bin_path, project, false);
            }

            ImGui::PushFont(g_Fonts.mono);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Col::BgOutput);
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Col::Border);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * Theme::DpiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Sp::S3, Theme::Sp::S2));
            if (ImGui::BeginChild("CmdPreviewBox", ImVec2(0.0f, Theme::Dim::CmdBoxH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar)) {
                ImGui::TextColored(Theme::Col::TextDim, "Compile: ");
                ImGui::SameLine();
                ImGui::TextUnformatted(compile_cmd.display_command.c_str());

                ImGui::TextColored(Theme::Col::TextDim, "Link:    ");
                ImGui::SameLine();
                ImGui::TextUnformatted(link_cmd.display_command.c_str());
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            ImGui::PopFont();
        }

        // Close button at bottom right
        ImGui::Spacing();
        const float close_btn_w = Theme::Btn::CloseW;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - close_btn_w);

        if (Theme::Button("Close", "Save settings to project.json and close modal", true, ImVec2(close_btn_w, Theme::Btn::DefaultH))) {
            project.Save();
            s_ShowSettingsModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndChild(); // SettingsModalBody
        ImGui::PopStyleVar(); // WindowPadding

        ImGui::EndPopup();
    } else {
        s_ShowSettingsModal = false;
    }

    // -------------------------------------------------------------------------
    // V2-4 Manage Configurations Dialog
    // -------------------------------------------------------------------------
    if (s_ShowManageConfigsModal) {
        ImGui::OpenPopup("Manage Configurations##Modal");
    }
    bool mc_close = false;
    if (Theme::BeginModal("Manage Configurations##Modal", "Manage Configurations", ICON_FA_GEAR, Theme::Col::Accent, 680.0f, &mc_close)) {
        const float dpi = Theme::DpiScale;
        auto cfg_names = project.GetConfigNames();
        static std::string s_SelectedCfg = "";
        static char s_NewCfgName[64] = "";
        static char s_RenameCfgBuf[64] = "";
        static std::string s_CfgErr = "";

        if (s_SelectedCfg.empty() || std::find(cfg_names.begin(), cfg_names.end(), s_SelectedCfg) == cfg_names.end()) {
            s_SelectedCfg = project.GetConfiguration();
            if (std::find(cfg_names.begin(), cfg_names.end(), s_SelectedCfg) == cfg_names.end() && !cfg_names.empty()) {
                s_SelectedCfg = cfg_names[0];
            }
            std::strncpy(s_RenameCfgBuf, s_SelectedCfg.c_str(), sizeof(s_RenameCfgBuf) - 1);
            s_RenameCfgBuf[sizeof(s_RenameCfgBuf) - 1] = '\0';
        }

        // Left pane: configuration list + Add / Duplicate / Delete
        ImGui::BeginChild("CfgListPane", ImVec2(190.0f * dpi, 340.0f * dpi), true);
        for (const auto& cname : cfg_names) {
            bool is_active = (cname == project.GetConfiguration());
            std::string lbl = is_active ? (cname + " (active)") : cname;
            if (ImGui::Selectable(lbl.c_str(), s_SelectedCfg == cname)) {
                s_SelectedCfg = cname;
                std::strncpy(s_RenameCfgBuf, cname.c_str(), sizeof(s_RenameCfgBuf) - 1);
                s_RenameCfgBuf[sizeof(s_RenameCfgBuf) - 1] = '\0';
                s_CfgErr.clear();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##NewCfgName", "New config name...", s_NewCfgName, sizeof(s_NewCfgName));
        if (Theme::Button("+ Add", "Create new configuration from Release preset", false, ImVec2(85.0f * dpi, Theme::Btn::SmallH))) {
            core::ConfigOverrides nc;
            nc.base = "relWithDebInfo";
            if (project.AddConfiguration(s_NewCfgName, nc, s_CfgErr)) {
                s_SelectedCfg = s_NewCfgName;
                std::strncpy(s_RenameCfgBuf, s_SelectedCfg.c_str(), sizeof(s_RenameCfgBuf) - 1);
                s_NewCfgName[0] = '\0';
            }
        }
        ImGui::SameLine();
        if (Theme::Button("Duplicate", "Duplicate selected configuration", false, ImVec2(85.0f * dpi, Theme::Btn::SmallH))) {
            std::string new_name = (s_NewCfgName[0] != '\0') ? std::string(s_NewCfgName) : (s_SelectedCfg + "_Copy");
            if (project.DuplicateConfiguration(s_SelectedCfg, new_name, s_CfgErr)) {
                s_SelectedCfg = new_name;
                std::strncpy(s_RenameCfgBuf, s_SelectedCfg.c_str(), sizeof(s_RenameCfgBuf) - 1);
                s_NewCfgName[0] = '\0';
            }
        }

        const bool can_delete = (cfg_names.size() > 1) && (s_SelectedCfg != project.GetConfiguration());
        if (!can_delete) ImGui::BeginDisabled(true);
        if (Theme::Button("Delete", nullptr, false, ImVec2(-1, Theme::Btn::SmallH))) {
            if (project.DeleteConfiguration(s_SelectedCfg, s_CfgErr)) {
                s_SelectedCfg.clear();
            }
        }
        if (!can_delete) ImGui::EndDisabled();
        if (!can_delete) {
            Theme::Tooltip(cfg_names.size() <= 1
                ? "Cannot delete the last remaining configuration."
                : "Cannot delete the active configuration. Switch active configuration first.");
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, Theme::Sp::S3);

        // Right pane: Base preset + overrides + resulting flags preview
        ImGui::BeginChild("CfgEditPane", ImVec2(0.0f, 340.0f * dpi), true);
        if (!s_SelectedCfg.empty()) {
            core::ConfigOverrides cfg_ov = project.GetConfigOverrides(s_SelectedCfg);
            bool cfg_mod = false;

            ImGui::TextColored(Theme::Col::Text, "Name:");
            ImGui::SameLine(130.0f * dpi);
            ImGui::SetNextItemWidth(160.0f * dpi);
            ImGui::InputText("##CfgRen", s_RenameCfgBuf, sizeof(s_RenameCfgBuf));
            ImGui::SameLine();
            if (Theme::Button("Rename", "Rename configuration across project & targets", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
                if (project.RenameConfiguration(s_SelectedCfg, s_RenameCfgBuf, s_CfgErr)) {
                    s_SelectedCfg = s_RenameCfgBuf;
                }
            }

            if (s_SelectedCfg != project.GetConfiguration()) {
                ImGui::SameLine();
                if (Theme::SuccessButton("Set Active", "Switch active build configuration", ImVec2(0.0f, Theme::Btn::DefaultH))) {
                    project.SetConfiguration(s_SelectedCfg);
                }
            }

            ImGui::Spacing();
            ImGui::TextColored(Theme::Col::Text, "Base Preset:");
            ImGui::SameLine(130.0f * dpi);
            const char* bases[] = { "debug", "release", "relWithDebInfo", "minSize" };
            std::string cur_base = cfg_ov.base.empty()
                ? core::BaseConfigPresetToString(core::BaseConfigPresetFromString("", s_SelectedCfg))
                : cfg_ov.base;
            ImGui::SetNextItemWidth(160.0f * dpi);
            if (ImGui::BeginCombo("##CfgBase", cur_base.c_str())) {
                for (const char* b : bases) {
                    if (ImGui::Selectable(b, cur_base == b)) {
                        cfg_ov.base = b;
                        cfg_mod = true;
                    }
                }
                ImGui::EndCombo();
            }

            auto render_combo = [&](const char* lbl, const char* id, std::string& val, const char* const* items, int count) {
                ImGui::TextColored(Theme::Col::Text, "%s", lbl);
                ImGui::SameLine(130.0f * dpi);
                std::string prev = val.empty() ? "(preset)" : val;
                ImGui::SetNextItemWidth(160.0f * dpi);
                if (ImGui::BeginCombo(id, prev.c_str())) {
                    for (int i = 0; i < count; ++i) {
                        std::string opt = (i == 0) ? "" : items[i];
                        if (ImGui::Selectable(items[i], val == opt)) {
                            val = opt;
                            cfg_mod = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            };

            const char* opt_items[] = { "(preset)", "O0", "O1", "O2", "O3", "Os" };
            render_combo("Optimize:", "##CfgOpt", cfg_ov.optimize, opt_items, 6);
            const char* dbg_items[] = { "(preset)", "none", "line", "full" };
            render_combo("Debug Info:", "##CfgDbg", cfg_ov.debugInfo, dbg_items, 4);
            const char* rt_items[] = { "(preset)", "dynamic", "static" };
            render_combo("MSVC Runtime:", "##CfgRt", cfg_ov.runtime, rt_items, 3);
            const char* lto_items[] = { "(preset)", "off", "thin", "full" };
            render_combo("LTO:", "##CfgLto", cfg_ov.lto, lto_items, 4);

            ImGui::Spacing();
            ImGui::TextColored(Theme::Col::TextDim, "Config Defines:");
            if (RenderEditableList("McDefs", cfg_ov.defines, false, false, project, "Add define")) cfg_mod = true;
            ImGui::TextColored(Theme::Col::TextDim, "Config Compile Flags:");
            if (RenderEditableList("McCFlags", cfg_ov.compileFlags, false, false, project, "Add compile flag")) cfg_mod = true;
            ImGui::TextColored(Theme::Col::TextDim, "Config Link Flags:");
            if (RenderEditableList("McLFlags", cfg_ov.linkFlags, false, false, project, "Add link flag")) cfg_mod = true;

            if (cfg_mod) {
                project.SetConfigOverrides(s_SelectedCfg, cfg_ov);
            }

            // Resulting flags preview
            ImGui::Spacing();
            ImGui::Separator();
            core::ResolvedConfigParams rp = core::ResolveConfigParams(project, s_SelectedCfg);
            std::string comp_low = project.GetCompiler();
            std::transform(comp_low.begin(), comp_low.end(), comp_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
            bool is_clang = (comp_low.find("clang") != std::string::npos);
            auto cf = core::GetConfigCompileFlags(rp, "", "", is_msvc, is_clang);
            auto lf = core::GetConfigLinkFlags(rp, core::TargetKind::Executable, is_msvc, is_clang);
            std::string preview_str = "Compile:";
            for (const auto& f : cf) preview_str += " " + f;
            if (rp.defineDebug) preview_str += is_msvc ? " /D_DEBUG" : " -D_DEBUG";
            if (rp.defineNdebug) preview_str += is_msvc ? " /DNDEBUG" : " -DNDEBUG";
            preview_str += "  |  Link:";
            for (const auto& f : lf) preview_str += " " + f;
            ImGui::TextWrapped("%s", preview_str.c_str());
        }
        ImGui::EndChild();

        if (!s_CfgErr.empty()) {
            ImGui::TextColored(Theme::Col::Error, "%s", s_CfgErr.c_str());
        }

        ImGui::Spacing();
        if (Theme::Button("Close", "Close Manage Configurations", true, ImVec2(90.0f * dpi, Theme::Btn::DefaultH)) || mc_close) {
            s_CfgErr.clear();
            s_ShowManageConfigsModal = false;
            ImGui::CloseCurrentPopup();
        }
        Theme::EndModal();
    }

    // Toolchain Manager Modal (V2-14 & V2-17)
    if (s_ShowToolchainManagerModal) {
        ImGui::OpenPopup("Toolchain Manager##Modal");
    }
    static bool s_TcInit = false;
    static int s_TcSelectedIdx = 0;
    static core::ToolchainProfile s_TcEdit;
    static core::ToolchainTestResult s_TcTestResult;
    static bool s_TcHasTestResult = false;
    static std::string s_TcStatusMsg;

    bool tc_close = false;
    if (Theme::BeginModal("Toolchain Manager##Modal", "Toolchain Manager", ICON_FA_GEAR, Theme::Col::Accent, 820.0f, &tc_close)) {
        const float dpi = Theme::DpiScale;
        std::vector<core::ToolchainProfile> all_profiles = compiler_manager.GetMergedToolchains(project);
        if (!s_TcInit || (s_TcSelectedIdx < 0 && !all_profiles.empty())) {
            s_TcSelectedIdx = 0;
            if (!all_profiles.empty()) {
                std::string active_tc = project.GetToolchain();
                if (active_tc.empty()) active_tc = project.GetCompiler();
                for (size_t i = 0; i < all_profiles.size(); ++i) {
                    if (all_profiles[i].name == active_tc) {
                        s_TcSelectedIdx = static_cast<int>(i);
                        break;
                    }
                }
                s_TcEdit = all_profiles[static_cast<size_t>(s_TcSelectedIdx)];
            }
            s_TcInit = true;
            s_TcHasTestResult = false;
            s_TcStatusMsg.clear();
        }
        if (s_TcSelectedIdx >= static_cast<int>(all_profiles.size())) {
            s_TcSelectedIdx = all_profiles.empty() ? -1 : static_cast<int>(all_profiles.size()) - 1;
            if (s_TcSelectedIdx >= 0) s_TcEdit = all_profiles[static_cast<size_t>(s_TcSelectedIdx)];
        }

        // Left list
        ImGui::BeginChild("TcListPane", ImVec2(235.0f * dpi, 420.0f * dpi), true);
        for (size_t i = 0; i < all_profiles.size(); ++i) {
            const auto& p = all_profiles[i];
            std::string tag = p.isProjectLocal ? " [project]" : (p.detected ? " [auto]" : " [user]");
            std::string lbl = p.name + tag;
            if (ImGui::Selectable((lbl + "##tcitem" + std::to_string(i)).c_str(), s_TcSelectedIdx == static_cast<int>(i))) {
                s_TcSelectedIdx = static_cast<int>(i);
                s_TcEdit = p;
                s_TcHasTestResult = false;
                s_TcStatusMsg.clear();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (Theme::Button("+ Add Manual", "Create a new custom toolchain profile", false, ImVec2(110.0f * dpi, Theme::Btn::SmallH))) {
            core::ToolchainProfile np;
            np.name = "custom-tc-" + std::to_string(all_profiles.size() + 1);
            np.family = core::ToolchainFamily::Gcc;
            np.cc = "gcc";
            np.cxx = "g++";
            np.ar = "ar";
            np.ld = "ld";
            np.objcopy = "objcopy";
            np.os = "freestanding";
            np.arch = "x64";
            np.detected = false;
            np.isProjectLocal = false;
            auto user_tcs = core::ToolchainManager::LoadUserProfiles();
            user_tcs.push_back(np);
            core::ToolchainManager::SaveUserProfiles(user_tcs);
            s_TcEdit = np;
            s_TcInit = false;
            s_TcHasTestResult = false;
            s_TcStatusMsg = "Added custom user toolchain '" + np.name + "'";
        }
        ImGui::SameLine();
        if (Theme::Button("Duplicate", "Clone selected profile into editable User/Project profile", false, ImVec2(105.0f * dpi, Theme::Btn::SmallH))) {
            if (s_TcSelectedIdx >= 0 && s_TcSelectedIdx < static_cast<int>(all_profiles.size())) {
                core::ToolchainProfile cp = s_TcEdit;
                cp.name = cp.name + "-copy";
                cp.detected = false;
                if (cp.isProjectLocal && project.HasProject()) {
                    auto proj_tcs = project.GetProjectToolchains();
                    proj_tcs.push_back(cp);
                    project.SetProjectToolchains(proj_tcs);
                } else {
                    cp.isProjectLocal = false;
                    auto user_tcs = core::ToolchainManager::LoadUserProfiles();
                    user_tcs.push_back(cp);
                    core::ToolchainManager::SaveUserProfiles(user_tcs);
                }
                s_TcEdit = cp;
                s_TcInit = false;
                s_TcHasTestResult = false;
                s_TcStatusMsg = "Duplicated into '" + cp.name + "'";
            }
        }
        if (Theme::Button("Refresh VS / Auto", "Re-scan PATH and Visual Studio installations (vcvarsall)", false, ImVec2(110.0f * dpi, Theme::Btn::SmallH))) {
            compiler_manager.DetectCompilers(true);
            s_TcInit = false;
            s_TcStatusMsg = "Re-scanned toolchains & Visual Studio environment.";
        }
        ImGui::SameLine();
        bool can_del_tc = (s_TcSelectedIdx >= 0 && !s_TcEdit.detected);
        if (!can_del_tc) ImGui::BeginDisabled(true);
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col::Danger);
        bool del_clicked = Theme::Button("Delete##DelTc", "Delete custom toolchain profile", false, ImVec2(105.0f * dpi, Theme::Btn::SmallH));
        ImGui::PopStyleColor();
        if (del_clicked) {
            if (s_TcEdit.isProjectLocal && project.HasProject()) {
                auto proj_tcs = project.GetProjectToolchains();
                proj_tcs.erase(std::remove_if(proj_tcs.begin(), proj_tcs.end(), [&](const core::ToolchainProfile& x) { return x.name == s_TcEdit.name; }), proj_tcs.end());
                project.SetProjectToolchains(proj_tcs);
            } else {
                auto user_tcs = core::ToolchainManager::LoadUserProfiles();
                user_tcs.erase(std::remove_if(user_tcs.begin(), user_tcs.end(), [&](const core::ToolchainProfile& x) { return x.name == s_TcEdit.name; }), user_tcs.end());
                core::ToolchainManager::SaveUserProfiles(user_tcs);
            }
            s_TcInit = false;
            s_TcHasTestResult = false;
            s_TcStatusMsg = "Deleted toolchain profile.";
        }
        if (!can_del_tc) ImGui::EndDisabled();
        ImGui::EndChild();

        ImGui::SameLine(0.0f, Theme::Sp::S2);

        // Right detail pane
        ImGui::BeginChild("TcDetailPane", ImVec2(0.0f, 420.0f * dpi), true);
        if (s_TcSelectedIdx >= 0) {
            if (s_TcEdit.detected) {
                ImGui::TextColored(Theme::Col::TextDim, "Auto-detected profile (click Duplicate to customize flags/tools/env)");
                ImGui::Spacing();
            }

            auto tc_text = [&](const char* lbl, const char* id, std::string& val, const char* hint) {
                ImGui::TextColored(Theme::Col::TextDim, "%s", lbl);
                char b[512];
                std::strncpy(b, val.c_str(), sizeof(b) - 1);
                b[sizeof(b) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint(id, hint, b, sizeof(b))) {
                    val = b;
                }
            };

            std::string original_name = s_TcEdit.name;
            bool was_detected = s_TcEdit.detected;
            if (was_detected) ImGui::BeginDisabled(true);

            tc_text("Profile Name:", "##TcName", s_TcEdit.name, "e.g. x86_64-elf-gcc");

            const char* fam_names[] = { "gcc", "clang", "msvc", "clang-cl" };
            int fam_idx = static_cast<int>(s_TcEdit.family);
            ImGui::TextColored(Theme::Col::TextDim, "Family:");
            ImGui::SetNextItemWidth(160.0f * dpi);
            if (ImGui::Combo("##TcFam", &fam_idx, fam_names, 4)) {
                s_TcEdit.family = static_cast<core::ToolchainFamily>(fam_idx);
            }
            ImGui::SameLine(0.0f, Theme::Sp::S2);
            const char* lw_opts[] = { "driver", "ld" };
            int lw_idx = (s_TcEdit.linkWith == "ld") ? 1 : 0;
            ImGui::SetNextItemWidth(130.0f * dpi);
            if (ImGui::Combo("Link with##TcLw", &lw_idx, lw_opts, 2)) {
                s_TcEdit.linkWith = lw_opts[lw_idx];
            }

            tc_text("C Compiler (cc):", "##TcCc", s_TcEdit.cc, "gcc / clang / cl.exe");
            tc_text("C++ Compiler (cxx):", "##TcCxx", s_TcEdit.cxx, "g++ / clang++ / cl.exe");
            tc_text("Archiver (ar):", "##TcAr", s_TcEdit.ar, "ar / llvm-ar / lib.exe");
            tc_text("Linker (ld):", "##TcLd", s_TcEdit.ld, "ld / ld.lld / link.exe");
            tc_text("Objcopy:", "##TcObjcopy", s_TcEdit.objcopy, "objcopy / llvm-objcopy");
            tc_text("Target Triple (--target):", "##TcTriple", s_TcEdit.targetTriple, "e.g. x86_64-elf");
            tc_text("Sysroot (--sysroot):", "##TcSysroot", s_TcEdit.sysroot, "e.g. ${root}/sysroot");
            tc_text("OS (windows/linux/macos/freestanding):", "##TcOs", s_TcEdit.os, "auto if empty");
            tc_text("Arch (x64/x86/arm64/riscv64):", "##TcArch", s_TcEdit.arch, "auto if empty");
            tc_text("Compiler Launcher (optional):", "##TcLauncher", s_TcEdit.launcher, "e.g. ccache / sccache");

            ImGui::TextColored(Theme::Col::TextDim, "Environment overrides (KEY / VALUE, supports PATH+):");
            RenderKeyValueMap("TcEnvList", s_TcEdit.env, "KEY or PATH+", "value");

            ImGui::TextColored(Theme::Col::TextDim, "Extra compile flags:");
            RenderEditableList("TcCFlags", s_TcEdit.extraCompileFlags, false, false, project, "Add compile flag");

            ImGui::TextColored(Theme::Col::TextDim, "Extra link flags:");
            RenderEditableList("TcLFlags", s_TcEdit.extraLinkFlags, false, false, project, "Add link flag");

            if (was_detected) ImGui::EndDisabled();

            if (!was_detected) {
                ImGui::Spacing();
                if (project.HasProject()) {
                    bool is_proj = s_TcEdit.isProjectLocal;
                    if (ImGui::Checkbox("Store in project (.geniebuild/project.json)", &is_proj)) {
                        s_TcEdit.isProjectLocal = is_proj;
                    }
                }
                if (Theme::SuccessButton("Save Profile", "Save edits to user or project toolchains", ImVec2(120.0f * dpi, Theme::Btn::DefaultH))) {
                    auto upsert = [&](std::vector<core::ToolchainProfile>& vec) {
                        for (auto& item : vec) {
                            if (item.name == original_name || item.name == s_TcEdit.name) {
                                item = s_TcEdit;
                                return;
                            }
                        }
                        vec.push_back(s_TcEdit);
                    };
                    if (s_TcEdit.isProjectLocal && project.HasProject()) {
                        auto pt = project.GetProjectToolchains();
                        upsert(pt);
                        project.SetProjectToolchains(pt);
                    } else {
                        auto ut = core::ToolchainManager::LoadUserProfiles();
                        upsert(ut);
                        core::ToolchainManager::SaveUserProfiles(ut);
                    }
                    s_TcInit = false;
                    s_TcStatusMsg = "Saved profile '" + s_TcEdit.name + "'";
                }
                ImGui::SameLine();
            }

            if (Theme::Button("Test Toolchain", "Verify compiler binary by compiling a tiny test program", false, ImVec2(140.0f * dpi, Theme::Btn::DefaultH))) {
                auto resolved = core::ToolchainManager::ResolveProfileVariables(s_TcEdit, project.GetRootPath());
                s_TcTestResult = core::ToolchainManager::TestProfile(resolved);
                s_TcHasTestResult = true;
            }
            if (project.HasProject()) {
                ImGui::SameLine();
                if (Theme::Button("Set Active for Project", "Use this toolchain in current project", true, ImVec2(165.0f * dpi, Theme::Btn::DefaultH))) {
                    project.SetToolchain(s_TcEdit.name);
                    s_TcStatusMsg = "Active project toolchain set to '" + s_TcEdit.name + "'";
                }
            }

            if (!s_TcStatusMsg.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(Theme::Col::Accent, "%s", s_TcStatusMsg.c_str());
            }
            if (s_TcHasTestResult) {
                ImGui::Spacing();
                ImVec4 col = s_TcTestResult.success ? Theme::Col::Success : Theme::Col::Error;
                ImGui::TextColored(col, "%s (%.2fs)", s_TcTestResult.success ? "Toolchain Test PASSED" : "Toolchain Test FAILED", s_TcTestResult.durationSeconds);
                if (!s_TcTestResult.output.empty()) {
                    ImGui::PushFont(g_Fonts.mono);
                    ImGui::TextWrapped("%s", s_TcTestResult.output.c_str());
                    ImGui::PopFont();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (Theme::Button("Close##TcClose", "Close Toolchain Manager", true, ImVec2(90.0f * dpi, Theme::Btn::DefaultH)) || tc_close) {
            s_ShowToolchainManagerModal = false;
            s_TcInit = false;
            ImGui::CloseCurrentPopup();
        }
        Theme::EndModal();
    }
}

} // namespace ui
