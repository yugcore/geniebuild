#include "output_panel.h"
#include "build_log.h"
#include "hints.h"
#include "project.h"
#include "theme.h"
#include "icons.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace ui {

static int s_PendingTabSwitch = -1; // 0 = Build, 1 = Run

void RequestOutputTab(bool run_tab) {
    s_PendingTabSwitch = run_tab ? 1 : 0;
}

static std::string FormatDuration(double seconds) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << seconds;
    return ss.str();
}

static std::string FormatMmSs(double seconds) {
    if (seconds < 0.0) return "--:--";
    int total = static_cast<int>(std::round(seconds));
    int mins = total / 60;
    int secs = total % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
    return std::string(buf);
}

void RenderOutputPanel(
    const core::CompilerManager& compiler_manager,
    const core::BuildRunner& build_runner,
    std::vector<core::OutLine>& output_lines,
    std::string& build_start_error,
    core::RunRunner* run_runner,
    std::vector<core::OutLine>* run_lines,
    std::string* run_start_error,
    const core::Project* project
) {
    static bool s_AutoScroll = true;
    static bool s_RunAutoScroll = true;
    static bool s_WasRunning = false;
    static size_t s_LastLineCount = 0;
    static size_t s_LastRunLineCount = 0;
    static int s_ActiveTab = 0; // 0 = Build, 1 = Run

    // V2-21 filter / search state
    static int s_FilterMode = 0; // 0=All, 1=Errors, 2=Warnings
    static std::string s_TargetFilter;
    static char s_SearchBuf[256] = "";
    static std::string s_LastSubmittedSearch;
    static int s_CurrentSearchIdx = -1;
    static int s_PendingJumpLine = -1;
    static int s_FocusedErrorLine = -1;
    static bool s_FocusSearchBox = false;
    static std::vector<uint32_t> s_CachedFiltered;
    static size_t s_CacheLogLines = static_cast<size_t>(-1);
    static int s_CacheMode = -1;
    static std::string s_CacheTarget;
    static std::string s_LastCopyFeedback;

    const auto build_log = build_runner.GetBuildLog();
    const size_t disk_log_lines = build_log ? build_log->GetTotalLines() : 0;

    const bool has_compilers = compiler_manager.HasCompilers();
    const bool is_running = build_runner.IsRunning();
    const core::BuildStatus status = build_runner.GetStatus();
    const bool show_run_tab = run_runner && (
        run_runner->HasEverStarted() ||
        (run_lines && !run_lines->empty()) ||
        (run_start_error && !run_start_error->empty())
    );
    if (!show_run_tab) {
        s_ActiveTab = 0;
    } else if (s_PendingTabSwitch >= 0) {
        s_ActiveTab = s_PendingTabSwitch;
        s_PendingTabSwitch = -1;
    }

    if (is_running && !s_WasRunning) {
        s_AutoScroll = true;
        s_FilterMode = 0;
        s_TargetFilter.clear();
        s_FocusedErrorLine = -1;
    }
    s_WasRunning = is_running;

    const size_t active_build_lines = (disk_log_lines > 0) ? disk_log_lines : output_lines.size();
    bool new_lines_arrived = (active_build_lines > s_LastLineCount);
    s_LastLineCount = active_build_lines;
    bool new_run_lines = (run_lines && run_lines->size() > s_LastRunLineCount);
    if (run_lines) s_LastRunLineCount = run_lines->size();

    // Shortcuts in Output panel: Ctrl+F search, F8 / Shift+F8 error jump (V2-21)
    ImGuiIO& io = ImGui::GetIO();
    if (!show_run_tab || s_ActiveTab == 0) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
            s_FocusSearchBox = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F8) && build_log && build_log->GetErrorCount() > 0) {
            bool forward = !io.KeyShift;
            size_t base = (s_FocusedErrorLine >= 0) ? static_cast<size_t>(s_FocusedErrorLine) : 0;
            size_t next_err = build_log->FindNextError(base, forward);
            if (next_err != static_cast<size_t>(-1)) {
                s_FocusedErrorLine = static_cast<int>(next_err);
                s_PendingJumpLine = static_cast<int>(next_err);
                s_AutoScroll = false;
            }
        }
    }

    if (Theme::PaneBegin("OutputCard", Theme::Col::BgOutput)) {
        const char* status_badge = "Idle";
        ImVec4 status_color = Theme::Col::TextDim;
        std::string badge_tooltip = "Terminal is idle";
        std::string building_badge;

        if (s_ActiveTab == 1 && run_runner) {
            if (run_runner->IsRunning()) {
                int elapsed = static_cast<int>(run_runner->GetElapsedSeconds());
                building_badge = "Running " + std::to_string(elapsed) + "s";
                status_badge = building_badge.c_str();
                status_color = Theme::Col::Success;
                badge_tooltip = "Target process is running";
            } else if (run_start_error && !run_start_error->empty()) {
                status_badge = "Blocked";
                status_color = Theme::Col::Error;
                badge_tooltip = *run_start_error;
            } else if (run_runner->HasEverStarted()) {
                int code = run_runner->GetLastExitCode();
                building_badge = "Exit " + std::to_string(code);
                status_badge = building_badge.c_str();
                status_color = (code == 0) ? Theme::Col::TextDim : Theme::Col::Error;
                badge_tooltip = "Last target run exited with code " + std::to_string(code);
            }
        } else if (is_running) {
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
            if (result.cancelled) {
                status_badge = "Cancelled";
                status_color = Theme::Col::Warning;
                badge_tooltip = "Build cancelled by user";
            } else if (result.success) {
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

        const float btn_w = Theme::Btn::IconW;
        const float btn_h = Theme::Btn::SmallH;
        const float spacing = Theme::Sp::S1;
        const float badge_w = ImGui::CalcTextSize(status_badge).x;
        const float tab_extra_w = show_run_tab ? (96.0f * Theme::DpiScale) : 0.0f;
        const float total_right_w = tab_extra_w + (btn_w * 3) + (spacing * 2) + Theme::Sp::S2 + badge_w;

        Theme::BeginPaneHeader(show_run_tab ? "OUTPUT" : "OUTPUT TERMINAL", total_right_w);

        if (show_run_tab) {
            if (Theme::SegmentButton("Build", s_ActiveTab == 0, ImVec2(44.0f * Theme::DpiScale, btn_h), ImDrawFlags_RoundCornersLeft)) {
                s_ActiveTab = 0;
            }
            ImGui::SameLine(0.0f, 0.0f);
            if (Theme::SegmentButton("Run", s_ActiveTab == 1, ImVec2(40.0f * Theme::DpiScale, btn_h), ImDrawFlags_RoundCornersRight)) {
                s_ActiveTab = 1;
            }
            ImGui::SameLine(0.0f, Theme::Sp::S2);
        }

        const bool in_run_tab = (s_ActiveTab == 1 && show_run_tab);
        const bool has_output = in_run_tab
            ? ((run_lines && !run_lines->empty()) || (run_start_error && !run_start_error->empty()))
            : (active_build_lines > 0 || !build_start_error.empty());

        const bool can_clear = has_output && (in_run_tab ? !run_runner->IsRunning() : !is_running);
        if (!can_clear) ImGui::BeginDisabled(true);
        if (Theme::IconButton("##OutClear", ICON_FA_TRASH, "Clear terminal output log", false, ImVec2(btn_w, btn_h))) {
            if (in_run_tab) {
                if (run_lines) run_lines->clear();
                if (run_start_error) run_start_error->clear();
                s_LastRunLineCount = 0;
            } else {
                output_lines.clear();
                if (build_log) build_log->Clear();
                build_start_error.clear();
                s_LastLineCount = 0;
                s_CachedFiltered.clear();
                s_CacheLogLines = static_cast<size_t>(-1);
            }
        }
        if (!can_clear) ImGui::EndDisabled();

        ImGui::SameLine(0.0f, spacing);

        if (!has_output) ImGui::BeginDisabled(true);
        if (Theme::IconButton("##OutCopy", ICON_FA_COPY, "Copy visible/summary output to clipboard", false, ImVec2(btn_w, btn_h))) {
            std::string full_text;
            if (in_run_tab) {
                if (run_start_error && !run_start_error->empty()) full_text += *run_start_error + "\n";
                if (run_lines) {
                    for (const auto& line : *run_lines) {
                        full_text += (line.kind == core::OutLineKind::Command ? "> " : "") + line.text + "\n";
                    }
                }
            } else {
                if (!build_start_error.empty()) full_text += build_start_error + "\n";
                size_t copy_cap = std::min<size_t>(active_build_lines, 20000);
                if (disk_log_lines > 0 && build_log) {
                    for (size_t i = 0; i < copy_cap; ++i) {
                        core::LogLineEntry meta;
                        std::string txt;
                        if (build_log->GetLineMetaAndText(i, meta, txt)) {
                            full_text += (meta.kind == static_cast<uint8_t>(core::OutLineKind::Command) ? "> " : "") + txt + "\n";
                        }
                    }
                } else {
                    for (const auto& line : output_lines) {
                        full_text += (line.kind == core::OutLineKind::Command ? "> " : "") + line.text + "\n";
                    }
                }
            }
            ImGui::SetClipboardText(full_text.c_str());
        }
        if (!has_output) ImGui::EndDisabled();

        ImGui::SameLine(0.0f, spacing);

        bool& cur_auto = in_run_tab ? s_RunAutoScroll : s_AutoScroll;
        std::string auto_tip = std::string("Auto-scroll is currently ") + (cur_auto ? "enabled" : "paused");
        if (Theme::IconButton("##OutAutoScroll", ICON_FA_ARROW_DOWN, auto_tip.c_str(), cur_auto, ImVec2(btn_w, btn_h))) {
            cur_auto = !cur_auto;
        }

        ImGui::SameLine(0.0f, Theme::Sp::S2);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(status_color, "%s", status_badge);
        Theme::Tooltip(badge_tooltip.c_str());

        Theme::EndPaneHeader();

        // ---------------------------------------------------------------------
        // V2-20 Progress bar & Active Jobs Strip (shown while building)
        // ---------------------------------------------------------------------
        if (!in_run_tab && is_running) {
            core::BuildProgress prog = build_runner.GetProgress();
            float frac = (prog.totalJobs > 0)
                ? std::clamp(static_cast<float>(prog.completedJobs) / static_cast<float>(prog.totalJobs), 0.0f, 1.0f)
                : 0.0f;
            std::ostringstream ptxt;
            if (prog.totalJobs > 0) {
                ptxt << prog.completedJobs << " / " << prog.totalJobs;
                if (prog.etaSeconds >= 0.0) {
                    ptxt << " · ETA " << FormatMmSs(prog.etaSeconds);
                }
            } else {
                ptxt << "Preparing build graph... " << FormatMmSs(prog.elapsedSeconds);
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::Sp::S2);
            ImGui::ProgressBar(frac, ImVec2(std::max(100.0f, ImGui::GetContentRegionAvail().x - Theme::Sp::S3), 16.0f * Theme::DpiScale), ptxt.str().c_str());

            if (!prog.activeJobs.empty()) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::Sp::S2);
                const size_t max_chips = 4;
                size_t show_n = std::min(prog.activeJobs.size(), max_chips);
                for (size_t ji = 0; ji < show_n; ++ji) {
                    if (ji > 0) ImGui::SameLine(0.0f, Theme::Sp::S1);
                    const auto& aj = prog.activeJobs[ji];
                    std::string short_lbl = std::filesystem::u8path(aj.label).filename().generic_u8string();
                    if (short_lbl.empty()) short_lbl = aj.label;
                    char chip[96];
                    std::snprintf(chip, sizeof(chip), "%s %.1fs", short_lbl.c_str(), aj.elapsedSeconds);
                    ImVec4 chip_col = (aj.elapsedSeconds > 30.0) ? Theme::Col::Warning : Theme::Col::TextDim;
                    ImGui::TextColored(chip_col, "[%s]", chip);
                }
                if (prog.activeJobs.size() > max_chips) {
                    ImGui::SameLine(0.0f, Theme::Sp::S1);
                    ImGui::TextColored(Theme::Col::Accent, "[+%zu]", prog.activeJobs.size() - max_chips);
                }
            }
        }

        // ---------------------------------------------------------------------
        // V2-21 Filter Chips, Target Filter, Search (Ctrl+F), F8 Error Jump & Logs
        // ---------------------------------------------------------------------
        if (!in_run_tab && build_log && disk_log_lines > 0) {
            const size_t err_cnt = build_log->GetErrorCount();
            const size_t warn_cnt = build_log->GetWarningCount();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::Sp::S2);

            std::string all_lbl = "All (" + std::to_string(disk_log_lines) + ")";
            std::string err_lbl = "Errors " + std::to_string(err_cnt);
            std::string warn_lbl = "Warnings " + std::to_string(warn_cnt);

            if (Theme::Button(err_lbl.c_str(), "Show only error lines (F8/Shift+F8 navigates)", s_FilterMode == 1, ImVec2(0.0f, btn_h))) {
                s_FilterMode = (s_FilterMode == 1) ? 0 : 1;
            }
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (Theme::Button(warn_lbl.c_str(), "Show only warning lines", s_FilterMode == 2, ImVec2(0.0f, btn_h))) {
                s_FilterMode = (s_FilterMode == 2) ? 0 : 2;
            }
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (Theme::Button(all_lbl.c_str(), "Show all log lines", s_FilterMode == 0, ImVec2(0.0f, btn_h))) {
                s_FilterMode = 0;
            }

            auto tnames = build_log->GetTargetNames();
            if (tnames.size() > 1) {
                ImGui::SameLine(0.0f, Theme::Sp::S1);
                ImGui::SetNextItemWidth(96.0f * Theme::DpiScale);
                if (ImGui::BeginCombo("##LogTgtFilter", s_TargetFilter.empty() ? "All targets" : s_TargetFilter.c_str())) {
                    if (ImGui::Selectable("All targets", s_TargetFilter.empty())) s_TargetFilter.clear();
                    for (const auto& tn : tnames) {
                        if (ImGui::Selectable(tn.c_str(), s_TargetFilter == tn)) s_TargetFilter = tn;
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (s_FocusSearchBox) {
                ImGui::SetKeyboardFocusHere();
                s_FocusSearchBox = false;
            }
            ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * 0.35f, 80.0f * Theme::DpiScale, 150.0f * Theme::DpiScale));
            if (ImGui::InputTextWithHint("##LogSearch", "Find (Ctrl+F)...", s_SearchBuf, sizeof(s_SearchBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                s_LastSubmittedSearch = s_SearchBuf;
                build_log->StartSearch(s_LastSubmittedSearch, false);
                s_CurrentSearchIdx = -1;
            }
            if (!s_LastSubmittedSearch.empty()) {
                auto matches = build_log->GetSearchMatches();
                ImGui::SameLine(0.0f, Theme::Sp::S1);
                if (Theme::Button(("Next (" + std::to_string(matches.size()) + ")").c_str(), "Jump to next search match", false, ImVec2(0.0f, btn_h)) && !matches.empty()) {
                    s_CurrentSearchIdx = (s_CurrentSearchIdx + 1) % static_cast<int>(matches.size());
                    s_FilterMode = 0;
                    s_TargetFilter.clear();
                    s_PendingJumpLine = static_cast<int>(matches[static_cast<size_t>(s_CurrentSearchIdx)]);
                    s_AutoScroll = false;
                }
            }

            if (!build_log->GetCurrentLogFile().empty()) {
                ImGui::SameLine(0.0f, Theme::Sp::S1);
                if (Theme::Button("Log", "Open full log in system editor", false, ImVec2(0.0f, btn_h))) {
                    core::HintsManager::OpenPathInShell(build_log->GetCurrentLogFile());
                }
                ImGui::SameLine(0.0f, Theme::Sp::S1);
                if (Theme::Button("Logs dir", "Open .geniebuild/logs folder", false, ImVec2(0.0f, btn_h))) {
                    core::HintsManager::OpenPathInShell(build_log->GetLogsDirectory());
                }
            }
        }

        // Refresh filtered index vector incrementally only when log count/filter changes
        const bool using_filtered_list = (!in_run_tab && build_log && disk_log_lines > 0 && (s_FilterMode != 0 || !s_TargetFilter.empty()));
        if (using_filtered_list) {
            if (s_CacheLogLines != disk_log_lines || s_CacheMode != s_FilterMode || s_CacheTarget != s_TargetFilter) {
                s_CachedFiltered = build_log->GetFilteredIndices(static_cast<core::LogFilterMode>(s_FilterMode), s_TargetFilter, "");
                s_CacheLogLines = disk_log_lines;
                s_CacheMode = s_FilterMode;
                s_CacheTarget = s_TargetFilter;
            }
        }

        // ---------------------------------------------------------------------
        // Footer / Run Stdin Sizing
        // ---------------------------------------------------------------------
        const float footer_h = Theme::Dim::FooterH;
        const float stdin_h = in_run_tab ? (Theme::Btn::DefaultH + Theme::Sp::S2) : 0.0f;
        const bool show_summary_card = (!in_run_tab && !is_running && status == core::BuildStatus::Finished);
        const float summary_reserve = show_summary_card ? (92.0f * Theme::DpiScale) : 0.0f;
        const float avail_h = ImGui::GetContentRegionAvail().y;
        const float item_gap_y = ImGui::GetStyle().ItemSpacing.y;
        const float scroll_h = std::max(40.0f, avail_h - footer_h - stdin_h - summary_reserve - item_gap_y * (in_run_tab ? 2.0f : 1.0f));

        ImGui::PushFont(g_Fonts.mono);

        const float mono_size = 13.5f * Theme::DpiScale;
        const float target_line_h = mono_size * 1.35f;
        const float line_gap = target_line_h - ImGui::GetTextLineHeight();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, (line_gap > 0.0f) ? line_gap : 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Sp::S3, Theme::Sp::S2));

        if (ImGui::BeginChild("OutputInnerScroll", ImVec2(0, scroll_h), false)) {
            if (in_run_tab) {
                if (run_start_error && !run_start_error->empty()) {
                    ImGui::TextColored(Theme::Col::Error, "> %s", run_start_error->c_str());
                } else if (run_lines && !run_lines->empty()) {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(run_lines->size()), target_line_h);
                    while (clipper.Step()) {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                            const auto& line = (*run_lines)[i];
                            if (line.kind == core::OutLineKind::Command) {
                                ImGui::TextColored(Theme::Col::TextDim, "> %s", line.text.c_str());
                            } else if (line.kind == core::OutLineKind::Status) {
                                ImGui::TextColored(line.has_error ? Theme::Col::Error : Theme::Col::Success, "%s", line.text.c_str());
                            } else {
                                ImGui::TextUnformatted(line.text.empty() ? " " : line.text.c_str());
                            }
                        }
                    }
                    if (s_RunAutoScroll && (run_runner->IsRunning() || new_run_lines)) {
                        ImGui::SetScrollHereY(1.0f);
                    }
                } else {
                    ImGui::TextColored(Theme::Col::TextDim, "Press Run (Ctrl+R) to launch the startup target.");
                }
            } else {
                if (ImGui::IsWindowHovered()) {
                    const float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel > 0.0f) {
                        s_AutoScroll = false;
                    } else if (wheel < 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
                        s_AutoScroll = true;
                    }
                }

                if (ImGui::GetScrollMaxY() > 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
                    s_AutoScroll = true;
                }

                if (s_PendingJumpLine >= 0) {
                    ImGui::SetScrollY(static_cast<float>(s_PendingJumpLine) * target_line_h);
                    s_PendingJumpLine = -1;
                }

                if (!build_start_error.empty()) {
                    ImGui::TextColored(Theme::Col::Error, "> %s", build_start_error.c_str());
                } else if (active_build_lines == 0) {
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
                    const size_t render_count = using_filtered_list
                        ? s_CachedFiltered.size()
                        : ((disk_log_lines > 0) ? disk_log_lines : output_lines.size());

                    core::UserSettings user_prefs;
                    bool loaded_prefs = false;
                    std::filesystem::path root_p = project ? project->GetRootPath() : std::filesystem::path();

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(render_count), line_height);

                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            uint32_t actual_idx = using_filtered_list
                                ? s_CachedFiltered[static_cast<size_t>(row)]
                                : static_cast<uint32_t>(row);

                            core::OutLineKind k = core::OutLineKind::Raw;
                            bool has_err = false;
                            bool has_warn = false;
                            std::string txt;

                            if (disk_log_lines > 0 && build_log) {
                                core::LogLineEntry meta;
                                if (!build_log->GetLineMetaAndText(actual_idx, meta, txt)) continue;
                                k = static_cast<core::OutLineKind>(meta.kind);
                                has_err = (meta.flags & 1) != 0;
                                has_warn = (meta.flags & 2) != 0;
                            } else {
                                const auto& ol = output_lines[actual_idx];
                                k = ol.kind;
                                has_err = ol.has_error;
                                has_warn = ol.has_warning;
                                txt = ol.text;
                            }

                            ImVec2 cur_pos = ImGui::GetCursorScreenPos();
                            if (static_cast<int>(actual_idx) == s_FocusedErrorLine) {
                                dl->AddRectFilled(cur_pos, ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height), IM_COL32(250, 204, 21, 35));
                            }

                            if (k == core::OutLineKind::Command) {
                                ImGui::TextColored(Theme::Col::TextDim, "$ %s", txt.c_str());
                                dl->AddLine(
                                    ImVec2(cur_pos.x, cur_pos.y + line_height - 1.0f),
                                    ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height - 1.0f),
                                    ImGui::GetColorU32(Theme::Col::Border)
                                );
                            } else if (k == core::OutLineKind::Progress) {
                                ImGui::TextColored(Theme::Col::Accent, "%s", txt.c_str());
                            } else if (k == core::OutLineKind::Status) {
                                if (!has_err) {
                                    ImGui::TextColored(Theme::Col::Success, "%s", txt.c_str());
                                } else {
                                    ImGui::TextColored(Theme::Col::Error, "%s", txt.c_str());
                                }
                            } else if (txt.empty()) {
                                ImGui::TextUnformatted(" ");
                            } else {
                                if (has_err) {
                                    dl->AddRectFilled(cur_pos, ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height), IM_COL32(239, 68, 68, 25));
                                    dl->AddRectFilled(cur_pos, ImVec2(cur_pos.x + 2.5f * Theme::DpiScale, cur_pos.y + line_height), IM_COL32(239, 68, 68, 220));
                                } else if (has_warn) {
                                    dl->AddRectFilled(cur_pos, ImVec2(cur_pos.x + full_line_w, cur_pos.y + line_height), IM_COL32(245, 158, 11, 22));
                                    dl->AddRectFilled(cur_pos, ImVec2(cur_pos.x + 2.5f * Theme::DpiScale, cur_pos.y + line_height), IM_COL32(245, 158, 11, 220));
                                }

                                // Raw text untouched (Principle 4); clickable diagnostic jump-to-editor (V2-21)
                                ImGui::TextUnformatted(txt.c_str());
                                if ((has_err || has_warn) && ImGui::IsItemHovered()) {
                                    std::filesystem::path diag_file;
                                    int diag_line = 0, diag_col = 1;
                                    if (core::HintsManager::ParseDiagnosticLocation(txt, root_p, diag_file, diag_line, diag_col)) {
                                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                        Theme::Tooltip(("Click to open " + diag_file.filename().generic_u8string() + ":" + std::to_string(diag_line)).c_str());
                                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                                            if (!loaded_prefs) {
                                                user_prefs = core::HintsManager::LoadUserSettings();
                                                loaded_prefs = true;
                                            }
                                            core::HintsManager::LaunchEditorForLocation(user_prefs.editorCommand, diag_file, diag_line, diag_col);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (s_AutoScroll && (is_running || new_lines_arrived) && !using_filtered_list) {
                        ImGui::SetScrollHereY(1.0f);
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopFont();

        // V2-20 Summary Card (counts, phase timings, top slowest files, cache)
        if (show_summary_card) {
            core::BuildResult res = build_runner.GetLastResult();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Sp::S2, Theme::Sp::S1));
            if (ImGui::BeginChild("BuildSummaryCard", ImVec2(0, summary_reserve), true)) {
                ImGui::TextColored(Theme::Col::Text, "Summary: %zu compiled, %zu up-to-date, %zu failed, %zu skipped  |  Steps %.2fs · PCH %.2fs · Compile %.2fs · Link %.2fs",
                    res.compiled_count, res.up_to_date_count, res.failed_count, res.skipped_count,
                    res.steps_seconds, res.pch_seconds, res.compile_seconds, res.link_seconds);

                if (!res.slowest_files.empty()) {
                    ImGui::TextColored(Theme::Col::TextDim, "Slowest files:");
                    const size_t max_show = std::min<size_t>(res.slowest_files.size(), 5);
                    for (size_t si = 0; si < max_show; ++si) {
                        if (si > 0) ImGui::SameLine(0.0f, Theme::Sp::S2);
                        const auto& sf = res.slowest_files[si];
                        std::string fname = std::filesystem::u8path(sf.path).filename().generic_u8string();
                        if (fname.empty()) fname = sf.path;
                        std::ostringstream lbl;
                        lbl << fname << " (" << std::fixed << std::setprecision(2) << (sf.durationMs / 1000.0) << "s)";
                        if (ImGui::SmallButton((lbl.str() + "##slow" + std::to_string(si)).c_str())) {
                            if (project) {
                                std::filesystem::path abs_f = project->ResolvePath(sf.path);
                                core::UserSettings up = core::HintsManager::LoadUserSettings();
                                core::HintsManager::LaunchEditorForLocation(up.editorCommand, abs_f, 1, 1);
                            }
                        }
                        Theme::Tooltip(sf.path.c_str());
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }

        // One-line stdin box for Run tab (V2-16)
        if (in_run_tab) {
            static char s_StdinBuf[512] = "";
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::Sp::S2);
            const float send_w = 56.0f * Theme::DpiScale;
            const float input_w = std::max(80.0f, ImGui::GetContentRegionAvail().x - send_w - Theme::Sp::S3);
            bool active_proc = run_runner && run_runner->IsRunning();
            if (!active_proc) ImGui::BeginDisabled(true);
            ImGui::SetNextItemWidth(input_w);
            bool submit = ImGui::InputTextWithHint("##RunStdinInput", active_proc ? "Send line to stdin (Enter)..." : "Process not running", s_StdinBuf, sizeof(s_StdinBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0.0f, Theme::Sp::S1);
            if (Theme::Button("Send", "Send text + newline to process stdin", false, ImVec2(send_w, Theme::Btn::DefaultH))) {
                submit = true;
            }
            if (submit && active_proc) {
                run_runner->SendStdin(s_StdinBuf);
                s_StdinBuf[0] = '\0';
                ImGui::SetKeyboardFocusHere(-2);
            }
            if (!active_proc) ImGui::EndDisabled();
        }

        // Pinned Footer Area
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
            std::string suffix;
            if (res.active_worker_count > 0) {
                suffix += " · " + std::to_string(res.active_worker_count) + " jobs";
                if (res.ram_capped) suffix += " (RAM-capped)";
            }
            if (res.cache_summary.valid && (res.cache_summary.hits + res.cache_summary.misses) > 0) {
                int hit_pct = static_cast<int>(res.cache_summary.hitRatePercent + 0.5);
                suffix += " · " + res.cache_summary.toolName + " " + std::to_string(hit_pct) + "% hit";
            }
            if (res.compile_commands_ok) {
                suffix += " · compile_commands " ICON_FA_CHECK;
            }
            if (res.cancelled) {
                std::string msg = "■ Build cancelled · " + std::to_string(res.compiled_count) + " compiled · " + FormatDuration(res.duration_seconds) + "s" + suffix;
                ImGui::TextColored(Theme::Col::Warning, "%s", msg.c_str());
            } else if (res.success) {
                std::string msg = std::string(ICON_FA_CHECK) + " Build succeeded · exit 0 · " + FormatDuration(res.duration_seconds) + "s" + suffix;
                ImGui::TextColored(Theme::Col::Success, "%s", msg.c_str());
            } else {
                std::string msg = std::string(ICON_FA_XMARK) + " Build failed · exit " + std::to_string(res.exit_code) + " · " + FormatDuration(res.duration_seconds) + "s" + suffix;
                ImGui::TextColored(Theme::Col::Error, "%s", msg.c_str());
            }
            ImGui::PopFont();
        } else if (is_running) {
            ImGui::PushFont(g_Fonts.bold);
            const float text_y = footer_top_y + (footer_h - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
            int elapsed = static_cast<int>(build_runner.GetElapsedSeconds());
            int workers = build_runner.GetActiveWorkerCount();
            std::string msg = std::string(ICON_FA_SPINNER) + " Building... " + std::to_string(elapsed) + "s";
            if (workers > 0) {
                msg += " · " + std::to_string(workers) + " jobs";
                if (build_runner.IsRamCapped()) msg += " (RAM-capped)";
            }
            if (build_runner.IsThrottledLowMemory()) {
                msg += " · throttled (low memory)";
            }
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
