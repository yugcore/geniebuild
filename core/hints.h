#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace core {

struct UserSettings {
    std::string editorCommand; // e.g. "code -g {file}:{line}:{col}"
    bool showPerformanceHints = true;
    bool defenderDismissedGlobal = false;
    std::vector<std::string> defenderDismissedProjects;
    bool longPathBannerDismissed = false;
};

// V2-21 / V2-22 / V2-23 User preferences & non-intrusive system environment hints
class HintsManager {
public:
    static std::filesystem::path GetUserSettingsPath();
    static UserSettings LoadUserSettings();
    static void SaveUserSettings(const UserSettings& s);

    // V2-23: Queries WMI MSFT_MpComputerStatus.RealTimeProtectionEnabled (no admin required)
    static bool IsWindowsDefenderRealTimeActive();

    // V2-23: Queries ReFS Dev Drive flag (Win11 23H2+); returns true on Dev Drive
    static bool IsPathOnDevDrive(const std::filesystem::path& path);

    // Evaluates once after a clean build of >500 files on Windows whether to suggest Defender exclusion
    static bool ShouldSuggestDefenderExclusion(
        const std::filesystem::path& project_root,
        size_t compiled_file_count,
        const UserSettings& settings
    );

    static void DismissDefenderForProject(const std::filesystem::path& project_root, bool forever_all_projects = false);

    // V2-21: Parses file(line), file(line,col), or file:line:col from a raw compiler diagnostic line
    static bool ParseDiagnosticLocation(
        const std::string& raw_line,
        const std::filesystem::path& project_root,
        std::filesystem::path& out_file,
        int& out_line,
        int& out_col
    );

    // V2-21: Launches configured editor command (with {file}, {line}, {col}) or system default opener
    static bool LaunchEditorForLocation(
        const std::string& editor_template,
        const std::filesystem::path& file_path,
        int line,
        int col
    );

    // Opens a file or folder in the host OS shell (for "Open full log" / "Open logs folder")
    static void OpenPathInShell(const std::filesystem::path& target);
};

} // namespace core
