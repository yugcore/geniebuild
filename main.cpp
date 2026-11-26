#include <iostream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shobjidl.h>
#else
#include <GLFW/glfw3.h>
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.h>

// Core project management
#include "core/project.h"
#include "core/compiler.h"
#include "core/build_runner.h"
#include "core/build_steps.h"
#include "core/run_runner.h"

// UI panel components
#include "ui/theme.h"
#include "ui/top_bar.h"
#include "ui/file_tree_panel.h"
#include "ui/build_list_panel.h"
#include "ui/output_panel.h"
#include "ui/build_settings_modal.h"

// Global project, compiler, and build execution state
static core::Project g_Project;
static core::CompilerManager g_CompilerManager;
static core::BuildRunner g_BuildRunner;
static std::vector<core::OutLine> g_OutputLines;
static std::string g_BuildStartError;
static core::RunRunner g_RunRunner;
static std::vector<core::OutLine> g_RunLines;
static std::string g_RunStartError;
static float g_PendingDpiScale = -1.0f;
static float g_DpiScale = 1.0f;
static GLFWwindow* g_Window = nullptr;

static void RenderFrame(GLFWwindow* window);

static void GlfwWindowRefreshCallback(GLFWwindow* window) {
    RenderFrame(window);
}

// Callback for reporting GLFW errors to standard error
static void GlfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << "\n";
}

// Live DPI change callback (Spec §1.7)
static void GlfwContentScaleCallback(GLFWwindow* window, float xscale, float yscale) {
    (void)window;
    (void)yscale;
    if (xscale > 0.0f) {
        g_PendingDpiScale = xscale;
    }
}

// Drag-and-drop of a folder onto the window opens that project
static void GlfwDropCallback(GLFWwindow* window, int count, const char** paths) {
    (void)window;
    if (count > 0 && paths && paths[0]) {
        std::filesystem::path dropped_path(paths[0]);
        std::error_code ec;
        if (std::filesystem::is_directory(dropped_path, ec)) {
            g_Project.Load(dropped_path);
            ui::RefreshFileTree();
            g_OutputLines.clear();
            g_BuildStartError.clear();
        }
    }
}

#if defined(_WIN32)
static WNDPROC g_OriginalWndProc = nullptr;

// Subclass window procedure to enable native Windows edge resizing and borderless frame
static LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            if (IsZoomed(hwnd)) {
                // When maximized, prevent window from extending beyond screen work area
                NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfo(hMon, &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0; // Removes OS title bar and client edge borders
        }
        break;
    }
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rect;
        GetWindowRect(hwnd, &rect);

        // 1. Edge resizing hit-testing when not maximized
        if (!IsZoomed(hwnd)) {
            const int border = 7; // Resize border hit thickness in pixels

            bool left   = (pt.x >= rect.left && pt.x < rect.left + border);
            bool right  = (pt.x < rect.right && pt.x >= rect.right - border);
            bool top    = (pt.y >= rect.top && pt.y < rect.top + border);
            bool bottom = (pt.y < rect.bottom && pt.y >= rect.bottom - border);

            if (top && left)     return HTTOPLEFT;
            if (top && right)    return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left)            return HTLEFT;
            if (right)           return HTRIGHT;
            if (top)             return HTTOP;
            if (bottom)          return HTBOTTOM;
        }

        // 2. Title bar header hit-testing (Spec §1.5)
        POINT client_pt = pt;
        ScreenToClient(hwnd, &client_pt);
        if (client_pt.y >= 0 && client_pt.y < ui::g_ChromeHitTest.header_height) {
            // Check Maximize button -> HTMAXBUTTON (enables Win11 Snap Layouts flyout!)
            if (PtInRect(&ui::g_ChromeHitTest.max_button_rect, client_pt)) {
                return HTMAXBUTTON;
            }

            // Check if cursor is over interactive items (buttons, combo, input text)
            for (const auto& r : ui::g_ChromeHitTest.interactive_rects) {
                if (PtInRect(&r, client_pt)) {
                    return HTCLIENT;
                }
            }

            // Empty header space is a draggable caption
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN: {
        if (wParam == HTMAXBUTTON) {
            return 0; // Handled
        }
        break;
    }
    case WM_NCLBUTTONUP: {
        if (wParam == HTMAXBUTTON) {
            if (IsZoomed(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            } else {
                ShowWindow(hwnd, SW_MAXIMIZE);
            }
            return 0;
        }
        break;
    }
    case WM_ENTERSIZEMOVE: {
        // Live rendering during window resize (Spec §6.1 P1)
        SetTimer(hwnd, 1, 10, nullptr);
        break;
    }
    case WM_TIMER: {
        if (wParam == 1 && g_Window != nullptr) {
            RenderFrame(g_Window);
        }
        break;
    }
    case WM_EXITSIZEMOVE: {
        KillTimer(hwnd, 1);
        break;
    }
    }
    return CallWindowProc(g_OriginalWndProc, hwnd, uMsg, wParam, lParam);
}
#endif

// Draggable splitter widget between child panels (colors come from Theme tokens per §1.4 & §4)
static bool RenderSplitter(const char* id, float* left_w, float* right_w, float min_left, float min_right, float thickness, float height) {
    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(thickness, height));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (active) {
        float delta = ImGui::GetIO().MouseDelta.x;
        if (delta != 0.0f) {
            if (delta < 0.0f) {
                float max_shrink = *left_w - min_left;
                if (max_shrink > 0.0f) {
                    float move = std::max(delta, -max_shrink);
                    *left_w  += move;
                    *right_w -= move;
                }
            } else {
                float max_shrink = *right_w - min_right;
                if (max_shrink > 0.0f) {
                    float move = std::min(delta, max_shrink);
                    *left_w  += move;
                    *right_w -= move;
                }
            }
        }
    }

    ImU32 col = active ? ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::Accent)
              : hovered ? ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::TextDim)
                        : ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::Border);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float mid_x = screen_pos.x + thickness * 0.5f;
    draw_list->AddLine(ImVec2(mid_x, screen_pos.y), ImVec2(mid_x, screen_pos.y + height), col, (active || hovered) ? 2.0f : 1.0f);

    return ImGui::IsItemDeactivated();
}

// Draggable main splitter between the merged left card (Explorer+BuildList) and the Output terminal card
static bool RenderMainSplitter(const char* id, float* w0, float* w1, float* w2, float min_each, float thickness, float height) {
    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(thickness, height));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (active) {
        float delta = ImGui::GetIO().MouseDelta.x;
        if (delta != 0.0f) {
            float left_total = *w0 + *w1;
            float min_left = min_each * 2.0f;
            float move = delta;
            if (move < 0.0f) {
                float max_shrink = left_total - min_left;
                move = (max_shrink > 0.0f) ? std::max(move, -max_shrink) : 0.0f;
            } else {
                float max_shrink = *w2 - min_each;
                move = (max_shrink > 0.0f) ? std::min(move, max_shrink) : 0.0f;
            }
            if (move != 0.0f && left_total > 0.0f) {
                float new_left = left_total + move;
                float r0 = *w0 / left_total;
                float new_w0 = std::clamp(new_left * r0, min_each, new_left - min_each);
                *w0 = new_w0;
                *w1 = new_left - new_w0;
                *w2 -= move;
            }
        }
    }

    // Subtle centered pill grip handle between the two cards
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float mid_x = screen_pos.x + thickness * 0.5f;
    float mid_y = screen_pos.y + height * 0.5f;
    float grip_h = 18.0f * ui::Theme::DpiScale;
    float grip_w = (active || hovered) ? 1.5f * ui::Theme::DpiScale : 1.0f * ui::Theme::DpiScale;
    ImU32 col = active ? ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::Accent)
              : hovered ? ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::TextDim)
                        : ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::Border);
    draw_list->AddRectFilled(
        ImVec2(mid_x - grip_w, mid_y - grip_h),
        ImVec2(mid_x + grip_w, mid_y + grip_h),
        col,
        grip_w
    );

    return ImGui::IsItemDeactivated();
}

static void RenderFrame(GLFWwindow* window) {
    static bool s_InFrame = false;
    if (s_InFrame || !window) return;
    s_InFrame = true;

    // Integrate any background scan results and flush debounced save (§2.8 & §2.4)
    g_Project.Update();

    // Handle live DPI scale change (Spec §1.7)
    ImGuiIO& io = ImGui::GetIO();
    if (g_PendingDpiScale > 0.0f && std::abs(g_PendingDpiScale - g_DpiScale) > 0.01f) {
        g_DpiScale = g_PendingDpiScale;
        g_PendingDpiScale = -1.0f;

        ImGui_ImplOpenGL3_DestroyDeviceObjects();
        io.Fonts->Clear();
        ui::apply_theme(g_DpiScale);
        ImGui_ImplOpenGL3_CreateDeviceObjects();
    }

    // Poll output lines from background build thread each frame
    std::vector<core::OutLine> drained = g_BuildRunner.DrainOutput();
    if (!drained.empty()) {
        g_OutputLines.insert(g_OutputLines.end(), drained.begin(), drained.end());

        // Cap in-memory lines to 200,000 (Spec §2.7; log file keeps everything)
        constexpr size_t MAX_OUTPUT_LINES = 200000;
        if (g_OutputLines.size() > MAX_OUTPUT_LINES) {
            size_t excess = g_OutputLines.size() - MAX_OUTPUT_LINES;
            g_OutputLines.erase(g_OutputLines.begin(), g_OutputLines.begin() + excess);
        }
    }

    // Poll output lines from background Run / Emulator process each frame (V2-16)
    std::vector<core::OutLine> run_drained = g_RunRunner.DrainOutput();
    if (!run_drained.empty()) {
        g_RunLines.insert(g_RunLines.end(), run_drained.begin(), run_drained.end());
        constexpr size_t MAX_RUN_LINES = 100000;
        if (g_RunLines.size() > MAX_RUN_LINES) {
            size_t excess = g_RunLines.size() - MAX_RUN_LINES;
            g_RunLines.erase(g_RunLines.begin(), g_RunLines.begin() + excess);
        }
    }

    // Start new ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Global build & run shortcuts (Ctrl+B, F5, Ctrl+Shift+B, Ctrl+Break, Ctrl+R, Ctrl+Shift+T, Ctrl+,)
    bool ctrl_shift_b = (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B));
    bool ctrl_shift_t = (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_T));
    bool ctrl_break = (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Pause));
    bool ctrl_b = (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B));
    bool ctrl_r = (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R));
    bool ctrl_comma = (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma));
    bool f5 = ImGui::IsKeyPressed(ImGuiKey_F5);

    ui::SetActiveRunRunner(&g_RunRunner);

    if (ctrl_break && g_BuildRunner.IsRunning()) {
        g_BuildRunner.CancelBuild();
    }

    if (ctrl_comma && !io.WantTextInput) {
        ui::OpenBuildSettingsModal();
    }
    if (ctrl_shift_t && !io.WantTextInput && g_Project.HasProject()) {
        ui::OpenNewTargetWizard();
    }

    if (ctrl_r && !io.WantTextInput && g_Project.HasProject() && !g_Project.IsScanning()) {
        if (g_RunRunner.IsRunning()) {
            g_RunRunner.Stop();
        } else if (!g_BuildRunner.IsRunning()) {
            ui::TriggerRunWithGuard(
                g_Project, g_CompilerManager, g_BuildRunner,
                g_BuildStartError, g_OutputLines,
                g_RunRunner, g_RunLines, g_RunStartError, false
            );
        }
    } else if (ctrl_shift_b && !g_BuildRunner.IsRunning() && !io.WantTextInput && !g_Project.IsScanning()) {
        if (!g_Project.GetFiles().empty() && !g_Project.HasMissingFiles() && g_CompilerManager.HasCompilers()) {
            ui::TriggerRebuildWithGuard(g_Project, g_CompilerManager, g_BuildRunner, g_BuildStartError, g_OutputLines);
        }
    } else if ((ctrl_b || f5) && !g_BuildRunner.IsRunning() && !io.WantTextInput && !g_Project.IsScanning()) {
        if (!g_Project.GetFiles().empty() && !g_Project.HasMissingFiles() && g_CompilerManager.HasCompilers()) {
            ui::TriggerBuildWithGuard(g_Project, g_CompilerManager, g_BuildRunner, g_BuildStartError, g_OutputLines);
        }
    }

    // Create a full-window viewport container so the UI fits the window automatically
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags root_window_flags = ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    if (ImGui::Begin("GenieBuildRoot", nullptr, root_window_flags)) {
        const float title_bar_bottom_y = ui::Theme::TitleBarHeight;
        const float toolbar_top_y = title_bar_bottom_y + 1.0f;
        const float toolbar_bottom_y = toolbar_top_y + ui::Theme::ToolBarHeight;

        // Top chrome backdrop (Title Bar + Toolbar surface)
        ImDrawList* root_dl = ImGui::GetWindowDrawList();
        root_dl->AddRectFilled(
            ImVec2(0.0f, 0.0f),
            ImVec2(ImGui::GetWindowWidth(), toolbar_bottom_y),
            ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::BgChrome)
        );

        // 1. Top Bar 1: Dedicated Window Title Bar (centered GenieBuild · Project)
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ui::RenderTitleBar(window, g_Project);

        // Faint 1px divider below Title Bar
        ImGui::SetCursorPos(ImVec2(0.0f, title_bar_bottom_y));
        ImGui::PushStyleColor(ImGuiCol_Separator, ui::Theme::Col::Border);
        ImGui::Separator();
        ImGui::PopStyleColor();

        // 2. Top Bar 2: Dedicated Action & Workflow Toolbar
        ImGui::SetCursorPos(ImVec2(ui::Theme::Sp::S3, toolbar_top_y + (ui::Theme::ToolBarHeight - ui::Theme::Btn::DefaultH) * 0.5f));
        ui::RenderToolBar(
            window, g_Project, g_CompilerManager, g_BuildRunner,
            g_BuildStartError, g_OutputLines,
            &g_RunRunner, &g_RunLines, &g_RunStartError
        );

        // Faint 1px divider below Toolbar
        ImGui::SetCursorPos(ImVec2(0.0f, toolbar_bottom_y));
        ImGui::PushStyleColor(ImGuiCol_Separator, ui::Theme::Col::Border);
        ImGui::Separator();
        ImGui::PopStyleColor();

        // 3. Layout: Merged Left Rounded Card (Explorer | split | Build List) + Output Terminal Card on Right
        const float outer_margin    = ui::Theme::Sp::S3;
        const float inner_split_w   = 4.0f * ui::Theme::DpiScale;
        const float main_splitter_w = ui::Theme::SplitterWidth;
        const float panes_top_y     = toolbar_bottom_y + 1.0f + ui::Theme::Sp::S2;
        const float min_pane_w      = ui::Theme::Dim::MinPaneW;

        float total_content_w = ImGui::GetWindowWidth() - (outer_margin * 2.0f);
        float total_panes_w   = total_content_w - inner_split_w - main_splitter_w;
        float panes_height    = ImGui::GetWindowHeight() - panes_top_y - outer_margin;

        static float s_PaneW0 = -1.0f;
        static float s_PaneW1 = -1.0f;
        static float s_PaneW2 = -1.0f;
        static std::filesystem::path s_LastLoadedRoot;

        if (s_PaneW0 < 0.0f || g_Project.GetRootPath() != s_LastLoadedRoot) {
            s_LastLoadedRoot = g_Project.GetRootPath();
            const auto& saved = g_Project.GetPaneWidths();
            if (saved.size() >= 3 && saved[0] > 0.0f && saved[1] > 0.0f && saved[2] > 0.0f) {
                float r0 = saved[0];
                float r1 = saved[1];
                float r2 = saved[2];
                if (r0 > 1.0f || r1 > 1.0f || r2 > 1.0f) {
                    float sum = r0 + r1 + r2;
                    r0 /= sum; r1 /= sum; r2 /= sum;
                }
                s_PaneW0 = total_panes_w * r0;
                s_PaneW1 = total_panes_w * r1;
                s_PaneW2 = total_panes_w * r2;
            } else {
                s_PaneW0 = total_panes_w * 0.25f;
                s_PaneW1 = total_panes_w * 0.30f;
                s_PaneW2 = total_panes_w - s_PaneW0 - s_PaneW1;
            }
        } else {
            float cur_sum = s_PaneW0 + s_PaneW1 + s_PaneW2;
            if (std::abs(cur_sum - total_panes_w) > 1.0f && cur_sum > 0.0f) {
                float factor = total_panes_w / cur_sum;
                s_PaneW0 *= factor;
                s_PaneW1 *= factor;
                s_PaneW2 = total_panes_w - s_PaneW0 - s_PaneW1;
            }
        }

        if (s_PaneW0 < min_pane_w) s_PaneW0 = min_pane_w;
        if (s_PaneW1 < min_pane_w) s_PaneW1 = min_pane_w;
        if (s_PaneW0 + s_PaneW1 > total_panes_w - min_pane_w) {
            float excess = (s_PaneW0 + s_PaneW1) - (total_panes_w - min_pane_w);
            s_PaneW1 -= excess * 0.5f;
            s_PaneW0 -= excess * 0.5f;
        }
        s_PaneW2 = total_panes_w - s_PaneW0 - s_PaneW1;
        if (s_PaneW2 < min_pane_w) s_PaneW2 = min_pane_w;

        // Paint unified rounded Left Card (Explorer + Build List merged with a center split)
        ImVec2 left_card_min(outer_margin, panes_top_y);
        float left_card_w = s_PaneW0 + inner_split_w + s_PaneW1;
        ImVec2 left_card_max(left_card_min.x + left_card_w, left_card_min.y + panes_height);
        float split_mid_x = std::floor(left_card_min.x + s_PaneW0 + inner_split_w * 0.5f) + 0.5f;
        float card_round  = ui::Theme::CardRounding;

        root_dl->AddRectFilled(
            left_card_min,
            ImVec2(split_mid_x, left_card_max.y),
            ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::BgExplorer),
            card_round,
            ImDrawFlags_RoundCornersLeft
        );
        root_dl->AddRectFilled(
            ImVec2(split_mid_x, left_card_min.y),
            left_card_max,
            ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::BgBuildList),
            card_round,
            ImDrawFlags_RoundCornersRight
        );
        root_dl->AddRectFilled(
            left_card_min,
            ImVec2(left_card_max.x, left_card_min.y + ui::Theme::HeaderHeight),
            IM_COL32(255, 255, 255, 7),
            card_round,
            ImDrawFlags_RoundCornersTop
        );

        ImGui::SetCursorPos(ImVec2(outer_margin, panes_top_y));

        // Left Card - Left Half: Project Explorer (embedded)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("Pane0Container", ImVec2(s_PaneW0, panes_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ui::RenderFileTreePanel(g_Project, true);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);

        // Middle split inside the merged card (1px vertical divider + draggable hit area)
        bool changed1 = RenderSplitter("##split1", &s_PaneW0, &s_PaneW1, min_pane_w, min_pane_w, inner_split_w, panes_height);

        ImGui::SameLine(0.0f, 0.0f);

        // Left Card - Right Half: Build List / Targets (embedded)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("Pane1Container", ImVec2(s_PaneW1, panes_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ui::RenderBuildListPanel(g_Project, g_CompilerManager, g_BuildRunner, g_BuildStartError, g_OutputLines, true);
        ImGui::EndChild();

        // Crisp 1px unified rounded outer border around the merged Left Card
        root_dl->AddRect(
            left_card_min,
            left_card_max,
            ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::Border),
            card_round,
            ImDrawFlags_RoundCornersAll,
            1.0f
        );

        ImGui::SameLine(0.0f, 0.0f);

        // Main Splitter between Merged Left Card and Standalone Output Terminal Card
        bool changed2 = RenderMainSplitter("##split_main", &s_PaneW0, &s_PaneW1, &s_PaneW2, min_pane_w, main_splitter_w, panes_height);

        ImGui::SameLine(0.0f, 0.0f);

        // Right Side: Output Terminal Card (standalone rounded card)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("Pane2Container", ImVec2(s_PaneW2, panes_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ui::RenderOutputPanel(
            g_CompilerManager, g_BuildRunner, g_OutputLines, g_BuildStartError,
            &g_RunRunner, &g_RunLines, &g_RunStartError, &g_Project
        );
        ImGui::EndChild();

        // Save pane widths as normalized ratios (0-1) in project.json on drag release (Spec §1.6)
        if (changed1 || changed2) {
            float sum = s_PaneW0 + s_PaneW1 + s_PaneW2;
            if (sum > 0.0f) {
                g_Project.SetPaneWidths({ s_PaneW0 / sum, s_PaneW1 / sum, s_PaneW2 / sum });
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2); // WindowBorderSize, WindowPadding

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);
    // V2-20 Windows Taskbar Progress (ITaskbarList3) & unfocused completion FlashWindowEx
    {
        static ITaskbarList3* s_Taskbar = nullptr;
        static bool s_TaskbarTried = false;
        static bool s_WasBuildingPrev = false;
        if (!s_TaskbarTried) {
            s_TaskbarTried = true;
            ITaskbarList3* tb = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tb))) && tb) {
                if (SUCCEEDED(tb->HrInit())) {
                    s_Taskbar = tb;
                } else {
                    tb->Release();
                }
            }
        }
        bool is_bld = g_BuildRunner.IsRunning();
        if (s_Taskbar) {
            if (is_bld) {
                core::BuildProgress bp = g_BuildRunner.GetProgress();
                if (bp.totalJobs > 0) {
                    s_Taskbar->SetProgressState(hwnd, TBPF_NORMAL);
                    s_Taskbar->SetProgressValue(hwnd, bp.completedJobs, bp.totalJobs);
                } else {
                    s_Taskbar->SetProgressState(hwnd, TBPF_INDETERMINATE);
                }
            } else if (s_WasBuildingPrev) {
                s_Taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
            }
        }
        if (s_WasBuildingPrev && !is_bld) {
            if (GetForegroundWindow() != hwnd) {
                FLASHWINFO fw{};
                fw.cbSize = sizeof(fw);
                fw.hwnd = hwnd;
                fw.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
                fw.uCount = 3;
                FlashWindowEx(&fw);
            }
        }
        s_WasBuildingPrev = is_bld;
    }
    // 1px Col::Border outline around non-maximized window for visible contrast on dark desktop backgrounds (Spec §1.5 & §6.3 U11)
    if (!IsZoomed(hwnd)) {
        ImGui::GetForegroundDrawList()->AddRect(
            ImVec2(0.5f, 0.5f),
            ImVec2(viewport->Size.x - 0.5f, viewport->Size.y - 0.5f),
            ImGui::ColorConvertFloat4ToU32(ui::Theme::Col::Border),
            0.0f,
            0,
            1.0f
        );
    }
#endif

    // Rendering OpenGL frame
    ImGui::Render();
    int display_w = 0, display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(ui::Theme::Col::WindowBg.x, ui::Theme::Col::WindowBg.y, ui::Theme::Col::WindowBg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
    s_InFrame = false;
}

int main(int argc, char* argv[]) {
    // Headless CLI mode (§0.4): geniebuild --build [--target T] [--config C] [--toolchain N] [--trust] <path>
    bool cli_build = false;
    bool cli_trust = false;
    std::string cli_target;
    std::string cli_config;
    std::string cli_toolchain;
    std::filesystem::path cli_project_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--build") {
            cli_build = true;
        } else if (arg == "--trust") {
            cli_trust = true;
        } else if (arg == "--target" && i + 1 < argc) {
            cli_target = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            cli_config = argv[++i];
        } else if (arg == "--toolchain" && i + 1 < argc) {
            cli_toolchain = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            std::error_code ec;
            std::filesystem::path p = std::filesystem::absolute(std::filesystem::u8path(arg), ec);
            if (!ec) cli_project_path = p;
        }
    }

    if (cli_build) {
        if (cli_project_path.empty()) {
            cli_project_path = std::filesystem::current_path();
        }
        core::Project proj;
        if (!proj.Load(cli_project_path)) {
            std::cerr << "Failed to load project at: " << cli_project_path.generic_u8string() << "\n";
            return 1;
        }
        while (proj.IsScanning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            proj.Update();
        }
        proj.Update();
        if (!cli_config.empty()) {
            proj.SetConfiguration(cli_config);
        }
        if (!cli_toolchain.empty()) {
            proj.SetToolchain(cli_toolchain);
        }
        if (cli_trust) {
            core::TrustStore::SetTrusted(cli_project_path, true);
        }
        core::CompilerManager cm;
        core::BuildRunner runner;
        std::string err;
        if (!runner.StartBuild(proj, cm, err, cli_target)) {
            std::cerr << err << "\n";
            return 1;
        }
        while (runner.IsRunning()) {
            for (const auto& l : runner.DrainOutput()) {
                std::cout << l.text << "\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        for (const auto& l : runner.DrainOutput()) {
            std::cout << l.text << "\n";
        }
        return runner.GetLastResult().success ? 0 : 1;
    }

    // Determine target project path safely: start empty unless explicitly opened via CLI arg
    std::filesystem::path project_path = cli_project_path;

    // Set GLFW error callback before initialization
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW.\n";
        return 1;
    }

    // Initialize Native File Dialog Extended
    NFD_Init();

    // Configure OpenGL context profile (OpenGL 3.3 Core Profile)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Borderless window (GLFW_DECORATED false)
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "GenieBuild v1", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window.\n";
        NFD_Quit();
        glfwTerminate();
        return 1;
    }

    g_Window = window;

    // Register drag-and-drop, live resize refresh, and DPI scaling callbacks (Spec §6.1 P1)
    glfwSetDropCallback(window, GlfwDropCallback);
    glfwSetWindowRefreshCallback(window, GlfwWindowRefreshCallback);
    glfwSetWindowContentScaleCallback(window, GlfwContentScaleCallback);

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);

    // Enable standard window resizing, minimizing, and Aero Snap behavior
    LONG_PTR win_style = GetWindowLongPtr(hwnd, GWL_STYLE);
    win_style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtr(hwnd, GWL_STYLE, win_style);

    // Enable DWM drop shadow for borderless window
    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

    // Request Windows 11 rounded window corners (Spec §6.3 U11)
    DWORD corner_preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference, sizeof(corner_preference));

    // Subclass window proc for edge resize hit-testing and client area calculation
    g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassWndProc);

    // Notify Windows that frame styles have changed
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
#endif

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable VSync

    // Query window content scale for DPI scaling
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    float dpi_scale = (xscale > 0.0f) ? xscale : 1.0f;
    g_DpiScale = dpi_scale;

    // Initialize Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Disable imgui.ini writing to CWD (Spec §4)
    io.IniFilename = nullptr;

    // Single apply_theme() function configuring fonts, icon font merging, style, and colors
    ui::apply_theme(dpi_scale);

    // Initialize ImGui backends for GLFW and OpenGL 3
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Load project state (restores project.json or pre-fills on first open)
    g_Project.Load(project_path);

    static auto s_LastActiveTime = std::chrono::steady_clock::now();

    // Main event and render loop
    while (!glfwWindowShouldClose(window)) {
        // Event processing (§2.6): Poll continuously during input and 500ms after, or during
        // active build/scan/indexing to keep 60 FPS locked to vsync. When completely idle,
        // sleep up to 250ms via glfwWaitEventsTimeout() to drop CPU usage well below 1%.
        bool has_background_work = g_BuildRunner.IsRunning() || g_RunRunner.IsRunning() || g_Project.IsScanning() || ui::IsFileTreeIndexing();
        bool has_user_input = (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                               io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                               io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2] ||
                               io.InputQueueCharacters.Size > 0);
        if (has_background_work || has_user_input) {
            s_LastActiveTime = std::chrono::steady_clock::now();
        }

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s_LastActiveTime
        ).count();

        if (has_background_work || elapsed_ms < 500) {
            glfwPollEvents();
        } else {
            glfwWaitEventsTimeout(0.250);
        }

        // Intercept close request if build is currently active (Spec §4)
        if (glfwWindowShouldClose(window) && g_BuildRunner.IsRunning()) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            ui::RequestExitConfirmation();
        }

        // Render frame
        RenderFrame(window);
    }

    // Cleanup resources
    g_RunRunner.Stop();
    if (g_BuildRunner.IsRunning()) {
        std::cout << "Waiting for active compiler process to finish before shutdown...\n";
    }
    ui::ShutdownFileTree();
    g_Project.SaveImmediate();
    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
