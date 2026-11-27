#pragma once

#include "theme.h"
#include "icons.h"
#include "tool_registry.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <cmath>

namespace ui {

enum class TreeRowType {
    Folder,
    SourceFile,
    HeaderFile
};

struct TreeRow {
    std::string id_str;             // Unique identifier key for selection & state
    std::string name;               // Display filename or directory segment name
    std::string rel_path;           // Normalized path relative to project root
    std::string extra_label;        // Optional count or metadata, e.g. " (24)"
    std::string type_badge;         // V2-7 badge: RC, ASM, CUDA, ISPC, or custom tool
    ImU32 type_badge_color = 0;     // Non-zero badge text color
    TreeRowType type = TreeRowType::Folder;
    int depth = 0;                  // Indentation depth level (0 = top-level)
    uint64_t ancestor_has_next = 0; // Bitmask for tree guide lines
    bool is_last_child = false;     // True if last sibling among its parent's children
    bool is_open = false;           // Expanded/collapsed state for folders
    bool in_build = false;          // True if in project build list (for Explorer)
    bool is_missing = false;        // True if missing on disk, or folder has missing files
    bool is_c = false;              // True for .c source, false for .cpp/.cc/.cxx
    int file_count = 0;             // Total files inside folder (for Build List)
    int missing_count = 0;          // Number of missing files (for Build List)
    bool show_checkbox = false;     // True if row displays a checkbox (for Build List)
    int check_state = 1;            // 0 = unchecked (excluded from build), 1 = checked (compiled), 2 = indeterminate
    bool is_disabled = false;       // True if file/folder is excluded from compile
    bool has_override = false;      // True if per-folder / per-file flag override applies (V2-3)
    std::string override_tooltip;   // Summary of matching overrides
    std::vector<ImVec4> target_dots; // Colored dots for owning targets in multi-target mode
    void* user_data = nullptr;      // Arbitrary user pointer
};

inline void PopulateTreeRowTypeBadge(TreeRow& row, const std::vector<core::CustomToolSpec>& custom_tools) {
    if (row.type != TreeRowType::SourceFile || row.rel_path.empty()) return;
    core::SourceFileKind kind = core::ToolRegistry::ClassifySourceFile(row.rel_path, custom_tools);
    switch (kind) {
        case core::SourceFileKind::Rc:
            row.type_badge = "RC";
            row.type_badge_color = IM_COL32(230, 175, 75, 235);
            break;
        case core::SourceFileKind::AsmPre:
        case core::SourceFileKind::AsmRaw:
        case core::SourceFileKind::AsmNasm:
            row.type_badge = "ASM";
            row.type_badge_color = IM_COL32(180, 140, 235, 235);
            break;
        case core::SourceFileKind::Cuda:
            row.type_badge = "CUDA";
            row.type_badge_color = IM_COL32(118, 210, 80, 235);
            break;
        case core::SourceFileKind::Ispc:
            row.type_badge = "ISPC";
            row.type_badge_color = IM_COL32(90, 200, 235, 235);
            break;
        case core::SourceFileKind::Custom: {
            std::string ext = row.rel_path;
            size_t dot = ext.find_last_of('.');
            ext = (dot != std::string::npos) ? ext.substr(dot + 1) : "TOOL";
            for (char& ch : ext) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            row.type_badge = ext.empty() ? "TOOL" : ext;
            row.type_badge_color = IM_COL32(235, 130, 170, 235);
            break;
        }
        default:
            break;
    }
}

struct TreeRowResult {
    bool selected_changed = false;  // User single-clicked to select
    bool double_clicked = false;    // User double-clicked row
    bool toggle_expanded = false;   // User clicked chevron to expand/collapse
    bool checkbox_clicked = false;  // User clicked the compile inclusion checkbox
    bool remove_clicked = false;    // User clicked the hover X button
    bool context_menu = false;      // User right-clicked row
    bool tooltip_hovered = false;   // Row (not remove button) hovered for tooltip delay
};

// Renders a single flat row in a virtualized tree with:
// - Connected vertical and elbow guide lines (using depth and ancestor bitmask)
// - Chevrons and dynamic open/close folder icons or file icons
// - Full-width hover and persistent selection with accent bar
// - Text color by role (neutral text, accent markers)
// - Optional remove-on-hover button for Build List
inline TreeRowResult RenderTreeRow(
    const TreeRow& row,
    const std::string& selected_id,
    bool show_remove_button = false,
    bool show_build_marker = false
) {
    TreeRowResult res;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float row_height = Theme::RowHeight;
    const float dpi = Theme::DpiScale;
    const float indent = 16.0f * dpi;
    const float base_x = ImGui::GetCursorScreenPos().x + Theme::Sp::S1;

    ImVec2 row_pos = ImGui::GetCursorScreenPos();
    float row_w = ImGui::GetContentRegionAvail().x;
    ImVec2 row_min = row_pos;
    ImVec2 row_max = ImVec2(row_pos.x + row_w, row_pos.y + row_height);

    bool is_selected = (!selected_id.empty() && selected_id == row.id_str);

    ImGui::PushID(row.id_str.c_str());

    // 1. Selectable row base for full-width hover tint and selection handling
    bool clicked = ImGui::Selectable(
        "##row_sel",
        is_selected,
        ImGuiSelectableFlags_AllowDoubleClick,
        ImVec2(0, row_height)
    );

    // Save exact post-selectable cursor coordinate to restore after rendering row widgets.
    // This completely eliminates hover vertical jitter/stutter when hover buttons appear!
    ImVec2 next_cursor_pos = ImGui::GetCursorPos();

    bool is_hovered = ImGui::IsItemHovered();
    res.tooltip_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
    if (clicked) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            res.double_clicked = true;
        } else {
            res.selected_changed = true;
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        res.context_menu = true;
        res.selected_changed = true;
    }

    // 2. Persistent Selected Row Accent Bar
    if (is_selected) {
        dl->AddRectFilled(
            ImVec2(row_min.x, row_min.y),
            ImVec2(row_min.x + 2.5f * dpi, row_max.y),
            ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent)
        );
    }

    // 3. Tree Guide Lines (connected vertical lines & elbow branches)
    const ImU32 border_col = ImGui::ColorConvertFloat4ToU32(Theme::Col::Border);

    // Vertical pass-through lines for ancestors that have more siblings
    for (int d = 0; d < row.depth - 1; ++d) {
        if ((row.ancestor_has_next & (1ULL << d)) != 0) {
            float line_x = std::floor(base_x + d * indent + indent * 0.5f) + 0.5f;
            dl->AddLine(
                ImVec2(line_x, row_min.y),
                ImVec2(line_x, row_max.y),
                border_col,
                1.0f
            );
        }
    }

    // Current row's vertical segment and horizontal elbow
    if (row.depth > 0) {
        float curr_x = std::floor(base_x + (row.depth - 1) * indent + indent * 0.5f) + 0.5f;
        float mid_y = std::floor(row_min.y + row_height * 0.5f) + 0.5f;
        float vert_end_y = row.is_last_child ? mid_y : row_max.y;

        // Vertical line
        dl->AddLine(
            ImVec2(curr_x, row_min.y),
            ImVec2(curr_x, vert_end_y),
            border_col,
            1.0f
        );

        // Horizontal elbow to glyph column (stops 3px before the chevron / icon per Spec U4)
        float elbow_end_x = std::floor(base_x + row.depth * indent - 3.0f * dpi) + 0.5f;
        if (elbow_end_x <= curr_x) {
            elbow_end_x = curr_x + 1.0f;
        }
        dl->AddLine(
            ImVec2(curr_x, mid_y),
            ImVec2(elbow_end_x, mid_y),
            border_col,
            1.0f
        );
    }

    // 4. Chevron and Icon columns
    float cursor_x = base_x + row.depth * indent;
    const float chev_w = 14.0f * dpi;
    const float icon_w = 18.0f * dpi;
    const float mid_text_y = row_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f;

    // Chevron (only for folders)
    if (row.type == TreeRowType::Folder) {
        const char* chev_icon = row.is_open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
        ImVec4 chev_col = Theme::Col::TextDim;
        chev_col.w *= 0.70f; // 70% alpha per Spec U4
        dl->AddText(
            ImVec2(cursor_x, mid_text_y),
            ImGui::ColorConvertFloat4ToU32(chev_col),
            chev_icon
        );

        // Hit-test chevron click specifically to toggle expansion
        if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImVec2 mouse_pos = ImGui::GetMousePos();
            if (mouse_pos.x >= cursor_x - 4.0f * dpi && mouse_pos.x <= cursor_x + chev_w + 4.0f * dpi) {
                res.toggle_expanded = true;
            }
        }
    }
    cursor_x += chev_w;

    // 4.5 Checkbox column (for Build List file & folder compile-inclusion toggling)
    if (row.show_checkbox) {
        const float chk_sz = 13.0f * dpi;
        const float chk_y = row_min.y + (row_height - chk_sz) * 0.5f;
        const ImVec2 chk_min(cursor_x, chk_y);
        const ImVec2 chk_max(cursor_x + chk_sz, chk_y + chk_sz);

        // Click detection on the checkbox specifically
        if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImVec2 mpos = ImGui::GetMousePos();
            if (mpos.x >= chk_min.x - 3.0f * dpi && mpos.x <= chk_max.x + 3.0f * dpi &&
                mpos.y >= chk_min.y - 3.0f * dpi && mpos.y <= chk_max.y + 3.0f * dpi) {
                res.checkbox_clicked = true;
            }
        }

        // Render checkbox box & state mark
        if (row.check_state == 1) {
            // Checked [✓]: Accent fill with white checkmark
            dl->AddRectFilled(chk_min, chk_max, ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent), 2.5f * dpi);
            dl->AddRect(chk_min, chk_max, ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent), 2.5f * dpi, 0, 1.0f);

            ImVec2 icon_pos(chk_min.x + 1.5f * dpi, row_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f - 1.0f * dpi);
            ImGui::PushFont(g_Fonts.secondary);
            dl->AddText(icon_pos, IM_COL32(255, 255, 255, 255), ICON_FA_CHECK);
            ImGui::PopFont();
        } else if (row.check_state == 2) {
            // Indeterminate [-]: Darker accent fill with horizontal bar (for folders with mixed files)
            dl->AddRectFilled(chk_min, chk_max, IM_COL32(30, 58, 110, 255), 2.5f * dpi);
            dl->AddRect(chk_min, chk_max, ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent), 2.5f * dpi, 0, 1.0f);
            float mid_chk_y = std::floor(chk_min.y + chk_sz * 0.5f) + 0.5f;
            dl->AddLine(ImVec2(chk_min.x + 3.0f * dpi, mid_chk_y), ImVec2(chk_max.x - 3.0f * dpi, mid_chk_y), IM_COL32(255, 255, 255, 255), 2.0f);
        } else {
            // Unchecked [ ]: Neutral dark monochrome box with subtle border
            dl->AddRectFilled(chk_min, chk_max, IM_COL32(22, 22, 26, 255), 2.5f * dpi);
            dl->AddRect(chk_min, chk_max, IM_COL32(72, 72, 80, 255), 2.5f * dpi, 0, 1.0f);
        }

        cursor_x += chk_sz + 5.0f * dpi;
    }

    // Folder or File Icon (monochrome neutral palette)
    const char* main_icon = "";
    ImU32 icon_color = ImGui::ColorConvertFloat4ToU32(Theme::Col::Text);

    if (row.type == TreeRowType::Folder) {
        main_icon = row.is_open ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER;
        icon_color = IM_COL32(215, 215, 222, 235);
    } else if (row.is_missing) {
        main_icon = ICON_FA_CIRCLE_EXCLAMATION;
        icon_color = ImGui::ColorConvertFloat4ToU32(Theme::Col::Error);
    } else if (row.type == TreeRowType::SourceFile) {
        main_icon = ICON_FA_FILE_CODE;
        icon_color = row.is_disabled ? ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim) : ImGui::ColorConvertFloat4ToU32(Theme::Col::Text);
    } else { // HeaderFile
        main_icon = ICON_FA_CODE;
        icon_color = ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim);
    }

    dl->AddText(
        ImVec2(cursor_x, mid_text_y),
        icon_color,
        main_icon
    );
    cursor_x += icon_w;

    // 5. Marker column (target color dots in multi-target mode, or accent check for in_build, or red dot for folder with missing files)
    if (show_build_marker && (row.type == TreeRowType::SourceFile || row.type == TreeRowType::HeaderFile)) {
        if (row.type == TreeRowType::SourceFile && !row.target_dots.empty()) {
            float dot_r = 3.0f * dpi;
            float dot_y = row_min.y + row_height * 0.5f;
            size_t n_dots = std::min<size_t>(row.target_dots.size(), 3);
            for (size_t di = 0; di < n_dots; ++di) {
                dl->AddCircleFilled(
                    ImVec2(cursor_x + dot_r + static_cast<float>(di) * (dot_r * 2.4f), dot_y),
                    dot_r,
                    ImGui::ColorConvertFloat4ToU32(row.target_dots[di])
                );
            }
            cursor_x += std::max(16.0f * dpi, static_cast<float>(n_dots) * (dot_r * 2.4f) + 4.0f * dpi);
        } else {
            if (row.type == TreeRowType::SourceFile && row.in_build) {
                dl->AddText(
                    ImVec2(cursor_x, mid_text_y),
                    ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent),
                    ICON_FA_CHECK
                );
            }
            cursor_x += 16.0f * dpi;
        }
    } else if (row.type == TreeRowType::Folder && row.is_missing) {
        // Red dot on folder containing missing files
        float dot_r = 3.0f * dpi;
        float dot_y = row_min.y + row_height * 0.5f;
        dl->AddCircleFilled(
            ImVec2(cursor_x + dot_r + 1.0f * dpi, dot_y),
            dot_r,
            ImGui::ColorConvertFloat4ToU32(Theme::Col::Error)
        );
        cursor_x += 12.0f * dpi;
    }

    // 6. Name and Extra Label (clipped strictly inside row bounds before any hover remove button)
    float text_limit_x = (show_remove_button && is_hovered) ? (row_max.x - 28.0f * dpi) : (row_max.x - Theme::Sp::S2);
    if (text_limit_x > cursor_x) {
        dl->PushClipRect(ImVec2(cursor_x, row_min.y), ImVec2(text_limit_x, row_max.y), true);

        ImU32 text_col;
        if (row.is_disabled || row.type == TreeRowType::HeaderFile) {
            text_col = ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim);
        } else {
            text_col = ImGui::ColorConvertFloat4ToU32(Theme::Col::Text);
        }

        dl->AddText(
            ImVec2(cursor_x, mid_text_y),
            text_col,
            row.name.c_str()
        );

        float name_w = ImGui::CalcTextSize(row.name.c_str()).x;

        if (row.is_disabled && row.type == TreeRowType::SourceFile) {
            ImGui::PushFont(g_Fonts.secondary);
            dl->AddText(
                ImVec2(cursor_x + name_w + 6.0f * dpi, mid_text_y),
                IM_COL32(235, 95, 95, 220),
                "[excluded]"
            );
            ImGui::PopFont();
            name_w += ImGui::CalcTextSize(" [excluded]").x;
        }

        if (!row.type_badge.empty()) {
            ImGui::PushFont(g_Fonts.secondary);
            ImU32 badge_col = row.type_badge_color != 0 ? row.type_badge_color : ImGui::ColorConvertFloat4ToU32(Theme::Col::Accent);
            std::string btxt = "[" + row.type_badge + "]";
            dl->AddText(
                ImVec2(cursor_x + name_w + 6.0f * dpi, mid_text_y),
                badge_col,
                btxt.c_str()
            );
            name_w += ImGui::CalcTextSize(btxt.c_str()).x + 6.0f * dpi;
            ImGui::PopFont();
        }

        if (row.has_override) {
            ImGui::PushFont(g_Fonts.secondary);
            dl->AddText(
                ImVec2(cursor_x + name_w + 6.0f * dpi, mid_text_y),
                ImGui::ColorConvertFloat4ToU32(Theme::Col::Override),
                ICON_FA_FLAG
            );
            ImGui::PopFont();
            name_w += ImGui::CalcTextSize(" " ICON_FA_FLAG).x + 2.0f * dpi;
        }

        if (!row.extra_label.empty()) {
            ImGui::PushFont(g_Fonts.secondary);
            dl->AddText(
                ImVec2(cursor_x + name_w + 4.0f * dpi, mid_text_y),
                ImGui::ColorConvertFloat4ToU32(Theme::Col::TextDim),
                row.extra_label.c_str()
            );
            ImGui::PopFont();
        }

        dl->PopClipRect();
    }

    // 7. Remove Button on Hover (for Build List folder and file rows)
    if (show_remove_button && is_hovered) {
        const float btn_w = 20.0f * dpi;
        const float btn_h = 18.0f * dpi;
        const float btn_x = row_max.x - btn_w - Theme::Sp::S2;
        const float btn_y = row_min.y + (row_height - btn_h) * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(btn_x, btn_y));
        ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Col::Button);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Col::Danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Col::Error);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * dpi);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

        if (ImGui::Button(ICON_FA_XMARK "##del_btn", ImVec2(btn_w, btn_h))) {
            res.remove_clicked = true;
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            res.tooltip_hovered = false;
            if (row.type == TreeRowType::Folder) {
                std::string tip = "Remove folder '" + row.name + "' and its " + std::to_string(row.file_count) + " files from build list";
                Theme::Tooltip(tip.c_str());
            } else {
                std::string tip = "Remove '" + row.name + "' from build list\n" + row.rel_path;
                Theme::Tooltip(tip.c_str());
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

    // Seamless cursor restoration: ensures subsequent rows are NEVER pushed or jittered up/down
    ImGui::SetCursorPos(next_cursor_pos);

    ImGui::PopID();
    return res;
}

} // namespace ui
