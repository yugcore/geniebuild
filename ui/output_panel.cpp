#include "output_panel.h"
#include "theme.h"
#include "icons.h"
#include <imgui.h>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace ui {

static std::string FormatDuration(double seconds) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << seconds;
    return ss.str();
}

void RenderOutputPanel(
    const core::CompilerManager& compiler_manager,
    const core::BuildRunner& build_runner,
    std::vector<core::OutLine>& output_lines,
    std::string& build_start_error
) {
    static bool s_AutoScroll = true;
    static bool s_WasRunning = false;
    static size_t s_LastLineCount = 0;

    const bool has_compilers = compiler_manager.HasCompilers();
    const bool is_running = build_runner.IsRunning();
    const core::BuildStatus status = build_runner.GetStatus();

    // Auto-scroll re-enabled automatically at start of new build
    if (is_running && !s_WasRunning) {
        s_AutoScroll = true;
    }
    s_WasRunning = is_running;

    // Detect newly arrived lines
    bool new_lines_arrived = (output_lines.size() > s_LastLineCount);
    s_LastLineCount = output_lines.size();

    // -------------------------------------------------------------------------
    // Tier 3: Output Terminal Pane
    // -------------------------------------------------------------------------
    if (Theme::PaneBegin("OutputCard", Theme::Col::BgOutput)) {
        // Status badge calculation (Spec §4: clean labels without brackets or jargon, reasons in tooltips)
        const char* status_badge = "Idle";
        ImVec4 status_color = Theme::Col::TextDim;
        std::string badge_tooltip = "Terminal is idle";
        std::string building_badge;

        if (is_running) {
            int elapsed = static_cast<int>(build_runner.GetElapsedSeconds());
            building_badge = "Building " + std::to_string(elapsed) + "s";
            status_badge = building_badge.c_str();
            status_color = Theme::Col::Warning;
            badge_tooltip = "Compiler process is currently running on a background thread";
        } else if (!build_start_error.empty()) {
            status_badge = "Blocked";
            status_color = Theme::Col::Error;
            badge_tooltip = "Build blocked: " + build_start_error;
        } else if (status == core::BuildStatus::Finished) {
            core::BuildResult result = build_runner.GetLastResult();
            if (result.success) {
                status_badge = "Success";
                status_color = Theme::Col::Success;
                badge_tooltip = "Last build completed successfully";
            } else {
                status_badge = "Failed";
                status_color = Theme::Col::Error;
                badge_tooltip = "Last build failed with exit code " + std::to_string(result.exit_code);
            }
        } else if (!has_compilers) {
            status_badge = "No Compiler";
            status_color = Theme::Col::Error;
            badge_tooltip = "No supported C/C++ compiler detected on PATH";
        }

        // Toolbar buttons moved into the header right side to save a full row (Spec §1.3 & §6.3 U10)
        const float btn_w = Theme::Btn::IconW;
        const float btn_h = Theme::Btn::SmallH;
        const float spacing = Theme::Sp::S1;
        const float badge_w = ImGui::CalcTextSize(status_badge).x;
        const float total_right_w = (btn_w * 3) + (spacing * 2) + Theme::Sp::S2 + badge_w;

        Theme::BeginPaneHeader("OUTPUT TERMINAL", total_right_w);

        const bool has_output = !output_lines.empty() || !build_start_error.empty();

        // 1. Clear button: disabled while build is running or no output (Spec §1.3)
        const bool can_clear = has_output && !is_running;
        if (!can_clear) ImGui::BeginDisabled(true);
        if (Theme::IconButton("##OutClear", ICON_FA_TRASH, "Clear terminal output log", false, ImVec2(btn_w, btn_h))) {
            output_lines.clear();
            build_start_error.clear();
            s_LastLineCount = 0;
        }
        if (!can_clear) ImGui::EndDisabled();

        ImGui::SameLine(0.0f, spacing);

        // 2. Copy all button
        if (!has_output) ImGui::BeginDisabled(true);
        if (Theme::IconButton("##OutCopy", ICON_FA_COPY, "Copy entire build output to clipboard", false, ImVec2(btn_w, btn_h))) {
            std::string full_text;
            if (!build_start_error.empty()) {
                full_text += build_start_error + "\n";
            }
            for (const auto& line : output_lines) {
                if (line.kind == core::OutLineKind::Command) {
                    full_text += "> " + line.text + "\n";
                } else {
                    full_text += line.text + "\n";
                }
            }
            ImGui::SetClipboardText(full_text.c_str());
        }
        if (!has_output) ImGui::EndDisabled();

        ImGui::SameLine(0.0f, spacing);

        // 3. Auto-scroll toggle
        std::string auto_tip = std::string("Auto-scroll is currently ") + (s_AutoScroll ? "enabled" : "paused");
        if (Theme::IconButton("##OutAutoScroll", ICON_FA_ARROW_DOWN, auto_tip.c_str(), s_AutoScroll, ImVec2(btn_w, btn_h))) {
            s_AutoScroll = !s_AutoScroll;
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);

        // 4. Status badge (Spec §4: clean label, reason in tooltip)
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(status_color, "%s", status_badge);
        Theme::Tooltip(badge_tooltip.c_str());

        Theme::EndPaneHeader();

        // -------------------------------------------------------------------------
        // Footer: Pinned Final Status Line at Bottom of Pane (Spec §1.3)
        // -------------------------------------------------------------------------
        const float footer_h = Theme::Dim::FooterH;
        const float avail_h = ImGui::GetContentRegionAvail().y;
        const float item_gap_y = ImGui::GetStyle().ItemSpacing.y;
        const float scroll_h = std::max(1.0f, avail_h - footer_h - item_gap_y);

        // Terminal text area with JetBrains Mono typography and exact line spacing
        ImGui::PushFont(g_Fonts.mono);

        const float mono_size = 13.5f * Theme::DpiScale;
        const float target_line_h = mono_size * 1.35f;
        const float line_gap = target_line_h - ImGui::GetTextLineHeight();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, (line_gap > 0.0f) ? line_gap : 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Sp::S3, Theme::Sp::S2));

        if (ImGui::BeginChild("OutputInnerScroll", ImVec2(0, scroll_h), false)) {
            // Auto-scroll wheel pause/resume logic
            if (ImGui::IsWindowHovered()) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel > 0.0f) {
                    s_AutoScroll = false;
                } else if (wheel < 0.0f) {
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
                        s_AutoScroll = true;
                    }
                }
            }

            if (ImGui::GetScrollMaxY() > 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
                s_AutoScroll = true;
            }

            if (!build_start_error.empty()) {
                ImGui::TextColored(Theme::Col::Error, "> %s", build_start_error.c_str());
            } else if (output_lines.empty()) {
                if (!has_compilers) {
                    ImGui::TextColored(Theme::Col::Error, "No supported C/C++ compiler found on PATH.");
                    ImGui::TextUnformatted(" ");
                    ImGui::TextColored(Theme::Col::TextDim, "GenieBuild searched system PATH for:");
                    ImGui::TextColored(Theme::Col::Text, "    - C++ : g++, clang++, cl");
                    ImGui::TextColored(Theme::Col::Text, "    - C   : gcc, clang, cl");
                    ImGui::TextUnformatted(" ");
                    ImGui::TextColored(Theme::Col::TextDim, "Please install a compiler and ensure its bin folder is in PATH.");
                } else {
                    ImGui::Spacing();
                    ImGui::TextColored(Theme::Col::TextDim, "Ready.");
                    ImGui::Spacing();
                    ImGui::TextColored(Theme::Col::TextDim, "Press Ctrl+B to start build.");
                    ImGui::TextColored(Theme::Col::TextDim, "Compiler commands and live diagnostics will stream here.");
                }
            } else {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float line_height = target_line_h;
                const float full_line_w = ImGui::GetContentRegionAvail().x;

                // Virtualized clipper rendering of terminal lines (Spec §2.7)
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(output_lines.size()), line_height);

                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const auto& line = output_lines[i];
                        ImVec2 cur_pos = ImGui::GetCursorScreenPos();

                        if (line.kind == core::OutLineKind::Command) {
                            // Dimmed command line with $ glyph and separator (Spec §1.3)
                            ImGui::TextColored(Theme::Col::TextDim, "$ %s", line.text.c_str());
                            dl->AddLine(
                                ImVec2(cur_pos.x, cur_pos.y + line_height - 1.0f),
                                ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height - 1.0f),
                                ImGui::GetColorU32(Theme::Col::Border)
                            );
                        } else if (line.kind == core::OutLineKind::Progress) {
                            // Progress line e.g. [12/120] src/main.cpp (Spec §5.1.5)
                            ImGui::TextColored(Theme::Col::Accent, "%s", line.text.c_str());
                        } else if (line.kind == core::OutLineKind::Status) {
                            // Status lines inside scroll (colored by line.has_error, no text prefix matching per §1.3)
                            if (!line.has_error) {
                                ImGui::TextColored(Theme::Col::Success, "%s", line.text.c_str());
                            } else {
                                ImGui::TextColored(Theme::Col::Error, "%s", line.text.c_str());
                            }
                        } else if (line.text.empty()) {
                            // Blank line rendered as single space so height is exactly one line tall (Spec §1.3)
                            ImGui::TextUnformatted(" ");
                        } else {
                            // Visual highlight for error / warning (text color unchanged, byte-identical copy)
                            if (line.has_error) {
                                dl->AddRectFilled(
                                    cur_pos,
                                    ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height),
                                    IM_COL32(239, 68, 68, 25)
                                );
                                dl->AddRectFilled(
                                    cur_pos,
                                    ImVec2(cur_pos.x + 2.5f * Theme::DpiScale, cur_pos.y + line_height),
                                    IM_COL32(239, 68, 68, 220)
                                );
                            } else if (line.has_warning) {
                                dl->AddRectFilled(
                                    cur_pos,
                                    ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height),
                                    IM_COL32(245, 158, 11, 22)
                                );
                                dl->AddRectFilled(
                                    cur_pos,
                                    ImVec2(cur_pos.x + 2.5f * Theme::DpiScale, cur_pos.y + line_height),
                                    IM_COL32(245, 158, 11, 220)
                                );
                            }

                            // Text is byte-identical to compiler output
                            ImGui::TextUnformatted(line.text.c_str());
                        }
                    }
                }

                // Auto-scroll when active or when new lines arrive (Spec §1.3 & §4)
                if (s_AutoScroll && (is_running || new_lines_arrived)) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopFont();

        // -------------------------------------------------------------------------
        // Pinned Footer Area: 1px border line + Status line strictly inside card bounds
        // -------------------------------------------------------------------------
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 win_pos = ImGui::GetWindowPos();
        const ImVec2 win_size = ImGui::GetWindowSize();
        const float footer_top_y = win_pos.y + win_size.y - footer_h;
        const float line_y = std::floor(footer_top_y) + 0.5f;

        dl->AddLine(
            ImVec2(win_pos.x, line_y),
            ImVec2(win_pos.x + win_size.x, line_y),
            ImGui::ColorConvertFloat4ToU32(Theme::Col::Border),
            1.0f
        );

        const float text_x = win_pos.x + Theme::Sp::S3;
        const float max_text_x = std::max(text_x + 1.0f, win_pos.x + win_size.x - Theme::Sp::S3);
        dl->PushClipRect(ImVec2(text_x, footer_top_y + 1.0f), ImVec2(max_text_x, win_pos.y + win_size.y - 1.0f), true);

        if (status == core::BuildStatus::Finished) {
            core::BuildResult res = build_runner.GetLastResult();
            ImGui::PushFont(g_Fonts.bold);
            const float text_y = footer_top_y + (footer_h - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
            if (res.success) {
                std::string msg = std::string(ICON_FA_CHECK) + " Build succeeded · exit 0 · " + FormatDuration(res.duration_seconds) + "s";
                ImGui::TextColored(Theme::Col::Success, "%s", msg.c_str());
            } else {
                std::string msg = std::string(ICON_FA_XMARK) + " Build failed · exit " + std::to_string(res.exit_code) + " · " + FormatDuration(res.duration_seconds) + "s";
                ImGui::TextColored(Theme::Col::Error, "%s", msg.c_str());
            }
            ImGui::PopFont();
        } else if (is_running) {
            ImGui::PushFont(g_Fonts.bold);
            const float text_y = footer_top_y + (footer_h - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
            int elapsed = static_cast<int>(build_runner.GetElapsedSeconds());
            std::string msg = std::string(ICON_FA_SPINNER) + " Building... " + std::to_string(elapsed) + "s";
            ImGui::TextColored(Theme::Col::Warning, "%s", msg.c_str());
            ImGui::PopFont();
        } else if (!build_start_error.empty()) {
            ImGui::PushFont(g_Fonts.bold);
            const float text_y = footer_top_y + (footer_h - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
            ImGui::TextColored(Theme::Col::Error, ICON_FA_CIRCLE_EXCLAMATION " Build blocked: check error diagnostics above");
            ImGui::PopFont();
        } else {
            ImGui::PushFont(g_Fonts.secondary);
            const float text_y = footer_top_y + (footer_h - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
            ImGui::TextColored(Theme::Col::TextDim, "Ready.");
            ImGui::PopFont();
        }

        dl->PopClipRect();
    }
    Theme::PaneEnd();
}

} // namespace ui
