#include "theme.h"
#include "icons.h"
#include <filesystem>
#include <iostream>
#include <cmath>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ui {

AppFonts g_Fonts;

namespace Theme {

namespace Sp {
    float S1 = 4.0f;
    float S2 = 8.0f;
    float S3 = 12.0f;
    float S4 = 16.0f;
}

namespace Btn {
    float WinCtrlW    = 46.0f;
    float WinCtrlH    = 34.0f;
    float DefaultH    = 26.0f;
    float SmallH      = 22.0f;
    float IconW       = 24.0f;
    float IconSmallW  = 22.0f;
    float CfgW        = 68.0f;
    float BuildW      = 106.0f;
    float SplitArrowW = 24.0f;
    float ClearW      = 72.0f;
    float RefreshW    = 86.0f;
    float CloseW      = 90.0f;
    float DelW        = 22.0f;
}

namespace Dim {
    float ComboW          = 156.0f;
    float ModalW          = 720.0f;
    float ModalH          = 560.0f;
    float FormLabelW      = 180.0f;
    float FormInputW      = 155.0f;
    float FormSmallInputW = 120.0f;
    float CmdBoxH         = 70.0f;
    float FooterH         = 28.0f;
    float MinPaneW        = 160.0f;
    float GlyphS1         = 3.0f;
    float GlyphS2         = 5.0f;
}

float TitleBarHeight = 34.0f;
float ToolBarHeight  = 42.0f;
float HeaderHeight   = 34.0f;
float RowHeight      = 24.0f;
float SplitterWidth  = 10.0f;
float CardRounding   = 10.0f;
float OutputDirWidth = 160.0f;
float DpiScale       = 1.0f;

namespace Col {
    ImVec4 Text         = ImVec4(0.957f, 0.957f, 0.961f, 1.00f);
    ImVec4 TextDim      = ImVec4(0.557f, 0.557f, 0.596f, 1.00f);
    ImVec4 Border       = ImVec4(0.153f, 0.153f, 0.169f, 1.00f);
    ImVec4 Hover        = ImVec4(1.000f, 1.000f, 1.000f, 0.065f);
    ImVec4 Selected     = ImVec4(1.000f, 1.000f, 1.000f, 0.120f);
    ImVec4 Accent       = ImVec4(0.240f, 0.560f, 0.980f, 1.00f);
    ImVec4 Success      = ImVec4(0.086f, 0.745f, 0.525f, 1.00f);
    ImVec4 Warning      = ImVec4(0.950f, 0.650f, 0.250f, 1.00f);
    ImVec4 Error        = ImVec4(0.950f, 0.350f, 0.350f, 1.00f);
    ImVec4 Danger       = ImVec4(0.850f, 0.200f, 0.200f, 0.90f);

    // Monochrome Black & White neutral surfaces (blue reserved for ticks/checks, green for Build)
    ImVec4 BgChrome     = ImVec4(0.051f, 0.051f, 0.059f, 1.00f);
    ImVec4 BgExplorer   = ImVec4(0.067f, 0.067f, 0.075f, 1.00f);
    ImVec4 BgBuildList  = ImVec4(0.078f, 0.078f, 0.088f, 1.00f);
    ImVec4 BgOutput     = ImVec4(0.027f, 0.027f, 0.032f, 1.00f);
    ImVec4 WindowBg     = ImVec4(0.035f, 0.035f, 0.040f, 1.00f);
    ImVec4 FrameBg      = ImVec4(0.094f, 0.094f, 0.106f, 1.00f);
    ImVec4 Button       = ImVec4(0.106f, 0.106f, 0.122f, 1.00f);
    ImVec4 Inactive     = ImVec4(0.420f, 0.420f, 0.460f, 0.65f);
    ImVec4 Override     = ImVec4(0.950f, 0.720f, 0.250f, 1.00f);
    ImVec4 TargetPalette[8] = {
        ImVec4(0.32f, 0.64f, 0.96f, 0.95f), // Blue
        ImVec4(0.24f, 0.76f, 0.56f, 0.95f), // Teal/Green
        ImVec4(0.92f, 0.64f, 0.28f, 0.95f), // Amber
        ImVec4(0.74f, 0.52f, 0.92f, 0.95f), // Purple
        ImVec4(0.92f, 0.46f, 0.56f, 0.95f), // Rose
        ImVec4(0.46f, 0.80f, 0.88f, 0.95f), // Cyan
        ImVec4(0.78f, 0.78f, 0.36f, 0.95f), // Olive/Gold
        ImVec4(0.64f, 0.74f, 0.92f, 0.95f)  // Periwinkle
    };
}

ImVec4 GetTargetColor(const std::string& target_name) {
    uint32_t h = 2166136261u;
    for (unsigned char c : target_name) {
        h ^= c;
        h *= 16777619u;
    }
    return Col::TargetPalette[h % 8];
}

// Helper: strips ImGui "##id" suffix for rendering visible label text
static std::string VisibleLabel(const char* label) {
    if (!label) return "";
    std::string s(label);
    size_t pos = s.find("##");
    if (pos != std::string::npos) {
        s.resize(pos);
    }
    return s;
}

// Custom rounded button renderer that guarantees text & corners never clip or spill outside bounds
static bool RenderCustomButton(
    const char* id,
    const char* visible_text,
    const ImVec2& size_in,
    const ImVec4& bg_col,
    const ImVec4& bg_hover,
    const ImVec4& bg_active,
    const ImVec4& border_col,
    const ImVec4& text_col,
    ImDrawFlags round_flags,
    bool bold_font = false,
    bool disabled = false,
    bool draw_chevron_down = false
) {
    ImGuiStyle& style = ImGui::GetStyle();
    if (bold_font && g_Fonts.bold) ImGui::PushFont(g_Fonts.bold);

    ImVec2 text_sz = visible_text && visible_text[0] ? ImGui::CalcTextSize(visible_text) : ImVec2(0, 0);
    ImVec2 sz = size_in;
    if (sz.x <= 0.0f) sz.x = text_sz.x + style.FramePadding.x * 2.0f;
    if (sz.y <= 0.0f) sz.y = Btn::DefaultH;

    ImVec2 p_min = ImGui::GetCursorScreenPos();
    ImVec2 p_max(p_min.x + sz.x, p_min.y + sz.y);

    bool clicked = false;
    bool hovered = false;
    bool active = false;

    if (disabled) {
        ImGui::BeginDisabled(true);
        ImGui::InvisibleButton(id, sz);
        ImGui::EndDisabled();
    } else {
        clicked = ImGui::InvisibleButton(id, sz);
        hovered = ImGui::IsItemHovered();
        active  = ImGui::IsItemActive();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 fill = disabled ? bg_col : (active ? bg_active : (hovered ? bg_hover : bg_col));
    float rounding = style.FrameRounding;

    dl->AddRectFilled(p_min, p_max, ImGui::ColorConvertFloat4ToU32(fill), rounding, round_flags);
    dl->AddRect(p_min, p_max, ImGui::ColorConvertFloat4ToU32(border_col), rounding, round_flags, 1.0f);

    if (draw_chevron_down) {
        // Crisp pixel-centered vector V-chevron (never clips outside box)
        ImVec2 c((p_min.x + p_max.x) * 0.5f, (p_min.y + p_max.y) * 0.5f + 0.5f * DpiScale);
        float hw = 3.5f * DpiScale;
        float hh = 2.0f * DpiScale;
        ImU32 col_u32 = ImGui::ColorConvertFloat4ToU32(text_col);
        dl->AddLine(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x, c.y + hh), col_u32, 1.4f * DpiScale);
        dl->AddLine(ImVec2(c.x, c.y + hh), ImVec2(c.x + hw, c.y - hh), col_u32, 1.4f * DpiScale);
    } else if (visible_text && visible_text[0]) {
        dl->PushClipRect(p_min, p_max, true);
        ImVec2 t_pos(
            p_min.x + (sz.x - text_sz.x) * 0.5f,
            p_min.y + (sz.y - text_sz.y) * 0.5f
        );
        dl->AddText(t_pos, ImGui::ColorConvertFloat4ToU32(text_col), visible_text);
        dl->PopClipRect();
    }

    if (bold_font && g_Fonts.bold) ImGui::PopFont();
    return clicked;
}

void Tooltip(const char* text) {
    if (text && text[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * DpiScale, 6.0f * DpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * DpiScale);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.075f, 0.075f, 0.088f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0.240f, 0.240f, 0.275f, 1.00f));
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(420.0f * DpiScale);
            ImGui::TextColored(Col::Text, "%s", text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

bool IconButton(const char* id, const char* icon, const char* tooltip, bool active, const ImVec2& size) {
    ImVec2 btn_sz = size;
    if (btn_sz.x <= 0.0f) btn_sz.x = Btn::IconW;
    if (btn_sz.y <= 0.0f) btn_sz.y = Btn::SmallH;

    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.06f, 0.25f, 0.19f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.09f, 0.34f, 0.26f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,        Col::Success);
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::Success);
    }

    bool clicked = ImGui::Button(icon, btn_sz);

    if (active) {
        ImGui::PopStyleColor(4);
    }

    ImGui::PopStyleVar(2);
    ImGui::PopID();

    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }

    return clicked;
}

bool Button(const char* label, const char* tooltip, bool active, const ImVec2& size) {
    ImGuiStyle& style = ImGui::GetStyle();
    bool has_custom_size = (size.x > 0.0f || size.y > 0.0f);
    if (has_custom_size) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(size.x > 0.0f ? 4.0f * DpiScale : style.FramePadding.x, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    }

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.21f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.24f, 0.28f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.45f, 0.45f, 0.50f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::Text);
    }

    bool clicked = ImGui::Button(label, size);

    if (active) {
        ImGui::PopStyleColor(4);
    }
    if (has_custom_size) {
        ImGui::PopStyleVar(2);
    }

    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }

    return clicked;
}

bool SplitMainButton(const char* label, const char* tooltip, bool active, const ImVec2& size) {
    std::string vis = VisibleLabel(label);
    ImVec4 bg     = active ? ImVec4(0.180f, 0.180f, 0.210f, 1.00f) : Col::Button;
    ImVec4 hover  = active ? ImVec4(0.240f, 0.240f, 0.280f, 1.00f) : ImVec4(0.145f, 0.145f, 0.168f, 1.00f);
    ImVec4 act    = ImVec4(0.190f, 0.190f, 0.220f, 1.00f);
    ImVec4 border = active ? ImVec4(0.450f, 0.450f, 0.500f, 1.00f) : Col::Border;
    ImVec4 txt    = Col::Text;

    bool clicked = RenderCustomButton(label, vis.c_str(), size, bg, hover, act, border, txt, ImDrawFlags_RoundCornersLeft, false, false, false);
    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }
    return clicked;
}

bool SplitArrowButton(const char* id, const char* tooltip, const ImVec2& size, bool accent_success, bool disabled) {
    ImVec2 sz = size;
    if (sz.x <= 0.0f) sz.x = Btn::SplitArrowW;
    if (sz.y <= 0.0f) sz.y = Btn::DefaultH;

    ImVec4 bg     = accent_success ? ImVec4(0.055f, 0.440f, 0.310f, 1.00f) : Col::Button;
    ImVec4 hover  = accent_success ? ImVec4(0.075f, 0.560f, 0.395f, 1.00f) : ImVec4(0.145f, 0.145f, 0.168f, 1.00f);
    ImVec4 act    = accent_success ? ImVec4(0.045f, 0.360f, 0.250f, 1.00f) : ImVec4(0.190f, 0.190f, 0.220f, 1.00f);
    ImVec4 border = accent_success ? ImVec4(0.095f, 0.650f, 0.460f, 1.00f) : Col::Border;
    ImVec4 txt    = disabled ? Col::TextDim : (accent_success ? ImVec4(0.95f, 1.0f, 0.98f, 1.0f) : Col::Text);

    bool clicked = RenderCustomButton(id, nullptr, sz, bg, hover, act, border, txt, ImDrawFlags_RoundCornersRight, false, disabled, true);
    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }
    return clicked;
}

bool ComboButton(const char* id, const char* preview_text, const char* tooltip, const ImVec2& size, bool active_popup) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 sz = size;
    if (sz.x <= 0.0f) sz.x = Dim::ComboW;
    if (sz.y <= 0.0f) sz.y = Btn::DefaultH;

    ImVec2 p_min = ImGui::GetCursorScreenPos();
    ImVec2 p_max(p_min.x + sz.x, p_min.y + sz.y);

    bool clicked = ImGui::InvisibleButton(id, sz);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive() || active_popup;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 fill = active ? ImVec4(0.155f, 0.155f, 0.180f, 1.00f)
                : hovered ? ImVec4(0.125f, 0.125f, 0.145f, 1.00f)
                          : Col::FrameBg;
    ImVec4 border = active ? ImVec4(0.380f, 0.380f, 0.430f, 1.00f) : Col::Border;

    dl->AddRectFilled(p_min, p_max, ImGui::ColorConvertFloat4ToU32(fill), style.FrameRounding);
    dl->AddRect(p_min, p_max, ImGui::ColorConvertFloat4ToU32(border), style.FrameRounding, 0, 1.0f);

    // Pixel-centered vector chevron down on right
    float arrow_w = Btn::SplitArrowW;
    float arrow_x0 = p_max.x - arrow_w;
    ImVec2 c(arrow_x0 + arrow_w * 0.5f, (p_min.y + p_max.y) * 0.5f + 0.5f * DpiScale);
    float hw = 3.5f * DpiScale;
    float hh = 2.0f * DpiScale;
    ImU32 col_txt = ImGui::ColorConvertFloat4ToU32(Col::Text);
    dl->AddLine(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x, c.y + hh), col_txt, 1.4f * DpiScale);
    dl->AddLine(ImVec2(c.x, c.y + hh), ImVec2(c.x + hw, c.y - hh), col_txt, 1.4f * DpiScale);

    // Clipped preview text with automatic ellipsis strictly inside the box left of chevron
    float pad_x = 10.0f * DpiScale;
    float text_min_x = p_min.x + pad_x;
    float text_max_x = arrow_x0 - 4.0f * DpiScale;
    if (preview_text && preview_text[0] && text_max_x > text_min_x) {
        std::string txt(preview_text);
        float max_w = text_max_x - text_min_x;
        if (ImGui::CalcTextSize(txt.c_str()).x > max_w) {
            const float ell_w = ImGui::CalcTextSize("...").x;
            while (!txt.empty() && ImGui::CalcTextSize(txt.c_str()).x + ell_w > max_w) {
                txt.pop_back();
            }
            txt += "...";
        }
        float text_h = ImGui::GetTextLineHeight();
        float text_y = p_min.y + (sz.y - text_h) * 0.5f;
        dl->PushClipRect(ImVec2(text_min_x, p_min.y), ImVec2(text_max_x, p_max.y), true);
        dl->AddText(ImVec2(text_min_x, text_y), col_txt, txt.c_str());
        dl->PopClipRect();
    }

    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }
    return clicked;
}

bool SegmentButton(const char* label, bool active, const ImVec2& size, ImDrawFlags round_flags) {
    std::string vis = VisibleLabel(label);
    ImVec4 bg     = active ? ImVec4(0.200f, 0.200f, 0.235f, 1.00f) : Col::FrameBg;
    ImVec4 hover  = active ? ImVec4(0.240f, 0.240f, 0.280f, 1.00f) : ImVec4(0.135f, 0.135f, 0.155f, 1.00f);
    ImVec4 act    = ImVec4(0.260f, 0.260f, 0.300f, 1.00f);
    ImVec4 border = active ? ImVec4(0.420f, 0.420f, 0.470f, 1.00f) : Col::Border;
    ImVec4 txt    = active ? Col::Text : Col::TextDim;

    return RenderCustomButton(label, vis.c_str(), size, bg, hover, act, border, txt, round_flags, false, false, false);
}

bool SuccessButton(const char* label, const char* tooltip, const ImVec2& size, ImDrawFlags round_flags) {
    std::string vis = VisibleLabel(label);
    ImVec4 bg     = ImVec4(0.060f, 0.480f, 0.340f, 1.0f);
    ImVec4 hover  = ImVec4(0.080f, 0.600f, 0.420f, 1.0f);
    ImVec4 act    = ImVec4(0.050f, 0.390f, 0.275f, 1.0f);
    ImVec4 border = ImVec4(0.095f, 0.680f, 0.480f, 1.0f);
    ImVec4 txt    = ImVec4(0.96f, 1.00f, 0.98f, 1.0f);

    bool clicked = RenderCustomButton(label, vis.c_str(), size, bg, hover, act, border, txt, round_flags, true, false, false);
    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }
    return clicked;
}

bool DangerIconButton(const char* id, const char* icon, const char* tooltip, const ImVec2& size) {
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.22f, 0.28f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::Danger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.12f, 0.12f, 1.00f));

    ImVec2 btn_sz = size;
    if (btn_sz.x <= 0.0f) btn_sz.x = Btn::DelW;
    if (btn_sz.y <= 0.0f) btn_sz.y = Btn::DefaultH;

    bool clicked = ImGui::Button(icon, btn_sz);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    if (tooltip && tooltip[0] != '\0') {
        Tooltip(tooltip);
    }
    ImGui::PopID();
    return clicked;
}

void DisabledButton(const char* label, const ImVec2& size, const ImVec4& text_col, const ImVec4& bg_col, const ImVec4& border_col, ImDrawFlags round_flags) {
    std::string vis = VisibleLabel(label);
    RenderCustomButton(label, vis.c_str(), size, bg_col, bg_col, bg_col, border_col, text_col, round_flags, true, true, false);
}

static bool s_PaneEmbedded = false;

bool PaneBegin(const char* id, const ImVec4& bg_col, const ImVec4& border_col, const ImVec2& size, const ImVec2& padding, bool embedded) {
    s_PaneEmbedded = embedded;
    if (embedded) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        return ImGui::BeginChild(id, size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg_col);
        ImGui::PushStyleColor(ImGuiCol_Border,  border_col);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CardRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        return ImGui::BeginChild(id, size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    }
}

void PaneEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static ImVec2 s_HeaderStartPos;
static float s_HeaderWidth = 0.0f;
static bool s_HeaderEmbedded = false;

void BeginPaneHeader(const char* title, float total_right_w, bool embedded) {
    s_HeaderStartPos = ImGui::GetCursorScreenPos();
    s_HeaderWidth = ImGui::GetContentRegionAvail().x;
    s_HeaderEmbedded = embedded;
    const float h = HeaderHeight;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Subtle elevated header tint for standalone cards (merged card paints its full header band in main.cpp)
    if (!embedded) {
        dl->AddRectFilled(
            s_HeaderStartPos,
            ImVec2(s_HeaderStartPos.x + s_HeaderWidth, s_HeaderStartPos.y + h),
            IM_COL32(255, 255, 255, 7),
            CardRounding,
            ImDrawFlags_RoundCornersTop
        );
    }

    // Anchor right widgets strictly inside the pane right border
    float right_x = s_HeaderStartPos.x + s_HeaderWidth - Sp::S3 - total_right_w;
    if (right_x < s_HeaderStartPos.x + Sp::S2) {
        right_x = s_HeaderStartPos.x + Sp::S2;
    }

    // Clip left title strictly before the right widget block so title and controls NEVER overlap or spill
    float title_clip_max_x = (total_right_w > 0.0f) ? (right_x - Sp::S2) : (s_HeaderStartPos.x + s_HeaderWidth - Sp::S3);
    if (title_clip_max_x < s_HeaderStartPos.x + Sp::S3) {
        title_clip_max_x = s_HeaderStartPos.x + Sp::S3;
    }

    ImGui::PushFont(g_Fonts.title);
    const float text_h = ImGui::GetTextLineHeight();
    const float title_y = s_HeaderStartPos.y + (h - text_h) * 0.5f;
    dl->PushClipRect(
        ImVec2(s_HeaderStartPos.x + Sp::S3, s_HeaderStartPos.y),
        ImVec2(title_clip_max_x, s_HeaderStartPos.y + h),
        true
    );
    dl->AddText(ImVec2(s_HeaderStartPos.x + Sp::S3, title_y), ImGui::ColorConvertFloat4ToU32(Col::Text), title);
    dl->PopClipRect();
    ImGui::PopFont();

    // Position cursor for right widgets
    if (total_right_w > 0.0f) {
        ImGui::SetCursorScreenPos(ImVec2(right_x, s_HeaderStartPos.y + (h - Btn::SmallH) * 0.5f));
    }
}

void EndPaneHeader() {
    const float h = HeaderHeight;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float line_y = std::floor(s_HeaderStartPos.y + h) + 0.5f;
    dl->AddLine(
        ImVec2(s_HeaderStartPos.x, line_y),
        ImVec2(s_HeaderStartPos.x + s_HeaderWidth, line_y),
        ImGui::ColorConvertFloat4ToU32(Col::Border),
        1.0f
    );

    // Position cursor below header with standard margin
    ImGui::SetCursorScreenPos(ImVec2(s_HeaderStartPos.x + Sp::S3, s_HeaderStartPos.y + h + Sp::S2));
}

void PaneHeader(const char* title, const char* right_text, const ImVec4& right_color, bool embedded) {
    float right_w = 0.0f;
    if (right_text && right_text[0] != '\0') {
        ImGui::PushFont(g_Fonts.secondary);
        right_w = ImGui::CalcTextSize(right_text).x;
        ImGui::PopFont();
    }

    BeginPaneHeader(title, right_w, embedded);
    if (right_w > 0.0f) {
        ImGui::PushFont(g_Fonts.title);
        float title_w = ImGui::CalcTextSize(title).x;
        ImGui::PopFont();
        float min_right_x = s_HeaderStartPos.x + Sp::S3 + title_w + Sp::S2;
        float right_x = s_HeaderStartPos.x + s_HeaderWidth - Sp::S3 - right_w;
        if (right_x >= min_right_x) {
            const float text_h = ImGui::GetTextLineHeight();
            const float text_y = s_HeaderStartPos.y + (HeaderHeight - text_h) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(right_x, text_y));
            ImGui::PushFont(g_Fonts.secondary);
            ImGui::TextColored(right_color, "%s", right_text);
            ImGui::PopFont();
        }
    }
    EndPaneHeader();
}

static float s_ModalWidth = 440.0f;

bool BeginModal(const char* popup_id, const char* title, const char* icon, const ImVec4& icon_col, float width, bool* out_close_clicked) {
    if (out_close_clicked) *out_close_clicked = false;
    const float w = width * DpiScale;
    s_ModalWidth = w;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, CardRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Col::BgExplorer);
    ImGui::PushStyleColor(ImGuiCol_Border, Col::Border);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoScrollbar;
    bool open = ImGui::BeginPopupModal(popup_id, nullptr, flags);

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (!open) {
        return false;
    }

    const float hdr_h = HeaderHeight + 4.0f * DpiScale;
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float win_w = ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(
        win_pos,
        ImVec2(win_pos.x + win_w, win_pos.y + hdr_h),
        ImGui::ColorConvertFloat4ToU32(Col::BgChrome),
        CardRounding,
        ImDrawFlags_RoundCornersTop
    );
    dl->AddLine(
        ImVec2(win_pos.x, std::floor(win_pos.y + hdr_h) + 0.5f),
        ImVec2(win_pos.x + win_w, std::floor(win_pos.y + hdr_h) + 0.5f),
        ImGui::ColorConvertFloat4ToU32(Col::Border),
        1.0f
    );

    float tx = Sp::S4;
    if (icon && icon[0] != '\0') {
        ImGui::PushFont(g_Fonts.title);
        ImGui::SetCursorPos(ImVec2(tx, (hdr_h - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(icon_col, "%s", icon);
        ImGui::PopFont();
        ImGui::SameLine(0.0f, Sp::S2);
    } else {
        ImGui::SetCursorPosX(tx);
    }

    ImGui::PushFont(g_Fonts.title);
    ImGui::SetCursorPosY((hdr_h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(Col::Text, "%s", title);
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(win_w - Btn::IconW - Sp::S3, (hdr_h - Btn::SmallH) * 0.5f));
    if (IconButton("##ModalCloseX", ICON_FA_XMARK, "Close", false, ImVec2(Btn::IconW, Btn::SmallH))) {
        if (out_close_clicked) *out_close_clicked = true;
    }

    ImGui::SetCursorPos(ImVec2(Sp::S4, hdr_h + Sp::S3));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(w - Sp::S4);
    return true;
}

void EndModal() {
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorPosX(0.0f);
    ImGui::Dummy(ImVec2(s_ModalWidth, Sp::S4));
    ImGui::EndPopup();
}

} // namespace Theme

static std::filesystem::path FindFontAsset(const std::string& rel_filename) {
    std::error_code ec;

    std::filesystem::path p1 = std::filesystem::path("assets/fonts") / rel_filename;
    if (std::filesystem::exists(p1, ec) && !ec) {
        return p1;
    }

#if defined(_WIN32)
    wchar_t exe_buf[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe_buf, MAX_PATH) > 0) {
        std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();

        std::filesystem::path p2 = exe_dir / "assets/fonts" / rel_filename;
        if (std::filesystem::exists(p2, ec) && !ec) {
            return p2;
        }

        std::filesystem::path p3 = exe_dir.parent_path() / "assets/fonts" / rel_filename;
        if (std::filesystem::exists(p3, ec) && !ec) {
            return p3;
        }

        std::filesystem::path p4 = exe_dir.parent_path().parent_path() / "assets/fonts" / rel_filename;
        if (std::filesystem::exists(p4, ec) && !ec) {
            return p4;
        }
    }
#endif

    return "";
}

void apply_theme(float dpi_scale) {
    if (dpi_scale <= 0.0f) {
        dpi_scale = 1.0f;
    }

    // Update tokens with new dpi scale
    Theme::DpiScale       = dpi_scale;
    Theme::Sp::S1         = 4.0f * dpi_scale;
    Theme::Sp::S2         = 8.0f * dpi_scale;
    Theme::Sp::S3         = 12.0f * dpi_scale;
    Theme::Sp::S4         = 16.0f * dpi_scale;

    Theme::Btn::WinCtrlW    = 46.0f * dpi_scale;
    Theme::Btn::WinCtrlH    = 34.0f * dpi_scale;
    Theme::Btn::DefaultH    = 26.0f * dpi_scale;
    Theme::Btn::SmallH      = 22.0f * dpi_scale;
    Theme::Btn::IconW       = 24.0f * dpi_scale;
    Theme::Btn::IconSmallW  = 22.0f * dpi_scale;
    Theme::Btn::CfgW        = 68.0f * dpi_scale;
    Theme::Btn::BuildW      = 106.0f * dpi_scale;
    Theme::Btn::SplitArrowW = 24.0f * dpi_scale;
    Theme::Btn::ClearW      = 72.0f * dpi_scale;
    Theme::Btn::RefreshW    = 86.0f * dpi_scale;
    Theme::Btn::CloseW      = 90.0f * dpi_scale;
    Theme::Btn::DelW        = 22.0f * dpi_scale;

    Theme::Dim::ComboW          = 156.0f * dpi_scale;
    Theme::Dim::ModalW          = 720.0f * dpi_scale;
    Theme::Dim::ModalH          = 560.0f * dpi_scale;
    Theme::Dim::FormLabelW      = 180.0f * dpi_scale;
    Theme::Dim::FormInputW      = 155.0f * dpi_scale;
    Theme::Dim::FormSmallInputW = 120.0f * dpi_scale;
    Theme::Dim::CmdBoxH         = 70.0f * dpi_scale;
    Theme::Dim::FooterH         = 30.0f * dpi_scale;
    Theme::Dim::MinPaneW        = 160.0f * dpi_scale;
    Theme::Dim::GlyphS1         = 3.0f * dpi_scale;
    Theme::Dim::GlyphS2         = 5.0f * dpi_scale;

    Theme::TitleBarHeight = 34.0f * dpi_scale;
    Theme::ToolBarHeight  = 42.0f * dpi_scale;
    Theme::HeaderHeight   = 34.0f * dpi_scale;
    Theme::RowHeight      = 24.0f * dpi_scale;
    Theme::SplitterWidth  = 10.0f * dpi_scale;
    Theme::CardRounding   = 10.0f * dpi_scale;
    Theme::OutputDirWidth = 160.0f * dpi_scale;

    ImGuiIO& io = ImGui::GetIO();

    // -------------------------------------------------------------------------
    // Typography:
    // §1.4: Title (15 semibold, letter-cased), Label (14), Secondary (13), Mono (13.5)
    // -------------------------------------------------------------------------
    const std::filesystem::path local_inter_regular = FindFontAsset("Inter-Regular.ttf");
    const std::filesystem::path local_inter_bold    = FindFontAsset("Inter-SemiBold.ttf");
    const std::filesystem::path local_jetbrains     = FindFontAsset("JetBrainsMono-Regular.ttf");
    const std::filesystem::path local_fa_solid      = FindFontAsset("fa-solid-900.ttf");

    const std::filesystem::path win_segoe_regular   = "C:/Windows/Fonts/segoeui.ttf";
    const std::filesystem::path win_segoe_bold      = "C:/Windows/Fonts/segoeuib.ttf";
    const std::filesystem::path win_cascadia        = "C:/Windows/Fonts/CascadiaMono.ttf";
    const std::filesystem::path win_consolas        = "C:/Windows/Fonts/consola.ttf";

    static const ImWchar icons_ranges[] = { 0xf000, 0xf8ff, 0 };
    ImFontConfig icon_config;
    icon_config.MergeMode = true;
    icon_config.PixelSnapH = true;
    icon_config.GlyphMinAdvanceX = 14.0f * dpi_scale;

    // 1. Label font (14px * dpi)
    if (!local_inter_regular.empty()) {
        g_Fonts.label = io.Fonts->AddFontFromFileTTF(local_inter_regular.generic_u8string().c_str(), 14.0f * dpi_scale);
    } else if (std::filesystem::exists(win_segoe_regular)) {
        g_Fonts.label = io.Fonts->AddFontFromFileTTF(win_segoe_regular.generic_u8string().c_str(), 14.5f * dpi_scale);
    } else {
        g_Fonts.label = io.Fonts->AddFontDefault();
    }
    if (!local_fa_solid.empty()) {
        io.Fonts->AddFontFromFileTTF(local_fa_solid.generic_u8string().c_str(), 13.0f * dpi_scale, &icon_config, icons_ranges);
    }

    // 2. Title font (15px * dpi, semibold)
    if (!local_inter_bold.empty()) {
        g_Fonts.title = io.Fonts->AddFontFromFileTTF(local_inter_bold.generic_u8string().c_str(), 15.0f * dpi_scale);
    } else if (std::filesystem::exists(win_segoe_bold)) {
        g_Fonts.title = io.Fonts->AddFontFromFileTTF(win_segoe_bold.generic_u8string().c_str(), 15.5f * dpi_scale);
    } else {
        g_Fonts.title = g_Fonts.label;
    }
    if (!local_fa_solid.empty()) {
        io.Fonts->AddFontFromFileTTF(local_fa_solid.generic_u8string().c_str(), 13.0f * dpi_scale, &icon_config, icons_ranges);
    }

    // 3. Secondary font (13px * dpi)
    if (!local_inter_regular.empty()) {
        g_Fonts.secondary = io.Fonts->AddFontFromFileTTF(local_inter_regular.generic_u8string().c_str(), 13.0f * dpi_scale);
    } else if (std::filesystem::exists(win_segoe_regular)) {
        g_Fonts.secondary = io.Fonts->AddFontFromFileTTF(win_segoe_regular.generic_u8string().c_str(), 13.5f * dpi_scale);
    } else {
        g_Fonts.secondary = g_Fonts.label;
    }
    if (!local_fa_solid.empty()) {
        ImFontConfig sec_icon_config = icon_config;
        sec_icon_config.GlyphMinAdvanceX = 12.0f * dpi_scale;
        io.Fonts->AddFontFromFileTTF(local_fa_solid.generic_u8string().c_str(), 12.0f * dpi_scale, &sec_icon_config, icons_ranges);
    }

    // 4. Monospace font (13.5px * dpi)
    if (!local_jetbrains.empty()) {
        g_Fonts.mono = io.Fonts->AddFontFromFileTTF(local_jetbrains.generic_u8string().c_str(), 13.5f * dpi_scale);
    } else if (std::filesystem::exists(win_cascadia)) {
        g_Fonts.mono = io.Fonts->AddFontFromFileTTF(win_cascadia.generic_u8string().c_str(), 14.0f * dpi_scale);
    } else if (std::filesystem::exists(win_consolas)) {
        g_Fonts.mono = io.Fonts->AddFontFromFileTTF(win_consolas.generic_u8string().c_str(), 14.0f * dpi_scale);
    } else {
        g_Fonts.mono = g_Fonts.label;
    }

    // Compatibility aliases
    g_Fonts.regular = g_Fonts.label;
    g_Fonts.bold = g_Fonts.title;

    // Set default UI font
    io.FontDefault = g_Fonts.label;

    // -------------------------------------------------------------------------
    // Geometry & Style Tokens:
    // -------------------------------------------------------------------------
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = Theme::CardRounding;
    style.FrameRounding     = 6.0f * dpi_scale;
    style.PopupRounding     = 8.0f * dpi_scale;
    style.ScrollbarRounding = 6.0f * dpi_scale;
    style.GrabRounding      = 6.0f * dpi_scale;
    style.TabRounding       = 6.0f * dpi_scale;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    style.ScrollbarSize     = 6.0f * dpi_scale;

    // FramePadding.y matched so InputText/Combo height == Btn::DefaultH (26px * dpi)
    style.WindowPadding     = ImVec2(Theme::Sp::S3, Theme::Sp::S3);
    style.FramePadding      = ImVec2(Theme::Sp::S2 + 2.0f * dpi_scale, 6.0f * dpi_scale);
    style.ItemSpacing       = ImVec2(Theme::Sp::S2, Theme::Sp::S2);
    style.ItemInnerSpacing  = ImVec2(6.0f * dpi_scale, 6.0f * dpi_scale);

    // -------------------------------------------------------------------------
    // Color Palette Tokens:
    // -------------------------------------------------------------------------
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg]             = Theme::Col::WindowBg;
    colors[ImGuiCol_ChildBg]              = Theme::Col::BgExplorer;
    colors[ImGuiCol_PopupBg]              = ImVec4(0.075f, 0.075f, 0.086f, 0.98f);
    colors[ImGuiCol_Border]               = Theme::Col::Border;
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);

    colors[ImGuiCol_Text]                 = Theme::Col::Text;
    colors[ImGuiCol_TextDisabled]         = Theme::Col::TextDim;

    colors[ImGuiCol_FrameBg]              = Theme::Col::FrameBg;
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.125f, 0.125f, 0.145f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.155f, 0.155f, 0.180f, 1.00f);

    colors[ImGuiCol_Button]               = Theme::Col::Button;
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.145f, 0.145f, 0.168f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.190f, 0.190f, 0.220f, 1.00f);

    colors[ImGuiCol_Header]               = Theme::Col::Selected;
    colors[ImGuiCol_HeaderHovered]        = Theme::Col::Hover;
    colors[ImGuiCol_HeaderActive]         = ImVec4(1.000f, 1.000f, 1.000f, 0.18f);

    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.190f, 0.190f, 0.215f, 0.75f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.270f, 0.270f, 0.305f, 0.90f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.360f, 0.360f, 0.400f, 1.00f);

    colors[ImGuiCol_Separator]            = Theme::Col::Border;
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.250f, 0.250f, 0.285f, 1.00f);
    colors[ImGuiCol_SeparatorActive]      = Theme::Col::Accent;

    colors[ImGuiCol_CheckMark]            = Theme::Col::Accent;
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.450f, 0.450f, 0.500f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = Theme::Col::Accent;

    // Modals, TitleBar, Tabs & Resize Grips (monochrome consistency with main UI)
    colors[ImGuiCol_TitleBg]              = Theme::Col::BgChrome;
    colors[ImGuiCol_TitleBgActive]        = Theme::Col::BgChrome;
    colors[ImGuiCol_TitleBgCollapsed]     = Theme::Col::BgChrome;
    colors[ImGuiCol_Tab]                  = Theme::Col::FrameBg;
    colors[ImGuiCol_TabHovered]           = ImVec4(0.200f, 0.200f, 0.230f, 1.00f);
    colors[ImGuiCol_TabSelected]          = ImVec4(0.235f, 0.235f, 0.270f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline]  = Theme::Col::Text;
    colors[ImGuiCol_TabDimmed]            = Theme::Col::FrameBg;
    colors[ImGuiCol_TabDimmedSelected]    = ImVec4(0.180f, 0.180f, 0.205f, 1.00f);
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.250f, 0.250f, 0.285f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.450f, 0.450f, 0.500f, 0.75f);
    colors[ImGuiCol_ResizeGripActive]     = Theme::Col::Accent;
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.65f);
}

} // namespace ui
