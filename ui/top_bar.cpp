#include "top_bar.h"
#include "theme.h"
#include "icons.h"
#include "file_tree_panel.h"
#include "build_runner.h"
#include "build_steps.h"
#include "build_settings_modal.h"
#include "output_panel.h"
#include "toolchain.h"
#include "msvc_env.h"
#include "compdb.h"
#include "fs_util.h"
#include "hints.h"
#include <imgui.h>
#include <nfd.h>
#include <cstring>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

namespace ui {

static bool s_ShowLargeBuildModal = false;
static bool s_ShowExitWhileBuildingModal = false;
static bool s_ShowTrustModal = false;
static bool s_TrustPendingBuild = false;
static bool s_TrustPendingRebuild = false;
static std::string s_TrustPendingTarget = "";
static std::string s_PromptedTrustRoot = "";
static std::string s_PendingBuildTarget = "";
static bool s_PendingRebuild = false;

static core::RunRunner* s_ActiveRunRunner = nullptr;
static bool s_PendingRunAfterBuild = false;
static bool s_PendingRunIsDebug = false;
static bool s_ShowRunningExeConflictModal = false;
static std::string s_ConflictExePath = "";
static bool s_ConflictIsRebuild = false;
static std::string s_ConflictTarget = "";
static std::string s_DismissedFallbackKey = "";
static std::string s_DismissedPeWarnKey = "";

// V2-22 / V2-23 / V2-24 Modals & Banners
static bool s_ShowLongPathHelpModal = false;
static bool s_ShowDefenderHelpModal = false;
static bool s_DismissedDefenderSession = false;
static bool s_ShowImportModal = false;
static bool s_ShowImportChecklistBanner = false;
static core::ImportPlan s_ImportPlan;
static std::string s_ImportRootBuf;
static int s_ImportSelectedTarget = 0;
static std::string s_ImportWriteError;
static bool s_LongPathsEnabledCached = true;
static bool s_LongPathsChecked = false;

void SetActiveRunRunner(core::RunRunner* run_runner) {
    s_ActiveRunRunner = run_runner;
}

static std::string ToLowerGenericPath(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path norm = std::filesystem::weakly_canonical(p, ec);
    std::string s = ec ? p.lexically_normal().generic_u8string() : norm.generic_u8string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool CheckRunningExeConflict(
    const core::Project& project,
    const std::string& target_name,
    std::string& out_conflict_path
) {
#if defined(_WIN32)
    if (!s_ActiveRunRunner || !s_ActiveRunRunner->IsRunning()) return false;
    std::string running = s_ActiveRunRunner->GetRunningProgram();
    if (running.empty()) return false;
    std::string run_low = ToLowerGenericPath(std::filesystem::u8path(running));
    for (const auto& t : project.GetTargets()) {
        if (!target_name.empty() && t.name != target_name) continue;
        if (t.kind != core::TargetKind::Executable) continue;
        std::filesystem::path bin_p = t.GetBinaryPath(project.GetAbsoluteOutputDir(), project.GetConfiguration(), false);
        std::string bin_low = ToLowerGenericPath(bin_p);
        if (!bin_low.empty() && bin_low == run_low) {
            out_conflict_path = bin_p.generic_u8string();
            return true;
        }
    }
#else
    (void)project;
    (void)target_name;
    (void)out_conflict_path;
#endif
    return false;
}

static const core::Target* FindStartupTarget(const core::Project& project) {
    if (!project.GetStartupTarget().empty()) {
        if (const core::Target* t = project.FindTarget(project.GetStartupTarget())) {
            return t;
        }
    }
    return project.GetPrimaryTarget();
}

static bool LaunchRunConfig(
    const core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::RunRunner& run_runner,
    std::vector<core::OutLine>& run_lines,
    std::string& run_start_error,
    bool is_debug
) {
    const core::Target* startup = FindStartupTarget(project);
    if (!startup) {
        const auto& ts = project.GetTargets();
        if (!ts.empty()) startup = &ts[0];
    }
    if (!startup) {
        run_start_error = "No target configured.";
        RequestOutputTab(true);
        return false;
    }
    const core::RunConfig& rc = is_debug ? startup->debug : startup->run;
    if (rc.command.empty()) {
        run_start_error = is_debug ? "No Debug command configured for startup target."
                                   : "No Run command configured. Configure Run in Build Settings -> Run / Debug.";
        RequestOutputTab(true);
        return false;
    }

    auto tc_res = compiler_manager.ResolveProjectToolchain(project);
    std::filesystem::path cwd;
    std::unordered_map<std::string, std::string> env;
    core::BuildCommand cmd = core::FormulateRunCommand(project, *startup, rc, &tc_res.profile, cwd, env);
    if (cmd.program.empty()) {
        run_start_error = "Failed to formulate run command.";
        RequestOutputTab(true);
        return false;
    }

    run_lines.clear();
    run_start_error.clear();
    core::OutLine header;
    header.kind = core::OutLineKind::Status;
    header.text = "> " + cmd.display_command;
    run_lines.push_back(std::move(header));

    RequestOutputTab(true);
    return run_runner.Start(cmd, cwd, &env, &run_start_error);
}

void RequestExitConfirmation() {
    s_ShowExitWhileBuildingModal = true;
}

static bool EnsureProjectTrustedForBuild(core::Project& project, bool is_rebuild, const std::string& target_name) {
    if (core::TrustStore::CountCustomCommands(project) > 0 && !core::TrustStore::IsTrusted(project.GetRootPath())) {
        s_TrustPendingBuild = !is_rebuild;
        s_TrustPendingRebuild = is_rebuild;
        s_TrustPendingTarget = target_name;
        s_ShowTrustModal = true;
        return false;
    }
    return true;
}

void TriggerBuildWithGuard(
    core::Project& project,
    const core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    const std::string& target_name
) {
    if (!EnsureProjectTrustedForBuild(project, false, target_name)) {
        build_start_error = "Project is not trusted yet. Trust this folder to run custom build steps.";
        return;
    }
    std::string conflict_path;
    if (CheckRunningExeConflict(project, target_name, conflict_path)) {
        s_ConflictExePath = conflict_path;
        s_ConflictIsRebuild = false;
        s_ConflictTarget = target_name;
        s_ShowRunningExeConflictModal = true;
        return;
    }
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
    if (!EnsureProjectTrustedForBuild(project, true, target_name)) {
        build_start_error = "Project is not trusted yet. Trust this folder to run custom build steps.";
        return;
    }
    std::string conflict_path;
    if (CheckRunningExeConflict(project, target_name, conflict_path)) {
        s_ConflictExePath = conflict_path;
        s_ConflictIsRebuild = true;
        s_ConflictTarget = target_name;
        s_ShowRunningExeConflictModal = true;
        return;
    }
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

void TriggerRunWithGuard(
    core::Project& project,
    core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    core::RunRunner& run_runner,
    std::vector<core::OutLine>& run_lines,
    std::string& run_start_error,
    bool is_debug
) {
    s_ActiveRunRunner = &run_runner;
    const core::Target* startup = FindStartupTarget(project);
    if (!startup) {
        run_start_error = "No active target found.";
        RequestOutputTab(true);
        return;
    }
    const core::RunConfig& rc = is_debug ? startup->debug : startup->run;
    if (rc.command.empty()) {
        run_start_error = is_debug ? "No Debug command configured for target."
                                   : "No Run command configured. Set one in Build Settings -> Run / Debug.";
        RequestOutputTab(true);
        return;
    }

    if (core::TrustStore::CountCustomCommands(project) > 0 && !core::TrustStore::IsTrusted(project.GetRootPath())) {
        s_TrustPendingBuild = false;
        s_TrustPendingRebuild = false;
        s_TrustPendingTarget = startup->name;
        s_ShowTrustModal = true;
        run_start_error = "Project folder is not trusted yet. Trust this folder to run custom commands.";
        RequestOutputTab(true);
        return;
    }

    if (!is_debug && rc.buildBeforeRun) {
        if (run_runner.IsRunning()) {
            run_runner.Stop();
        }
        s_PendingRunAfterBuild = true;
        s_PendingRunIsDebug = false;
        output_lines.clear();
        build_start_error.clear();
        if (!build_runner.StartBuild(project, compiler_manager, build_start_error, startup->name)) {
            s_PendingRunAfterBuild = false;
            RequestOutputTab(false);
        }
        return;
    }

    LaunchRunConfig(project, compiler_manager, run_runner, run_lines, run_start_error, is_debug);
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
    core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    core::RunRunner* run_runner,
    std::vector<core::OutLine>* run_lines,
    std::string* run_start_error
) {
    (void)window;
    if (run_runner) {
        s_ActiveRunRunner = run_runner;
    }
    if (s_PendingRunAfterBuild && !build_runner.IsRunning()) {
        s_PendingRunAfterBuild = false;
        if (build_runner.GetLastResult().success && run_runner && run_lines && run_start_error) {
            LaunchRunConfig(project, compiler_manager, *run_runner, *run_lines, *run_start_error, s_PendingRunIsDebug);
        } else {
            RequestOutputTab(false);
        }
    }

    const float open_btn_h = Theme::Btn::DefaultH;
    const float recent_arrow_w = Theme::Btn::SplitArrowW;
    const std::filesystem::path& root_path = project.GetRootPath();
    const float avail_width = ImGui::GetWindowWidth() - Theme::Sp::S3;

    // Pre-calculate Right Group widths so Left Group & Right Group adapt cleanly with zero overlap
    const auto& targets = project.GetTargets();
    const bool multi_target = (targets.size() > 1);
    const float startup_combo_w = multi_target ? (126.0f * Theme::DpiScale) : 0.0f;
    const float startup_group_w = multi_target ? (startup_combo_w + Theme::Sp::S2) : 0.0f;

    const core::Target* startup_tgt = FindStartupTarget(project);
    const bool has_debug_btn = (run_runner != nullptr && startup_tgt != nullptr && !startup_tgt->debug.command.empty());
    const float run_btn_w = (run_runner != nullptr) ? (76.0f * Theme::DpiScale) : 0.0f;
    const float debug_btn_w = has_debug_btn ? (66.0f * Theme::DpiScale) : 0.0f;
    const float run_group_w = (run_runner != nullptr ? (Theme::Sp::S2 + run_btn_w) : 0.0f)
                            + (has_debug_btn ? (Theme::Sp::S1 + debug_btn_w) : 0.0f);

    const float cfg_combo_w   = 132.0f * Theme::DpiScale;
    const float cfg_group_w   = cfg_combo_w;
    const float build_btn_w   = Theme::Btn::BuildW;
    const float split_btn_w   = Theme::Btn::SplitArrowW;
    const float build_group_w = build_btn_w + split_btn_w - 1.0f;
    const float build_btn_h   = Theme::Btn::DefaultH;
    const float combo_width   = Theme::Dim::ComboW;

    const char* label_text = "Toolchain:";
    const float comp_label_w = ImGui::CalcTextSize(label_text).x;

    // Compute responsive breakpoints
    const float right_with_label = startup_group_w + cfg_group_w + Theme::Sp::S2 + comp_label_w + Theme::Sp::S1 + combo_width + Theme::Sp::S2 + build_group_w + run_group_w;
    const float right_no_label   = startup_group_w + cfg_group_w + Theme::Sp::S2 + combo_width + Theme::Sp::S2 + build_group_w + run_group_w;

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
            for (size_t ri = 0; ri < recent_list.size(); ++ri) {
                const auto& rpath = recent_list[ri];
                std::filesystem::path rp = std::filesystem::u8path(rpath);
                std::string fname = rp.filename().generic_u8string();
                if (fname.empty()) fname = rpath;
                std::string label = std::string(ICON_FA_FOLDER) + "  " + fname;

                ImGui::PushID(static_cast<int>(ri));
                if (ImGui::MenuItem(label.c_str())) {
                    project.Load(rp);
                    RefreshFileTree();
                    output_lines.clear();
                    build_start_error.clear();
                }
                Theme::Tooltip(rpath.c_str());
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CODE "  Import from compile_commands.json...")) {
            nfdchar_t* out_cc = nullptr;
            nfdu8filteritem_t cc_filter[1] = { { "Compilation Database", "json" } };
            std::string start_d = root_path.generic_u8string();
            if (NFD_OpenDialog(&out_cc, cc_filter, 1, start_d.empty() ? nullptr : start_d.c_str()) == NFD_OKAY && out_cc != nullptr) {
                std::filesystem::path cc_path = std::filesystem::u8path(out_cc);
                NFD_FreePath(out_cc);
                std::filesystem::path guess_root = cc_path.parent_path();
                std::string par_low = guess_root.filename().generic_u8string();
                std::transform(par_low.begin(), par_low.end(), par_low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (par_low == "build" || par_low == "out" || par_low.rfind("cmake-build", 0) == 0) {
                    if (guess_root.has_parent_path()) guess_root = guess_root.parent_path();
                }
                s_ImportRootBuf = guess_root.generic_u8string();
                s_ImportPlan = core::CompDbManager::AnalyzeCompileCommands(cc_path, guess_root);
                s_ImportSelectedTarget = 0;
                s_ImportWriteError.clear();
                s_ShowImportModal = true;
            }
        }
        Theme::Tooltip("One-time import from CMake/Meson/Bazel compile_commands.json into v2 project.json");
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
        for (size_t ci = 0; ci < cfg_names.size(); ++ci) {
            const auto& cname = cfg_names[ci];
            bool sel = (cname == cur_cfg);
            std::string lbl = std::string(sel ? ICON_FA_CHECK "  " : "     ") + cname;
            ImGui::PushID(static_cast<int>(ci));
            if (ImGui::MenuItem(lbl.c_str())) {
                project.SetConfiguration(cname);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_GEAR "  Manage configurations...")) {
            OpenManageConfigsModal();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    ImGui::SameLine(0.0f, Theme::Sp::S2);

    // 2. Toolchain Label & Selector (V2-14)
    if (show_comp_label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Col::TextDim, "%s", label_text);
        ImGui::SameLine(0.0f, Theme::Sp::S1);
    }

    auto merged_toolchains = compiler_manager.GetMergedToolchains(project);
    auto tc_res = compiler_manager.ResolveProjectToolchain(project);
    const bool has_compilers = !merged_toolchains.empty() || compiler_manager.HasCompilers();

    if (project.GetToolchain().empty() && project.GetCompiler().empty() && tc_res.found && !tc_res.profile.name.empty()) {
        project.SetCompiler(tc_res.profile.name);
    }

    std::string preview_text;
    if (!tc_res.profile.name.empty()) {
        preview_text = tc_res.profile.name;
        if (tc_res.isFallback) {
            preview_text += " (fallback)";
        }
    } else if (!project.GetToolchain().empty()) {
        preview_text = project.GetToolchain() + " (missing)";
    } else {
        preview_text = "No toolchain";
    }

    static ImVec2 s_CompAnchorMin(0, 0);
    static ImVec2 s_CompAnchorMax(0, 0);
    bool compiler_popup_open = ImGui::IsPopupOpen("CompilerSelectPopup");
    std::string tc_tip = tc_res.isFallback ? tc_res.bannerMessage : "Select compiler / cross toolchain (or open Toolchain Manager)";
    if (Theme::ComboButton("##CompilerSelector", preview_text.c_str(), tc_tip.c_str(), ImVec2(combo_width, build_btn_h), compiler_popup_open)) {
        s_CompAnchorMin = ImGui::GetItemRectMin();
        s_CompAnchorMax = ImGui::GetItemRectMax();
        ImGui::OpenPopup("CompilerSelectPopup");
    }

    ImGui::SetNextWindowPos(ImVec2(s_CompAnchorMin.x, s_CompAnchorMax.y + 4.0f * Theme::DpiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(combo_width, 0.0f), ImVec2(520.0f * Theme::DpiScale, 320.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Theme::DpiScale, 4.0f * Theme::DpiScale));
    if (ImGui::BeginPopup("CompilerSelectPopup")) {
        if (merged_toolchains.empty()) {
            ImGui::MenuItem("No compiler or toolchain detected", nullptr, false, false);
        } else {
            if (tc_res.isFallback && !tc_res.requestedName.empty()) {
                std::string missing_item = tc_res.requestedName + " (not found -> fallback: " + tc_res.profile.name + ")";
                ImGui::MenuItem(missing_item.c_str(), nullptr, false, false);
                ImGui::Separator();
            }
            for (size_t ti = 0; ti < merged_toolchains.size(); ++ti) {
                const auto& tc = merged_toolchains[ti];
                const bool is_selected = (tc.name == tc_res.profile.name);
                std::string suffix;
                if (tc.isProjectLocal) suffix = " [project]";
                else if (!tc.detected) suffix = " [user]";
                else if (!tc.targetTriple.empty() && tc.targetTriple.find("mingw") == std::string::npos && tc.targetTriple.find("msvc") == std::string::npos) suffix = " [cross]";
                std::string item_label = std::string(is_selected ? ICON_FA_CHECK "  " : "     ") + tc.name + suffix;
                ImGui::PushID(static_cast<int>(ti));
                if (ImGui::MenuItem(item_label.c_str(), nullptr, false)) {
                    project.SetToolchain(tc.name);
                }
                std::string tip = "Family: " + core::ToolchainFamilyToString(tc.family)
                                + "\nC++: " + tc.cxx
                                + (tc.targetTriple.empty() ? "" : ("\nTriple: " + tc.targetTriple))
                                + (tc.sysroot.empty() ? "" : ("\nSysroot: " + tc.sysroot))
                                + "\nLink with: " + tc.linkWith;
                Theme::Tooltip(tip.c_str());
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_GEAR "  Manage toolchains...")) {
            OpenToolchainManagerModal();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    ImGui::SameLine(0.0f, Theme::Sp::S2);

    // 3. Build / Rebuild / Clean Split Button (Spec §5.1.5)
    const bool is_building = build_runner.IsRunning();
    const bool is_scanning = project.IsScanning();
    const bool list_empty = project.GetFiles().empty() && (core::TrustStore::CountCustomCommands(project) == 0);
    const bool has_missing = project.HasMissingFiles();
    const bool compiler_valid = has_compilers && tc_res.found;
    const bool can_build = !is_building && !is_scanning && !list_empty && !has_missing && compiler_valid;

    if (is_building) {
        int elapsed_s = static_cast<int>(build_runner.GetElapsedSeconds());
        std::string stop_label = std::string(ICON_FA_XMARK) + " Stop " + std::to_string(elapsed_s) + "s";
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col::Danger);
        bool stop_bld = Theme::Button(stop_label.c_str(), "Cancel running build and kill child process trees (Ctrl+Break)", false, ImVec2(build_btn_w, build_btn_h));
        ImGui::PopStyleColor();
        if (stop_bld) {
            build_runner.CancelBuild();
        }
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
                for (size_t ti = 0; ti < targets.size(); ++ti) {
                    const auto& t = targets[ti];
                    std::string t_lbl = t.name + " (" + core::TargetKindToString(t.kind) + ")";
                    ImGui::PushID(static_cast<int>(ti));
                    if (ImGui::MenuItem(t_lbl.c_str(), nullptr, false, can_build)) {
                        TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, t.name);
                    }
                    ImGui::PopID();
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

        ImGui::Separator();
        bool keep_going = project.GetKeepGoing();
        if (ImGui::MenuItem("     Keep going on errors", nullptr, keep_going, !is_building)) {
            project.SetKeepGoing(!keep_going);
        }
        Theme::Tooltip("Continue compiling independent files/targets after a compile error");

        if (ImGui::MenuItem(ICON_FA_FILE_CODE "  Export compile_commands.json", nullptr, false, has_project)) {
            auto ex = core::CompDbManager::ExportCompileCommands(project, compiler_manager);
            core::OutLine ol;
            ol.kind = core::OutLineKind::Status;
            if (ex.success) {
                ol.text = "Exported " + std::to_string(ex.entryCount) + " compile_commands.json entries (" + ex.outputPath.generic_u8string() + ")";
            } else {
                ol.text = "compile_commands.json export failed: " + ex.error;
                ol.has_error = true;
            }
            output_lines.push_back(std::move(ol));
        }
        Theme::Tooltip("Write compile_commands.json for clangd/VS Code without building");

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    // 4. Run / Stop + Optional Debug Button (V2-16)
    if (run_runner != nullptr) {
        ImGui::SameLine(0.0f, Theme::Sp::S2);
        const bool is_running_proc = run_runner->IsRunning();
        const bool has_run_cmd = (startup_tgt != nullptr && !startup_tgt->run.command.empty());
        if (is_running_proc) {
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col::Danger);
            bool stop_clicked = Theme::Button(ICON_FA_XMARK " Stop", "Stop running target / emulator (Ctrl+R)", false, ImVec2(run_btn_w, build_btn_h));
            ImGui::PopStyleColor();
            if (stop_clicked) {
                run_runner->Stop();
            }
        } else if (!has_project || !has_run_cmd || is_building || is_scanning) {
            Theme::DisabledButton(ICON_FA_PLAY " Run", ImVec2(run_btn_w, build_btn_h), Theme::Col::TextDim, Theme::Col::Button, Theme::Col::Border);
            Theme::Tooltip(!has_project ? "Open a project first" : (!has_run_cmd ? "Configure a Run command in Build Settings -> Run / Debug" : "Wait for active build/scan to finish"));
        } else {
            if (Theme::SuccessButton(ICON_FA_PLAY " Run", "Run startup target / emulator (Ctrl+R)", ImVec2(run_btn_w, build_btn_h))) {
                if (run_lines && run_start_error) {
                    TriggerRunWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, *run_runner, *run_lines, *run_start_error, false);
                }
            }
        }

        if (has_debug_btn) {
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (is_running_proc || is_building || is_scanning) {
                Theme::DisabledButton("Debug", ImVec2(debug_btn_w, build_btn_h), Theme::Col::TextDim, Theme::Col::Button, Theme::Col::Border);
            } else if (Theme::Button("Debug", "Launch configured debug command", false, ImVec2(debug_btn_w, build_btn_h))) {
                if (run_lines && run_start_error) {
                    TriggerRunWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, *run_runner, *run_lines, *run_start_error, true);
                }
            }
        }
    }

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
    if (Theme::BeginModal("Build in Progress##ExitModal", "Stop build and quit?", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 420.0f, &exit_close_x)) {
        ImGui::TextColored(Theme::Col::Text, "A build is currently running.");
        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::TextDim, "Stop active compiler processes and exit, or keep building?");

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float dpi = Theme::DpiScale;
        const float btn_h = Theme::Btn::DefaultH;
        const float keep_w = 136.0f * dpi;
        const float exit_w = 108.0f * dpi;
        const float right_end = 420.0f * dpi - Theme::Sp::S4;

        ImGui::SetCursorPosX(right_end - keep_w - Theme::Sp::S2 - exit_w);
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col::Danger);
        bool stop_quit = Theme::Button("Stop & quit", "Terminate running build processes and exit immediately", false, ImVec2(exit_w, btn_h));
        ImGui::PopStyleColor();
        if (stop_quit) {
            build_runner.CancelBuild();
            s_ShowExitWhileBuildingModal = false;
            ImGui::CloseCurrentPopup();
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);
        if (Theme::Button("Keep building", "Keep build running", true, ImVec2(keep_w, btn_h)) || exit_close_x) {
            s_ShowExitWhileBuildingModal = false;
            ImGui::CloseCurrentPopup();
        }

        Theme::EndModal();
    }

    // Trust prompt on first open of a project with custom build steps (V2-6 security)
    if (has_project && !project.IsScanning()) {
        std::string cur_root_str = project.GetRootPath().generic_u8string();
        if (cur_root_str != s_PromptedTrustRoot) {
            s_PromptedTrustRoot = cur_root_str;
            if (core::TrustStore::CountCustomCommands(project) > 0 && !core::TrustStore::IsTrusted(project.GetRootPath())) {
                s_TrustPendingBuild = false;
                s_TrustPendingRebuild = false;
                s_TrustPendingTarget.clear();
                s_ShowTrustModal = true;
            }
        }
    }

    if (s_ShowTrustModal) {
        ImGui::OpenPopup("Trust Project Folder##Modal");
    }
    bool trust_close_x = false;
    if (Theme::BeginModal("Trust Project Folder##Modal", "Trust Project Folder?", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 520.0f, &trust_close_x)) {
        size_t cmd_count = core::TrustStore::CountCustomCommands(project);
        ImGui::TextWrapped("This project runs %zu custom command(s) during builds. Trust this folder?", cmd_count);
        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::TextDim, "%s", project.GetRootPath().generic_u8string().c_str());
        ImGui::Spacing();

        ImGui::BeginChild("TrustCmdPreview", ImVec2(0.0f, 130.0f * Theme::DpiScale), true);
        ImGui::PushFont(g_Fonts.mono);
        for (const auto& t : project.GetTargets()) {
            for (const auto& st : t.steps) {
                ImGui::TextColored(Theme::Col::Accent, "[%s / %s]", t.name.c_str(), st.name.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(st.command.c_str());
            }
        }
        ImGui::PopFont();
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::TextDim, "Untrusted projects can be browsed, but cannot be built.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float dpi = Theme::DpiScale;
        const float btn_h = Theme::Btn::DefaultH;
        if (Theme::Button("Don't Trust (Browse Only)", "Keep folder untrusted and cancel build", false, ImVec2(0.0f, btn_h)) || trust_close_x) {
            s_ShowTrustModal = false;
            s_TrustPendingBuild = false;
            s_TrustPendingRebuild = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Theme::SuccessButton("Trust Folder", "Store trust in %APPDATA%/GenieBuild/trust.json", ImVec2(0.0f, btn_h))) {
            core::TrustStore::SetTrusted(project.GetRootPath(), true);
            s_ShowTrustModal = false;
            ImGui::CloseCurrentPopup();
            if (s_TrustPendingBuild) {
                s_TrustPendingBuild = false;
                TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, s_TrustPendingTarget);
            } else if (s_TrustPendingRebuild) {
                s_TrustPendingRebuild = false;
                TriggerRebuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, s_TrustPendingTarget);
            }
        }
        Theme::EndModal();
    }

    // Running Executable Conflict Modal on Windows (V2-16)
    if (s_ShowRunningExeConflictModal) {
        ImGui::OpenPopup("Running Executable Conflict##Modal");
    }
    bool conflict_close_x = false;
    if (Theme::BeginModal("Running Executable Conflict##Modal", "Target Binary In Use", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 470.0f, &conflict_close_x)) {
        ImGui::TextWrapped("Target binary is currently running and locked by Windows:");
        ImGui::Spacing();
        ImGui::TextColored(Theme::Col::TextDim, "%s", s_ConflictExePath.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Stop the running process so linking can overwrite the executable?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float btn_h = Theme::Btn::DefaultH;
        if (Theme::Button("Cancel##RunConflict", "Keep running and cancel build", false, ImVec2(100.0f * Theme::DpiScale, btn_h)) || conflict_close_x) {
            s_ShowRunningExeConflictModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col::Danger);
        bool stop_build_clicked = Theme::Button("Stop & build", "Terminate running process and start build", false, ImVec2(130.0f * Theme::DpiScale, btn_h));
        ImGui::PopStyleColor();
        if (stop_build_clicked) {
            if (s_ActiveRunRunner) {
                s_ActiveRunRunner->Stop();
            }
            s_ShowRunningExeConflictModal = false;
            ImGui::CloseCurrentPopup();
            if (s_ConflictIsRebuild) {
                TriggerRebuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, s_ConflictTarget);
            } else {
                TriggerBuildWithGuard(project, compiler_manager, build_runner, build_start_error, output_lines, s_ConflictTarget);
            }
        }
        Theme::EndModal();
    }

    // Slim dismissible banners for VS-found (V2-17), Toolchain Fallback (V2-14), and Freestanding-on-PE (V2-15)
    {
        std::string vs_banner = core::MsvcEnvDetector::GetReadyBannerText(compiler_manager.GetMsvcEnvState());
        bool has_msvc_active = (tc_res.profile.family == core::ToolchainFamily::Msvc || tc_res.profile.family == core::ToolchainFamily::ClangCl);
        bool show_vs = has_project && !vs_banner.empty() && !has_msvc_active;

        std::string fb_key = tc_res.requestedName + "->" + tc_res.profile.name;
        bool show_fb = has_project && tc_res.isFallback && !tc_res.bannerMessage.empty() && (s_DismissedFallbackKey != fb_key);

        bool any_freestanding = false;
        for (const auto& t : project.GetTargets()) {
            if (t.freestanding) {
                any_freestanding = true;
                break;
            }
        }
        std::string pe_key = tc_res.profile.name;
        bool show_pe = has_project && any_freestanding && (tc_res.profile.os == "windows") && (s_DismissedPeWarnKey != pe_key);

        if (!s_LongPathsChecked) {
            s_LongPathsEnabledCached = core::fs_util::IsWindowsLongPathsEnabled();
            s_LongPathsChecked = true;
        }
        bool deep_root = has_project && (project.GetRootPath().generic_u8string().size() >= 150);
        static core::UserSettings s_BannerPrefs;
        static bool s_BannerPrefsLoaded = false;
        if (!s_BannerPrefsLoaded) {
            s_BannerPrefs = core::HintsManager::LoadUserSettings();
            s_BannerPrefsLoaded = true;
        }
        bool show_lp = deep_root && !s_LongPathsEnabledCached && !s_BannerPrefs.longPathBannerDismissed;
        bool show_def = has_project && !s_DismissedDefenderSession &&
                        (build_runner.GetStatus() == core::BuildStatus::Finished) &&
                        build_runner.GetLastResult().defender_hint_recommended;

        if (show_vs || show_fb || show_pe || show_lp || show_def || s_ShowImportChecklistBanner) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float banner_x = vp->WorkPos.x + Theme::Sp::S3;
            const float banner_y = vp->WorkPos.y + Theme::TitleBarHeight + Theme::ToolBarHeight + 4.0f * Theme::DpiScale;
            ImGui::SetNextWindowPos(ImVec2(banner_x, banner_y), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.95f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * Theme::DpiScale, 6.0f * Theme::DpiScale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::CardRounding);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Col::BgExplorer);
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Col::Border);
            ImGuiWindowFlags bflags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            if (ImGui::Begin("##ToolchainBannersStrip", nullptr, bflags)) {
                if (show_vs) {
                    ImGui::TextColored(Theme::Col::Accent, "%s", vs_banner.c_str());
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (Theme::Button("Use MSVC##UseMsvc", "Switch project toolchain to detected MSVC", true, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        for (const auto& tc : merged_toolchains) {
                            if (tc.family == core::ToolchainFamily::Msvc) {
                                project.SetToolchain(tc.name);
                                break;
                            }
                        }
                        compiler_manager.DismissMsvcBanner();
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button("Dismiss##VsDis", "Dismiss VS notification", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        compiler_manager.DismissMsvcBanner();
                    }
                }
                if (show_fb) {
                    ImGui::TextColored(Theme::Col::Warning, "%s", tc_res.bannerMessage.c_str());
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (Theme::Button("Manage Toolchains...##FbMgr", "Open Toolchain Manager", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        OpenToolchainManagerModal();
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button("Dismiss##FbDis", "Dismiss banner", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_DismissedFallbackKey = fb_key;
                    }
                }
                if (show_pe) {
                    ImGui::TextColored(Theme::Col::Warning, "Kernel targets ELF/multiboot, but '%s' produces Windows PE. Select an x86_64-elf / i686-elf cross toolchain.", tc_res.profile.name.c_str());
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (Theme::Button("Manage Toolchains...##PeMgr", "Open Toolchain Manager", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        OpenToolchainManagerModal();
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button("Dismiss##PeDis", "Dismiss warning", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_DismissedPeWarnKey = pe_key;
                    }
                }
                if (show_lp) {
                    ImGui::TextColored(Theme::Col::Warning, "Windows LongPathsEnabled is 0. GenieBuild shortens object paths automatically, or enable system long paths.");
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (Theme::Button("How to Enable##LpHow", "Show exact Windows Long Paths instructions", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_ShowLongPathHelpModal = true;
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button("Dismiss##LpDis", "Don't show long-path banner again", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_BannerPrefs.longPathBannerDismissed = true;
                        core::HintsManager::SaveUserSettings(s_BannerPrefs);
                    }
                }
                if (show_def) {
                    ImGui::TextColored(Theme::Col::Warning, "Builds may be slowed by antivirus scanning. You can exclude .geniebuild/ or use a Dev Drive.");
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (Theme::Button("Learn how##DefHow", "Show PowerShell exclusion command & Dev Drive info", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_ShowDefenderHelpModal = true;
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button("Don't show again##DefDis", "Dismiss Defender hint for this project", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        core::HintsManager::DismissDefenderForProject(project.GetRootPath(), false);
                        s_DismissedDefenderSession = true;
                    }
                }
                if (s_ShowImportChecklistBanner) {
                    ImGui::TextColored(Theme::Col::Accent, "Imported project.json: set target kinds, dependencies, and libraries in Settings, then build.");
                    ImGui::SameLine(0.0f, Theme::Sp::S2);
                    if (Theme::Button("Open Settings##ImpSet", "Review imported targets", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        OpenBuildSettingsModal();
                    }
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    if (Theme::Button("Dismiss##ImpDis", "Dismiss checklist banner", false, ImVec2(0.0f, Theme::Btn::SmallH))) {
                        s_ShowImportChecklistBanner = false;
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
    }

    // V2-22 Long Paths Help Modal
    if (s_ShowLongPathHelpModal) ImGui::OpenPopup("Enable Windows Long Paths##Modal");
    bool lp_close = false;
    if (Theme::BeginModal("Enable Windows Long Paths##Modal", "Enable Windows Long Paths", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 540.0f, &lp_close)) {
        ImGui::TextWrapped("To enable Win32 long paths system-wide (admin PowerShell command to run yourself):");
        ImGui::Spacing();
        const char* lp_cmd = "New-ItemProperty -Path \"HKLM:\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\" -Name \"LongPathsEnabled\" -Value 1 -PropertyType DWORD -Force";
        ImGui::PushFont(g_Fonts.mono);
        ImGui::TextWrapped("%s", lp_cmd);
        ImGui::PopFont();
        ImGui::Spacing();
        if (Theme::Button("Copy PowerShell Command##LpCopy", "Copy command to clipboard", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
            ImGui::SetClipboardText(lp_cmd);
        }
        ImGui::SameLine();
        if (Theme::Button("Close##LpClose", "Close", true, ImVec2(90.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || lp_close) {
            s_ShowLongPathHelpModal = false;
            ImGui::CloseCurrentPopup();
        }
        Theme::EndModal();
    }

    // V2-23 Windows Defender Help Modal
    if (s_ShowDefenderHelpModal) ImGui::OpenPopup("Antivirus Performance Hint##Modal");
    bool def_close = false;
    if (Theme::BeginModal("Antivirus Performance Hint##Modal", "Speed Up Builds on Windows", ICON_FA_TRIANGLE_EXCLAMATION, Theme::Col::Warning, 560.0f, &def_close)) {
        std::string gb_dir = (project.GetRootPath() / ".geniebuild").generic_u8string();
        std::string ps_cmd = "Add-MpPreference -ExclusionPath \"" + gb_dir + "\"";
        ImGui::TextWrapped("Real-time scanning of intermediate .o/.obj/.pch writes can slow large builds. GenieBuild never changes security settings automatically; run this in an admin PowerShell or move your repo onto a Windows Dev Drive (ReFS):");
        ImGui::Spacing();
        ImGui::PushFont(g_Fonts.mono);
        ImGui::TextWrapped("%s", ps_cmd.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        if (Theme::Button("Copy PowerShell Command##DefCopy", "Copy exclusion command", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
            ImGui::SetClipboardText(ps_cmd.c_str());
        }
        ImGui::SameLine();
        if (Theme::Button("Don't show for any project", "Suppress hint globally", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
            core::HintsManager::DismissDefenderForProject(project.GetRootPath(), true);
            s_DismissedDefenderSession = true;
            s_ShowDefenderHelpModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Theme::Button("Close##DefClose", "Close", true, ImVec2(90.0f * Theme::DpiScale, Theme::Btn::DefaultH)) || def_close) {
            s_ShowDefenderHelpModal = false;
            ImGui::CloseCurrentPopup();
        }
        Theme::EndModal();
    }

    // V2-24 Import compile_commands.json Modal
    if (s_ShowImportModal) ImGui::OpenPopup("Import compile_commands.json##Modal");
    bool imp_close = false;
    if (Theme::BeginModal("Import compile_commands.json##Modal", "Import from compile_commands.json", ICON_FA_CODE, Theme::Col::Accent, 780.0f, &imp_close)) {
        const float dpi = Theme::DpiScale;
        ImGui::TextColored(Theme::Col::TextDim, "Project root:");
        ImGui::SameLine();
        char rbuf[512];
        std::strncpy(rbuf, s_ImportRootBuf.c_str(), sizeof(rbuf) - 1);
        rbuf[sizeof(rbuf) - 1] = '\0';
        ImGui::SetNextItemWidth(std::max(180.0f * dpi, ImGui::GetContentRegionAvail().x - 160.0f * dpi));
        if (ImGui::InputText("##ImpRoot", rbuf, sizeof(rbuf))) {
            s_ImportRootBuf = rbuf;
        }
        ImGui::SameLine();
        if (Theme::Button("Re-analyze##ImpRe", "Re-analyze with new project root", false, ImVec2(0.0f, Theme::Btn::DefaultH))) {
            s_ImportPlan = core::CompDbManager::AnalyzeCompileCommands(s_ImportPlan.compdbPath, std::filesystem::u8path(s_ImportRootBuf));
            s_ImportSelectedTarget = 0;
        }

        if (!s_ImportPlan.success) {
            ImGui::TextColored(Theme::Col::Error, "%s", s_ImportPlan.error.c_str());
        } else {
            ImGui::TextColored(Theme::Col::TextDim, "Parsed %zu entries -> %zu suggested targets (%zu generated excluded).",
                s_ImportPlan.totalEntriesParsed, s_ImportPlan.targets.size(), s_ImportPlan.excludedGeneratedCount);

            // Left: targets list
            ImGui::BeginChild("ImpTargetsLeft", ImVec2(240.0f * dpi, 320.0f * dpi), true);
            for (size_t i = 0; i < s_ImportPlan.targets.size(); ++i) {
                const auto& ts = s_ImportPlan.targets[i];
                std::string lbl = ts.name + " (" + core::TargetKindToString(ts.kind) + ", " + std::to_string(ts.files.size()) + " files)";
                if (ImGui::Selectable((lbl + "##impt" + std::to_string(i)).c_str(), s_ImportSelectedTarget == static_cast<int>(i))) {
                    s_ImportSelectedTarget = static_cast<int>(i);
                }
            }
            ImGui::EndChild();
            ImGui::SameLine(0.0f, Theme::Sp::S2);

            // Right: selected target & factored flags preview
            ImGui::BeginChild("ImpTargetRight", ImVec2(0.0f, 320.0f * dpi), true);
            if (s_ImportSelectedTarget >= 0 && s_ImportSelectedTarget < static_cast<int>(s_ImportPlan.targets.size())) {
                auto& ts = s_ImportPlan.targets[static_cast<size_t>(s_ImportSelectedTarget)];
                char nbuf[128];
                std::strncpy(nbuf, ts.name.c_str(), sizeof(nbuf) - 1);
                nbuf[sizeof(nbuf) - 1] = '\0';
                ImGui::TextColored(Theme::Col::TextDim, "Target name:");
                ImGui::SetNextItemWidth(220.0f * dpi);
                if (ImGui::InputText("##ImpTName", nbuf, sizeof(nbuf))) ts.name = nbuf;

                const char* kind_items[] = { "executable", "static", "shared", "interface" };
                int kidx = static_cast<int>(ts.kind);
                ImGui::TextColored(Theme::Col::TextDim, "Target kind:");
                ImGui::SetNextItemWidth(180.0f * dpi);
                if (ImGui::Combo("##ImpTKind", &kidx, kind_items, 4)) {
                    ts.kind = static_cast<core::TargetKind>(kidx);
                }

                ImGui::Separator();
                ImGui::TextColored(Theme::Col::Text, "Factored Project Includes (%zu), Defines (%zu), Flags (%zu)",
                    s_ImportPlan.projectIncludeDirs.size(), s_ImportPlan.projectDefines.size(), s_ImportPlan.projectCompileFlags.size());
                ImGui::TextColored(Theme::Col::Text, "Target Includes (%zu), Defines (%zu), Flags (%zu), Folder groups (%zu)",
                    ts.includeDirs.size(), ts.defines.size(), ts.compileFlags.size(), ts.folders.size());
                for (const auto& inc : ts.includeDirs) {
                    ImGui::TextColored(Theme::Col::TextDim, "  -I %s", inc.c_str());
                }
                for (const auto& fg : ts.folders) {
                    if (!fg.includeDirs.empty() || !fg.defines.empty() || !fg.compileFlags.empty()) {
                        ImGui::TextColored(Theme::Col::Accent, "  [override %s] +%zu inc, +%zu def, +%zu flags",
                            fg.folderRel.c_str(), fg.includeDirs.size(), fg.defines.size(), fg.compileFlags.size());
                    }
                }
            }
            ImGui::EndChild();

            if (!s_ImportPlan.warningsList.empty()) {
                ImGui::BeginChild("ImpWarnings", ImVec2(0.0f, 68.0f * dpi), true);
                for (const auto& w : s_ImportPlan.warningsList) {
                    ImGui::TextColored(Theme::Col::Warning, "! %s", w.c_str());
                }
                ImGui::EndChild();
            }
        }

        if (!s_ImportWriteError.empty()) {
            ImGui::TextColored(Theme::Col::Error, "%s", s_ImportWriteError.c_str());
        }

        if (Theme::Button("Cancel##ImpCancel", "Cancel import", false, ImVec2(90.0f * dpi, Theme::Btn::DefaultH)) || imp_close) {
            s_ShowImportModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (!s_ImportPlan.success) ImGui::BeginDisabled(true);
        if (Theme::SuccessButton("Write project.json", "Backup existing project.json, write v2 project.json, and open", ImVec2(150.0f * dpi, Theme::Btn::DefaultH))) {
            std::filesystem::path rpath = std::filesystem::u8path(s_ImportRootBuf);
            if (core::CompDbManager::WriteImportedProject(s_ImportPlan, rpath, s_ImportWriteError)) {
                project.Load(rpath);
                RefreshFileTree();
                output_lines.clear();
                build_start_error.clear();
                s_ShowImportModal = false;
                s_ShowImportChecklistBanner = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!s_ImportPlan.success) ImGui::EndDisabled();
        Theme::EndModal();
    }

    // Build Settings Modal (Spec §5.1.1)
    RenderBuildSettingsModal(project, compiler_manager);
}

void RenderTopBar(
    GLFWwindow* window,
    core::Project& project,
    core::CompilerManager& compiler_manager,
    core::BuildRunner& build_runner,
    std::string& build_start_error,
    std::vector<core::OutLine>& output_lines,
    core::RunRunner* run_runner,
    std::vector<core::OutLine>* run_lines,
    std::string* run_start_error
) {
    RenderTitleBar(window, project);
    ImGui::Spacing();
    RenderToolBar(window, project, compiler_manager, build_runner, build_start_error, output_lines, run_runner, run_lines, run_start_error);
}

} // namespace ui
