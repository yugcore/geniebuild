#pragma once

#include <imgui.h>
#include <string>

namespace ui {

// Holds pointers to loaded application fonts
struct AppFonts {
    ImFont* label = nullptr;     // UI default / label font (Inter Regular, 14px * dpi)
    ImFont* title = nullptr;     // Pane title & bold font (Inter SemiBold, 15px * dpi)
    ImFont* secondary = nullptr; // Secondary / text-dim font (Inter Regular, 13px * dpi)
    ImFont* mono = nullptr;      // Terminal monospace font (JetBrains Mono, 13.5px * dpi)

    // Backward-compatibility aliases
    ImFont* regular = nullptr;   // Same as label
    ImFont* bold = nullptr;      // Same as title
};

// Global font handle accessible across UI components
extern AppFonts g_Fonts;

namespace Theme {

// Spacing tokens (scaled by dpi)
namespace Sp {
    extern float S1; // 4px * dpi
    extern float S2; // 8px * dpi
    extern float S3; // 12px * dpi
    extern float S4; // 16px * dpi
}

// Button and control dimension tokens (scaled by dpi)
namespace Btn {
    extern float WinCtrlW;    // 46px * dpi
    extern float WinCtrlH;    // 32px * dpi (matches HeaderHeight)
    extern float DefaultH;    // 24px * dpi
    extern float SmallH;      // 20px * dpi
    extern float IconW;       // 24px * dpi
    extern float IconSmallW;  // 20px * dpi
    extern float CfgW;        // 50px * dpi
    extern float BuildW;      // 90px * dpi
    extern float SplitArrowW; // 20px * dpi
    extern float ClearW;      // 64px * dpi
    extern float RefreshW;    // 78px * dpi
    extern float CloseW;      // 90px * dpi
    extern float DelW;        // 22px * dpi
}

// Layout, form, and modal dimension tokens (scaled by dpi)
namespace Dim {
    extern float ComboW;          // 150px * dpi
    extern float ModalW;          // 720px * dpi
    extern float ModalH;          // 560px * dpi
    extern float FormLabelW;      // 180px * dpi
    extern float FormInputW;      // 140px * dpi
    extern float FormSmallInputW; // 120px * dpi
    extern float CmdBoxH;         // 70px * dpi
    extern float FooterH;         // 28px * dpi
    extern float MinPaneW;        // 160px * dpi
    extern float GlyphS1;         // 3px * dpi
    extern float GlyphS2;         // 5px * dpi
}

// Fixed dimensions (scaled by dpi)
extern float TitleBarHeight;  // 34px * dpi (top window title bar)
extern float ToolBarHeight;   // 42px * dpi (action & workflow toolbar)
extern float HeaderHeight;    // 34px * dpi (pane headers)
extern float RowHeight;       // 24px * dpi
extern float SplitterWidth;   // 10px * dpi
extern float CardRounding;    // 10px * dpi (merged left card & terminal card rounding)
extern float OutputDirWidth;  // 160px * dpi
extern float DpiScale;

// Color tokens
namespace Col {
    extern ImVec4 Text;
    extern ImVec4 TextDim;
    extern ImVec4 Border;
    extern ImVec4 Hover;
    extern ImVec4 Selected;
    extern ImVec4 Accent;
    extern ImVec4 Success;
    extern ImVec4 Warning;
    extern ImVec4 Error;
    extern ImVec4 Danger;

    // Surface palette tokens
    extern ImVec4 BgChrome;    // Top title bar & toolbar surface
    extern ImVec4 BgExplorer;  // Merged card left surface
    extern ImVec4 BgBuildList; // Merged card right surface
    extern ImVec4 BgOutput;    // Terminal card surface
    extern ImVec4 WindowBg;    // Root canvas backdrop
    extern ImVec4 FrameBg;     // Input / combo fill
    extern ImVec4 Button;      // Button fill
    extern ImVec4 Inactive;    // Condition-false / dimmed row text
    extern ImVec4 Override;    // Per-folder / per-file override flag marker (V2-3)
    extern ImVec4 TargetPalette[8]; // Stable muted per-target colors
}

// Returns a stable color from TargetPalette for a target name
ImVec4 GetTargetColor(const std::string& target_name);

// UI helper methods
void Tooltip(const char* text);
bool IconButton(const char* id, const char* icon, const char* tooltip = nullptr, bool active = false, const ImVec2& size = ImVec2(0, 0));
bool Button(const char* label, const char* tooltip = nullptr, bool active = false, const ImVec2& size = ImVec2(0, 0));
bool SplitMainButton(const char* label, const char* tooltip = nullptr, bool active = false, const ImVec2& size = ImVec2(0, 0));
bool SplitArrowButton(const char* id, const char* tooltip = nullptr, const ImVec2& size = ImVec2(0, 0), bool accent_success = false, bool disabled = false);
bool ComboButton(const char* id, const char* preview_text, const char* tooltip = nullptr, const ImVec2& size = ImVec2(0, 0), bool active_popup = false);
bool SegmentButton(const char* label, bool active, const ImVec2& size = ImVec2(0, 0), ImDrawFlags round_flags = ImDrawFlags_RoundCornersAll);
bool SuccessButton(const char* label, const char* tooltip = nullptr, const ImVec2& size = ImVec2(0, 0), ImDrawFlags round_flags = ImDrawFlags_RoundCornersAll);
bool DangerIconButton(const char* id, const char* icon, const char* tooltip = nullptr, const ImVec2& size = ImVec2(0, 0));
void DisabledButton(const char* label, const ImVec2& size = ImVec2(0, 0), const ImVec4& text_col = Col::TextDim, const ImVec4& bg_col = Col::Button, const ImVec4& border_col = Col::Border, ImDrawFlags round_flags = ImDrawFlags_RoundCornersAll);

// Pane container helpers (embedded=true omits outer border/rounding for panes inside the merged left card)
bool PaneBegin(const char* id, const ImVec4& bg_col, const ImVec4& border_col = Col::Border, const ImVec2& size = ImVec2(0, 0), const ImVec2& padding = ImVec2(0, 0), bool embedded = false);
void PaneEnd();

// Pane header helpers: fixed 34px * dpi height with 1px border line below
void PaneHeader(const char* title, const char* right_text = nullptr, const ImVec4& right_color = Col::TextDim, bool embedded = false);
void BeginPaneHeader(const char* title, float total_right_w = 0.0f, bool embedded = false);
void EndPaneHeader();

// Monochrome rounded modal helpers matching the main UI card chrome
bool BeginModal(const char* popup_id, const char* title, const char* icon = nullptr, const ImVec4& icon_col = Col::Text, float width = 440.0f, bool* out_close_clicked = nullptr);
void EndModal();

} // namespace Theme

// Sets up typography, icon font merging, geometry, and cohesive dark theme in a single call
void apply_theme(float dpi_scale = 1.0f);

} // namespace ui
