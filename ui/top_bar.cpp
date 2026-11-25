#include "top_bar.h"
#include "theme.h"
#include "icons.h"
#include "file_tree_panel.h"
#include "build_runner.h"
#include "build_settings_modal.h"
#include <imgui.h>
#include <nfd.h>
#include <cstring>
#include <string>
#include <algorithm>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

namespace ui {

static bool s_ShowLargeBuildModal = false;
static bool s_ShowExitWhileBuildingModal = false;
static std::string s_PendingBuildTarget = "";
static bool s_PendingRebuild = false;

void RequestExitConfirmation() {
    s_ShowExitWhileBuildingModal = true;
}

void TriggerBuildWithGuard(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    const std::string& target_name
) {
    if (project.GetFiles().size() > 500 && project.GetConfirmLargeBuild()) {
        s_PendingBuildTarget = target_name;
        s_PendingRebuild = false;
        s_ShowLargeBuildModal = true;
    } else {
        output_lines.clear();
        build_start_error.clear();
        build_runner.StartBuild(project, compiler_manager, build_start_error, target_name);
    }
}

void TriggerRebuildWithGuard(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    const std::string& target_name
) {
    if (project.GetFiles().size() > 500 && project.GetConfirmLargeBuild()) {
        s_PendingBuildTarget = target_name;
        s_PendingRebuild = true;
        s_ShowLargeBuildModal = true;
    } else {
        output_lines.clear();
        build_start_error.clear();
        build_runner.StartRebuild(project, compiler_manager, build_start_error, target_name);
    }
}

#if defined(_WIN32)
WindowChromeHitTest g_ChromeHitTest;
#endif

enum class CaptionButtonType {
    Minimize,
    Maximize,
    Close
};

// Custom drawn proper minimize, maximize, and close buttons (~46px wide)
// with hover backgrounds (red for close)
static bool DrawCaptionButton(const char* id, CaptionButtonType type, bool is_maximized, float width, float height) {
    ImGui::PushID(id);
    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    ImVec2 btn_size(width, height);

    bool clicked = ImGui::InvisibleButton("##btn", btn_size);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p_min = screen_pos;
    ImVec2 p_max = ImVec2(screen_pos.x + width, screen_pos.y + height);

    // Background fill on hover/active
    if (type == CaptionButtonType::Close) {
        if (active) {
            draw_list->AddRectFilled(p_min, p_max, IM_COL32(185, 28, 28, 255));
        } else if (hovered) {
            draw_list->AddRectFilled(p_min, p_max, IM_COL32(225, 29, 72, 255));
        }
    } else {
        if (active) {
            draw_list->AddRectFilled(p_min, p_max, IM_COL32(62, 62, 70, 220));
        } else if (hovered) {
            draw_list->AddRectFilled(p_min, p_max, IM_COL32(42, 42, 48, 210));
        }
    }

    // Glyph color: pure white when close is hovered, crisp neutral light gray otherwise
    ImU32 glyph_col = (hovered && type == CaptionButtonType::Close)
                          ? IM_COL32(255, 255, 255, 255)
                          : IM_COL32(228, 228, 232, 255);
    ImVec2 center(screen_pos.x + width * 0.5f, screen_pos.y + height * 0.5f);

    if (type == CaptionButtonType::Minimize) {
        // Horizontal minimize bar (10px wide)
        float half_w = Theme::Dim::GlyphS2;
        draw_list->AddLine(ImVec2(center.x - half_w, center.y), ImVec2(center.x + half_w, center.y), glyph_col, 1.2f);
    } else if (type == CaptionButtonType::Maximize) {
        float half_s = Theme::Dim::GlyphS2 - 0.5f * Theme::DpiScale;
        if (is_maximized) {
            // Restore symbol: two overlapping rectangles
            draw_list->AddRect(ImVec2(center.x - Theme::Dim::GlyphS1, center.y - Theme::Dim::GlyphS2),
                               ImVec2(center.x + Theme::Dim::GlyphS2, center.y + Theme::Dim::GlyphS1), glyph_col, 0.0f, 0, 1.0f);
            draw_list->AddRectFilled(ImVec2(center.x - Theme::Dim::GlyphS2, center.y - Theme::Dim::GlyphS1),
                                     ImVec2(center.x + Theme::Dim::GlyphS1, center.y + Theme::Dim::GlyphS2),
                                     hovered ? IM_COL32(45, 52, 68, 220) : ImGui::ColorConvertFloat4ToU32(Theme::Col::BgChrome));
            draw_list->AddRect(ImVec2(center.x - Theme::Dim::GlyphS2, center.y - Theme::Dim::GlyphS1),
                               ImVec2(center.x + Theme::Dim::GlyphS1, center.y + Theme::Dim::GlyphS2), glyph_col, 0.0f, 0, 1.0f);
        } else {
            // Maximize symbol: 10x10 square
            draw_list->AddRect(ImVec2(center.x - half_s, center.y - half_s), ImVec2(center.x + half_s, center.y + half_s), glyph_col, 0.0f, 0, 1.0f);
        }
    } else if (type == CaptionButtonType::Close) {
        // Clean 'X' diagonal lines
        float half_s = Theme::Dim::GlyphS2 - 0.5f * Theme::DpiScale;
        draw_list->AddLine(ImVec2(center.x - half_s, center.y - half_s), ImVec2(center.x + half_s, center.y + half_s), glyph_col, 1.2f);
        draw_list->AddLine(ImVec2(center.x - half_s, center.y + half_s), ImVec2(center.x + half_s, center.y - half_s), glyph_col, 1.2f);
    }

    ImGui::PopID();
    return clicked;
}

#if defined(_WIN32)
static void RecordItemRect() {
    ImVec2 r_min = ImGui::GetItemRectMin();
    ImVec2 r_max = ImGui::GetItemRectMax();
    RECT rect;
    rect.left   = static_cast<LONG>(r_min.x);
    rect.top    = static_cast<LONG>(r_min.y);
    rect.right  = static_cast<LONG>(r_max.x);
    rect.bottom = static_cast<LONG>(r_max.y);
    g_ChromeHitTest.interactive_rects.push_back(rect);
}
#endif

void RenderTitleBar(
    GLFWwindow* window,
    const core::Project& project
) {
    const float title_bar_h = Theme::TitleBarHeight; // 34px * dpi
    const float win_btn_w = Theme::Btn::WinCtrlW;    // 46px * dpi
    const float win_btn_h = Theme::Btn::WinCtrlH;    // 34px * dpi
    const float win_w = ImGui::GetWindowWidth();
    const float window_controls_w = win_btn_w * 3.0f;
    const float controls_x = win_w - window_controls_w;

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);
    bool is_maximized = IsZoomed(hwnd);

    g_ChromeHitTest.header_height = title_bar_h;
    g_ChromeHitTest.interactive_rects.clear();
#else
    bool is_maximized = false;
#endif

    // -------------------------------------------------------------------------
    // Center: GenieBuild · Project Folder (centered horizontally & vertically, no v1.0)
    // -------------------------------------------------------------------------
    const std::filesystem::path& root_path = project.GetRootPath();
    std::string folder_name;
    if (!root_path.empty()) {
        folder_name = root_path.filename().generic_u8string();
        if (folder_name.empty()) {
            folder_name = root_path.generic_u8string();
        }
    }

    ImGui::PushFont(g_Fonts.label);
    const float icon_w = ImGui::CalcTextSize(ICON_FA_CODE).x;
    const float icon_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();

    ImGui::PushFont(g_Fonts.title);
    const float brand_w = ImGui::CalcTextSize("GenieBuild").x;
    const float brand_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();

    const char* sep_str = "  ·  ";
    ImGui::PushFont(g_Fonts.label);
    const float sep_w = folder_name.empty() ? 0.0f : ImGui::CalcTextSize(sep_str).x;
    const float folder_w = folder_name.empty() ? 0.0f : ImGui::CalcTextSize(folder_name.c_str()).x;
    ImGui::PopFont();

    const float icon_gap = Theme::Sp::S2;
    const float total_title_w = icon_w + icon_gap + brand_w + sep_w + folder_w;
    const float max_center_right = controls_x - Theme::Sp::S3;

    float center_x = (win_w - total_title_w) * 0.5f;
    if (center_x + total_title_w > max_center_right) {
        center_x = max_center_right - total_title_w;
    }
    if (center_x < Theme::Sp::S3) {
        center_x = Theme::Sp::S3;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(Theme::Sp::S3, 0.0f), ImVec2(max_center_right, title_bar_h), true);

    float draw_x = center_x;

    // 1. Brand icon
    ImGui::PushFont(g_Fonts.label);
    dl->AddText(
        ImVec2(draw_x, (title_bar_h - icon_h) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim),
        ICON_FA_CODE
    );
    ImGui::PopFont();
    draw_x += icon_w + icon_gap;

    // 2. App Name
    ImGui::PushFont(g_Fonts.title);
    dl->AddText(
        ImVec2(draw_x, (title_bar_h - brand_h) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(Theme::Col::Text),
        "GenieBuild"
    );
    ImGui::PopFont();
    draw_x += brand_w;

    // 3. Project Name
    if (!folder_name.empty()) {
        ImGui::PushFont(g_Fonts.label);
        dl->AddText(
            ImVec2(draw_x, (title_bar_h - icon_h) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim),
            sep_str
        );
        draw_x += sep_w;

        dl->AddText(
            ImVec2(draw_x, (title_bar_h - icon_h) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(Theme::Col::Text),
            folder_name.c_str()
        );
        ImGui::PopFont();
    }

    dl->PopClipRect();

    // -------------------------------------------------------------------------
    // Right: Minimize, Maximize/Restore, Close caption buttons
    // -------------------------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(controls_x, 0.0f));

    // 1. Minimize Button
    if (DrawCaptionButton("##TitleWinMin", CaptionButtonType::Minimize, is_maximized, win_btn_w, win_btn_h)) {
#if defined(_WIN32)
        ShowWindow(hwnd, SW_MINIMIZE);
#else
        glfwIconifyWindow(window);
#endif
    }
#if defined(_WIN32)
    RecordItemRect();
#endif

    ImGui::SameLine(0.0f, 0.0f);

    // 2. Maximize / Restore Button (with Win11 Snap Flyout support)
    if (DrawCaptionButton("##TitleWinMax", CaptionButtonType::Maximize, is_maximized, win_btn_w, win_btn_h)) {
#if defined(_WIN32)
        if (is_maximized) {
            ShowWindow(hwnd, SW_RESTORE);
        } else {
            ShowWindow(hwnd, SW_MAXIMIZE);
        }
#endif
    }
#if defined(_WIN32)
    {
        ImVec2 r_min = ImGui::GetItemRectMin();
        ImVec2 r_max = ImGui::GetItemRectMax();
        g_ChromeHitTest.max_button_rect.left   = static_cast<LONG>(r_min.x);
        g_ChromeHitTest.max_button_rect.top    = static_cast<LONG>(r_min.y);
        g_ChromeHitTest.max_button_rect.right  = static_cast<LONG>(r_max.x);
        g_ChromeHitTest.max_button_rect.bottom = static_cast<LONG>(r_max.y);
    }
#endif

    ImGui::SameLine(0.0f, 0.0f);

    // 3. Close Button
    if (DrawCaptionButton("##TitleWinClose", CaptionButtonType::Close, is_maximized, win_btn_w, win_btn_h)) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
#if defined(_WIN32)
    RecordItemRect();
#endif
}

void RenderToolBar(
    GLFWwindow* window,
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines
) {
    (void)window;
    const float open_btn_h = Theme::Btn::DefaultH;
    const float recent_arrow_w = Theme::Btn::SplitArrowW;
    const std::filesystem::path& root_path = project.GetRootPath();
    const float avail_width = ImGui::GetWindowWidth() - Theme::Sp::S3;

    // Pre-calculate Right Group widths so Left Group & Right Group adapt cleanly with zero overlap
    const auto& targets = project.GetTargets();
    const bool multi_target = (targets.size() > 1);
    const float startup_combo_w = multi_target ? (126.0f * Theme::DpiScale) : 0.0f;
    const float startup_group_w = multi_target ? (startup_combo_w + Theme::Sp::S2) : 0.0f;

    const float cfg_combo_w   = 132.0f * Theme::DpiScale;
    const float cfg_group_w   = cfg_combo_w;
    const float build_btn_w   = Theme::Btn::BuildW;
    const float split_btn_w   = Theme::Btn::SplitArrowW;
    const float build_group_w = build_btn_w + split_btn_w - 1.0f;
    const float build_btn_h   = Theme::Btn::DefaultH;
    const float combo_width   = Theme::Dim::ComboW;

    const char* label_text = "Compiler:";
    const float comp_label_w = ImGui::CalcTextSize(label_text).x;

    // Compute responsive breakpoints (FPS overlay removed for clean toolbar alignment)
    const float right_with_label = startup_group_w + cfg_group_w + Theme::Sp::S2 + comp_label_w + Theme::Sp::S1 + combo_width + Theme::Sp::S2 + build_group_w;
    const float right_no_label   = startup_group_w + cfg_group_w + Theme::Sp::S2 + combo_width + Theme::Sp::S2 + build_group_w;

    // -------------------------------------------------------------------------
    // Left group: Open Folder split button, Settings, Output Path
    // -------------------------------------------------------------------------
    ImGui::AlignTextToFramePadding();

    // Open Folder Split Button (Spec §5)
    static ImVec2 s_RecentAnchorMin(0, 0);
    static ImVec2 s_RecentAnchorMax(0, 0);
    if (Theme::SplitMainButton(ICON_FA_FOLDER_OPEN " Open Folder", "Open a project folder (or drag and drop a folder anywhere on the window)", false, ImVec2(0.0f, open_btn_h))) {
        nfdchar_t* outPath = nullptr;
        std::string current_dir = root_path.generic_u8string();
        nfdresult_t result = NFD_PickFolder(&outPath, current_dir.empty() ? nullptr : current_dir.c_str());
        if (result == NFD_OKAY && outPath != nullptr) {
            project.Load(std::filesystem::u8path(outPath));
            RefreshFileTree();
            output_lines.clear();
            build_start_error.clear();
            NFD_FreePath(outPath);
        }
    }
    s_RecentAnchorMin = ImGui::GetItemRectMin();

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 1.0f);
    if (Theme::SplitArrowButton("##recent_arrow", "Recent projects list", ImVec2(recent_arrow_w, open_btn_h))) {
        s_RecentAnchorMax = ImGui::GetItemRectMax();
        ImGui::OpenPopup("RecentProjectsPopup");
    }

    ImGui::SetNextWindowPos(ImVec2(s_RecentAnchorMin.x, s_RecentAnchorMax.y + 4.0f * Theme::DpiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f * Theme::DpiScale, 0.0f), ImVec2(480.0f * Theme::DpiScale, 320.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Theme::DpiScale, 4.0f * Theme::DpiScale));
    if (ImGui::BeginPopup("RecentProjectsPopup")) {
        ImGui::TextColored(Theme::Col::TextDim, "  Recent Projects");
        ImGui::Separator();
        auto recent_list = core::RecentProjects::Load();
        if (recent_list.empty()) {
            ImGui::MenuItem("No recent projects", nullptr, false, false);
        } else {
            for (const auto& rpath : recent_list) {
                std::filesystem::path rp = std::filesystem::u8path(rpath);
                std::string fname = rp.filename().generic_u8string();
                if (fname.empty()) fname = rpath;
                std::string label = std::string(ICON_FA_FOLDER) + "  " + fname;

                if (ImGui::MenuItem(label.c_str())) {
                    project.Load(rp);
                    RefreshFileTree();
                    output_lines.clear();
                    build_start_error.clear();
                }
                Theme::Tooltip(rpath.c_str());
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    const bool has_project = project.HasProject();

    // Settings Button (Spec §5.1.1)
    ImGui::SameLine(0.0f, Theme::Sp::S2);
    if (!has_project) ImGui::BeginDisabled(true);
    if (Theme::Button(ICON_FA_GEAR " Settings", has_project ? "Build settings: includes, libraries, defines, flags (Ctrl+,)" : "Open a project folder to configure build settings", false, ImVec2(0.0f, open_btn_h))) {
        OpenBuildSettingsModal();
    }
    if (!has_project) ImGui::EndDisabled();

    // Responsive sizing for Output controls + Right group (measure actual item end X in window coords)
    const float cursor_after_settings = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const float out_lbl_w = ImGui::CalcTextSize("Output:").x;
    const float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float fixed_out_overhead = Theme::Sp::S3 + out_lbl_w + Theme::Sp::S1 + Theme::Sp::S1 + browse_w + Theme::Sp::S2;

    bool show_comp_label = true;
    float total_right_w = right_with_label;
    float out_dir_w = Theme::OutputDirWidth;

    float needed_full = cursor_after_settings + fixed_out_overhead + out_dir_w + total_right_w;
    if (needed_full > avail_width) {
        // Step 1: Drop "Compiler:" label cleanly
        show_comp_label = false;
        total_right_w = right_no_label;
        float free_for_out = avail_width - cursor_after_settings - fixed_out_overhead - total_right_w;
        if (free_for_out < Theme::OutputDirWidth) {
            out_dir_w = std::clamp(free_for_out, 56.0f * Theme::DpiScale, Theme::OutputDirWidth);
        }
    }

    // Output Directory controls
    ImGui::SameLine(0.0f, Theme::Sp::S3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::Col::TextDim, "Output:");
    ImGui::SameLine(0.0f, Theme::Sp::S1);

    if (!has_project) ImGui::BeginDisabled(true);
    char out_dir_buf[260];
    const std::string& current_out_dir = project.GetOutputDir();
    std::strncpy(out_dir_buf, current_out_dir.c_str(), sizeof(out_dir_buf) - 1);
    out_dir_buf[sizeof(out_dir_buf) - 1] = '\0';

    ImGui::SetNextItemWidth(out_dir_w);
    if (ImGui::InputText("##OutputDirInput", out_dir_buf, sizeof(out_dir_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        project.SetOutputDir(out_dir_buf);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        project.SetOutputDir(out_dir_buf);
    }
    Theme::Tooltip(has_project ? ("Output directory (saved in project.json)\nResolved target:\n" + project.GetAbsoluteOutputDir().generic_u8string()).c_str() : "Open a project folder first");

    ImGui::SameLine(0.0f, Theme::Sp::S1);

    if (Theme::Button("Browse...##OutBrowse", "Choose output folder for compiled binaries", false, ImVec2(0.0f, open_btn_h))) {
        nfdchar_t* outBrowsePath = nullptr;
        std::string cur_abs_out = project.GetAbsoluteOutputDir().generic_u8string();
        nfdresult_t res = NFD_PickFolder(&outBrowsePath, cur_abs_out.empty() ? nullptr : cur_abs_out.c_str());
        if (res == NFD_OKAY && outBrowsePath != nullptr) {
            project.SetOutputDirPath(std::filesystem::u8path(outBrowsePath));
            NFD_FreePath(outBrowsePath);
        }
    }
    if (!has_project) ImGui::EndDisabled();

    // -------------------------------------------------------------------------
    // Right group: [Startup target picker], Debug/Release toggle, Compiler selector, Build split button
    // -------------------------------------------------------------------------
    const float left_end_x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const float target_cursor_x = avail_width - total_right_w;
    ImGui::SameLine(std::max(left_end_x + Theme::Sp::S2, target_cursor_x));

    // 0. Startup target picker (progressive disclosure: only when >1 target, V2-2)
    if (multi_target) {
        std::string cur_startup = project.GetStartupTarget();
        if (cur_startup.empty() && !targets.empty()) cur_startup = targets[0].name;
        std::string startup_preview = std::string(ICON_FA_PLAY " ") + cur_startup;

        static ImVec2 s_StartAnchorMin(0, 0);
        static ImVec2 s_StartAnchorMax(0, 0);
        bool start_popup_open = ImGui::IsPopupOpen("StartupTargetPopup");
        if (Theme::ComboButton("##StartupTargetSelector", startup_preview.c_str(), "Startup target (used by Build startup target)", ImVec2(startup_combo_w, build_btn_h), start_popup_open)) {
            s_StartAnchorMin = ImGui::GetItemRectMin();
            s_StartAnchorMax = ImGui::GetItemRectMax();
            ImGui::OpenPopup("StartupTargetPopup");
        }
        ImGui::SetNextWindowPos(ImVec2(s_StartAnchorMin.x, s_StartAnchorMax.y + 4.0f * Theme::DpiScale), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Theme::DpiScale, 4.0f * Theme::DpiScale));
        if (ImGui::BeginPopup("StartupTargetPopup")) {
            ImGui::TextColored(Theme::Col::TextDim, "  Startup Target");
            ImGui::Separator();
            for (const auto& t : targets) {
                bool sel = (t.name == cur_startup);
                std::string lbl = std::string(sel ? ICON_FA_CHECK "  " : "     ") + t.name + " (" + core::TargetKindToString(t.kind) + ")";
                if (ImGui::MenuItem(lbl.c_str())) {
                    project.SetStartupTarget(t.name);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::SameLine(0.0f, Theme::Sp::S2);
    }

    // 1. Active Configuration Dropdown (V2-4)
    std::string cur_cfg = project.GetConfiguration();
    if (cur_cfg.empty()) cur_cfg = "Debug";

    static ImVec2 s_CfgAnchorMin(0, 0);
    static ImVec2 s_CfgAnchorMax(0, 0);
    bool cfg_popup_open = ImGui::IsPopupOpen("ConfigSelectorPopup");
    std::string cfg_tip = "Active build configuration: " + cur_cfg + "\nSwitch configuration or manage custom configurations";
    if (!has_project) ImGui::BeginDisabled(true);
    if (Theme::ComboButton("##ConfigSelector", cur_cfg.c_str(), cfg_tip.c_str(), ImVec2(cfg_combo_w, build_btn_h), cfg_popup_open)) {
        s_CfgAnchorMin = ImGui::GetItemRectMin();
        s_CfgAnchorMax = ImGui::GetItemRectMax();
        ImGui::OpenPopup("ConfigSelectorPopup");
    }
    if (!has_project) ImGui::EndDisabled();

    ImGui::SetNextWindowPos(ImVec2(s_CfgAnchorMin.x, s_CfgAnchorMax.y + 4.0f * Theme::DpiScale), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Theme::DpiScale, 4.0f * Theme::DpiScale));
    if (ImGui::BeginPopup("ConfigSelectorPopup")) {
        ImGui::TextColored(Theme::Col::TextDim, "  Configuration");
        ImGui::Separator();
        std::vector<std::string> cfg_names = project.GetConfigNames();
        for (const auto& cname : cfg_names) {
            bool sel = (cname == cur_cfg);
            std::string lbl = std::string(sel ? ICON_FA_CHECK "  " : "     ") + cname;
            if (ImGui::MenuItem(lbl.c_str())) {
                project.SetConfiguration(cname);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_GEAR "  Manage configurations...")) {
            OpenManageConfigsModal();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    ImGui::SameLine(0.0f, Theme::Sp::S2);

    // 2. Compiler Label & Selector
    if (show_comp_label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Col::TextDim, "%s", label_text);
        ImGui::SameLine(0.0f, Theme::Sp::S1);
    }

    const auto& detected_compilers = compiler_manager.GetDetectedCompilers();
    const bool has_compilers = !detected_compilers.empty();

    std::string current_compiler = project.GetCompiler();
    bool saved_compiler_found = false;

    if (current_compiler.empty()) {
        if (has_compilers) {
            current_compiler = detected_compilers[0].name;
            project.SetCompiler(current_compiler);
            saved_compiler_found = true;
        }
    } else {
        for (const auto& comp : detected_compilers) {
            if (comp.name == current_compiler) {
                saved_compiler_found = true;
                break;
            }
        }
    }

    std::string preview_text;
    if (!current_compiler.empty()) {
        if (saved_compiler_found) {
            preview_text = current_compiler;
        } else {
            preview_text = current_compiler + " (not found)";
        }
    } else if (has_compilers) {
        preview_text = detected_compilers[0].name;
    } else {
        preview_text = "No compiler";
    }

    static ImVec2 s_CompAnchorMin(0, 0);
    static ImVec2 s_CompAnchorMax(0, 0);
    bool compiler_popup_open = ImGui::IsPopupOpen("CompilerSelectPopup");
    if (Theme::ComboButton("##CompilerSelector", preview_text.c_str(), "Select compiler toolchain", ImVec2(combo_width, build_btn_h), compiler_popup_open)) {
        s_CompAnchorMin = ImGui::GetItemRectMin();
        s_CompAnchorMax = ImGui::GetItemRectMax();
        ImGui::OpenPopup("CompilerSelectPopup");
    }

    ImGui::SetNextWindowPos(ImVec2(s_CompAnchorMin.x, s_CompAnchorMax.y + 4.0f * Theme::DpiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(combo_width, 0.0f), ImVec2(480.0f * Theme::DpiScale, 280.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Theme::DpiScale, 4.0f * Theme::DpiScale));
    if (ImGui::BeginPopup("CompilerSelectPopup")) {
        if (!has_compilers) {
            ImGui::MenuItem("No compiler detected on PATH", nullptr, false, false);
        } else {
            if (!saved_compiler_found && !current_compiler.empty()) {
                std::string missing_item = current_compiler + " (not found)";
                ImGui::MenuItem(missing_item.c_str(), nullptr, false, false);
                ImGui::Separator();
            }
            for (const auto& comp : detected_compilers) {
                const bool is_selected = (comp.name == current_compiler);
                std::string item_label = std::string(is_selected ? ICON_FA_CHECK "  " : "     ") + comp.name;
                if (ImGui::MenuItem(item_label.c_str(), nullptr, false)) {
                    project.SetCompiler(comp.name);
                }
                Theme::Tooltip(("Compiler binary:\n" + comp.executable_path.generic_u8string()).c_str());
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    ImGui::SameLine(0.0f, Theme::Sp::S2);

    // 3. Build / Rebuild / Clean Split Button (Spec §5.1.5)
    const bool is_building = build_runner.IsRunning();
    const bool is_scanning = project.IsScanning();
    const bool list_empty = project.GetFiles().empty();
    const bool has_missing = project.HasMissingFiles();
    const bool compiler_valid = has_compilers && (compiler_manager.FindCompiler(project.GetCompiler()) != nullptr);
    const bool can_build = !is_building && !is_scanning && !list_empty && !has_missing && compiler_valid;

    if (is_building) {
        int elapsed_s = static_cast<int>(build_runner.GetElapsedSeconds());
        std::string build_label = "Building... " + std::to_string(elapsed_s) + "s";
        Theme::DisabledButton(build_label.c_str(), ImVec2(build_btn_w, build_btn_h), ImVec4(1.0f, 1.0f, 1.0f, 1.0f), Theme::Col::Warning, Theme::Col::Border, ImDrawFlags_RoundCornersLeft);
    } else if (is_scanning) {
        std::string scan_label = std::string(ICON_FA_SPINNER) + " Scanning...";
        Theme::DisabledButton(scan_label.c_str(), ImVec2(build_btn_w, build_btn_h), Theme::Col::TextDim, Theme::Col::Button, Theme::Col::Border, ImDrawFlags_RoundCornersLeft);
    } else if (!can_build) {
        Theme::DisabledButton("Build", ImVec2(build_btn_w, build_btn_h), Theme::Col::TextDim, Theme::Col::Button, Theme::Col::Border, ImDrawFlags_RoundCornersLeft);

        if (is_scanning) {
            Theme::Tooltip("Scanning project directory in background...");
        } else if (!compiler_valid) {
            Theme::Tooltip("Cannot build: no valid compiler found on PATH.\nPlease install g++, clang++, or cl.");
        } else if (has_missing) {
            Theme::Tooltip("Cannot build: some files in the build list are missing on disk.\nRemove missing files to proceed.");
        } else if (list_empty) {
            Theme::Tooltip("Cannot build: build list is empty.\nAdd source files from the Project Explorer.");
        }
    } else {
        std::string build_tooltip = "Incremental build (Ctrl+B / F5) with " + project.GetCompiler();
        if (Theme::SuccessButton("Build", build_tooltip.c_str(), ImVec2(build_btn_w, build_btn_h), ImDrawFlags_RoundCornersLeft)) {
            TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines);
        }
    }

    // Split Dropdown Arrow for Rebuild / Clean (§5.1.5)
    static ImVec2 s_BuildAnchorMax(0, 0);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 1.0f);
    if (Theme::SplitArrowButton("##build_split", "Build options: Rebuild, Clean", ImVec2(split_btn_w, build_btn_h), can_build)) {
        s_BuildAnchorMax = ImGui::GetItemRectMax();
        ImGui::OpenPopup("BuildSplitDropdown");
    }

    ImGui::SetNextWindowPos(ImVec2(s_BuildAnchorMax.x, s_BuildAnchorMax.y + 4.0f * Theme::DpiScale), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Theme::DpiScale, 4.0f * Theme::DpiScale));
    if (ImGui::BeginPopup("BuildSplitDropdown")) {
        if (multi_target) {
            if (ImGui::MenuItem(ICON_FA_PLAY "  Build all", nullptr, false, can_build)) {
                TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, "");
            }
            std::string st = project.GetStartupTarget();
            if (!st.empty()) {
                std::string st_lbl = std::string(ICON_FA_PLAY "  Build startup target (") + st + ")";
                if (ImGui::MenuItem(st_lbl.c_str(), nullptr, false, can_build)) {
                    TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, st);
                }
            }
            if (ImGui::BeginMenu("     Build target")) {
                for (const auto& t : targets) {
                    std::string t_lbl = t.name + " (" + core::TargetKindToString(t.kind) + ")";
                    if (ImGui::MenuItem(t_lbl.c_str(), nullptr, false, can_build)) {
                        TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, t.name);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
        }

        if (ImGui::MenuItem(ICON_FA_ROTATE "  Rebuild (Ctrl+Shift+B)", nullptr, false, can_build)) {
            TriggerRebuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines);
        }
        Theme::Tooltip("Clean all object files and perform a full compile");

        if (ImGui::MenuItem(ICON_FA_TRASH "  Clean", nullptr, false, !is_building)) {
            std::string clean_msg;
            if (build_runner.Clean(project, clean_msg)) {
                core::OutLine ol;
                ol.kind = core::OutLineKind::Status;
                ol.text = clean_msg;
                output_lines.push_back(ol);
            }
        }
        Theme::Tooltip("Delete compiled object files and target binary for active configuration");

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    // Huge-list warning confirmation modal (Spec §3.7)
    if (s_ShowLargeBuildModal) {
        ImGui::OpenPopup("Large Build Confirmation##Modal");
    }
    bool large_close_x = false;
    if (Theme::BeginModal("Large Build Confirmation##Modal", "Large Build Confirmation", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 460.0f, &large_close_x)) {
        size_t n_files = project.GetEnabledFilesCount();
        if (n_files == 0) n_files = project.GetFiles().size();
        unsigned int cores = std::thread::hardware_concurrency();
        if (cores == 0) cores = 4;
        float est_minutes = std::max(0.5f, (n_files * 0.20f) / (cores * 60.0f));

        ImGui::TextColored(Theme::Col::Text, "This build queue contains %zu source files.", n_files);
        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::TextDim, "A full build will compile all enabled files in parallel (estimated ~%.1f min on %u CPU cores).", est_minutes, cores);
        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::Text, "Do you want to continue?");

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float dpi = Theme::DpiScale;
        const float btn_h = Theme::Btn::DefaultH;
        const float cancel_w = 78.0f * dpi;
        const float build_w  = 92.0f * dpi;
        const float right_end = 460.0f * dpi - Theme::Sp::S4;

        if (Theme::Button("Don't ask again", "Do not show this warning for this project", false, ImVec2(0.0f, btn_h))) {
            project.SetConfirmLargeBuild(false);
            output_lines.clear();
            build_start_error.clear();
            if (s_PendingRebuild) {
                build_runner.StartRebuild(project, compiler_manager, build_start_error, s_PendingBuildTarget);
            } else {
                build_runner.StartBuild(project, compiler_manager, build_start_error, s_PendingBuildTarget);
            }
            s_ShowLargeBuildModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        ImGui::SetCursorPosX(right_end - build_w - Theme::Sp::S2 - cancel_w);
        if (Theme::Button("Cancel", "Abort build", false, ImVec2(cancel_w, btn_h)) || large_close_x) {
            s_ShowLargeBuildModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);
        if (Theme::SuccessButton("Build", "Proceed with compilation", ImVec2(build_w, btn_h))) {
            output_lines.clear();
            build_start_error.clear();
            if (s_PendingRebuild) {
                build_runner.StartRebuild(project, compiler_manager, build_start_error, s_PendingBuildTarget);
            } else {
                build_runner.StartBuild(project, compiler_manager, build_start_error, s_PendingBuildTarget);
            }
            s_ShowLargeBuildModal = false;
            ImGui::CloseCurrentPopup();
        }

        Theme::EndModal();
    }

    // Exit confirmation modal if build is in progress (Spec §4)
    if (s_ShowExitWhileBuildingModal) {
        ImGui::OpenPopup("Build in Progress##ExitModal");
    }
    bool exit_close_x = false;
    if (Theme::BeginModal("Build in Progress##ExitModal", "Build in Progress", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 420.0f, &exit_close_x)) {
        ImGui::TextColored(Theme::Col::Text, "A compiler process is currently running.");
        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::TextDim, "Exiting now will cancel the active build.");

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float dpi = Theme::DpiScale;
        const float btn_h = Theme::Btn::DefaultH;
        const float keep_w = 136.0f * dpi;
        const float exit_w = 96.0f * dpi;
        const float right_end = 420.0f * dpi - Theme::Sp::S4;

        ImGui::SetCursorPosX(right_end - keep_w - Theme::Sp::S2 - exit_w);
        if (Theme::Button("Exit anyway", "Abort running build and exit", false, ImVec2(exit_w, btn_h))) {
            s_ShowExitWhileBuildingModal = false;
            ImGui::CloseCurrentPopup();
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);
        if (Theme::Button("Continue building", "Keep build running", true, ImVec2(keep_w, btn_h)) || exit_close_x) {
            s_ShowExitWhileBuildingModal = false;
            ImGui::CloseCurrentPopup();
        }

        Theme::EndModal();
    }

    // Build Settings Modal (Spec §5.1.1)
    RenderBuildSettingsModal(project, compiler_manager);
}

void RenderTopBar(
    GLFWwindow* window,
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines
) {
    RenderTitleBar(window, project);
    ImGui::Spacing();
    RenderToolBar(window, project, compiler_manager, build_runner, build_start_error, output_lines);
}

} // namespace ui
