#include "build_settings_modal.h"
#include "settings_resolver.h"
#include "theme.h"
#include "icons.h"

#include <imgui.h>
#include <nfd.h>
#include <reproc++/reproc.hpp>

#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cmath>

namespace ui {

static bool s_ShowSettingsModal = false;
static bool s_ShowManageConfigsModal = false;
static std::string s_SelectedTargetScope = ""; // "" = Project (Global), else target name
static std::string s_SelectedFolderScope = ""; // e.g. "src/simd"
static std::string s_SelectedFileScope = "";   // e.g. "src/hot/mathlib.cpp"
static std::string s_EffectiveFlagsFile = "";

void OpenBuildSettingsModal(const std::string& target_name) {
    s_ShowSettingsModal = true;
    s_SelectedTargetScope = target_name;
    s_SelectedFolderScope.clear();
    s_SelectedFileScope.clear();
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
    if (!file_rel.empty()) {
        s_EffectiveFlagsFile = file_rel;
    }
}

void OpenManageConfigsModal() {
    s_ShowManageConfigsModal = true;
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

void RenderBuildSettingsModal(core::Project& project, const core::CompilerManager& compiler_manager) {
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

                        // Condition
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
                        if (RenderEditableList((std::string(id_prefix) + "_libs").c_str(), blk.libs, false, false, project, "Add library")) t_modified = true;
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
                            for (size_t oi = 0; oi < t_edit.overrides.size(); ++oi) {
                                const auto& ov = t_edit.overrides[oi];
                                ImGui::PushID(static_cast<int>(oi));
                                ImGui::TextColored(Theme::Col::Override, "%s", ICON_FA_FLAG);
                                ImGui::SameLine(0.0f, Theme::Sp::S1);
                                ImGui::TextColored(Theme::Col::Text, "%s", ov.match.c_str());
                                if (!ov.condition.empty()) {
                                    ImGui::SameLine();
                                    ImGui::TextColored(Theme::Col::TextDim, "[if %s]", ov.condition.c_str());
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

                // Parallel Jobs
                ImGui::TextColored(Theme::Col::Text, "Compile Jobs:");
                ImGui::SameLine(Theme::Dim::FormLabelW);
                int jobs = project.GetJobs();
                ImGui::SetNextItemWidth(Theme::Dim::FormInputW);
                if (ImGui::InputInt("##JobsInput", &jobs, 1, 2)) {
                    if (jobs < 0) jobs = 0;
                    if (jobs > 64) jobs = 64;
                    project.SetJobs(jobs);
                }
                Theme::Tooltip("Number of parallel worker compilation jobs (0 = auto-detect all CPU cores)");

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

                // 2. Library Names
                ImGui::TextColored(Theme::Col::TextDim, "Libraries (-l<name> or <name>.lib):");
                auto libs = project.GetLibs();
                if (RenderEditableList("LibsList", libs, false, false, project, "Add library name (e.g. SDL2, opengl32)")) {
                    project.SetLibs(libs);
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
}

} // namespace ui
