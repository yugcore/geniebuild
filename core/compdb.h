#pragma once

#include "target.h"

#include <filesystem>
#include <string>
#include <vector>

namespace core {

class Project;
class CompilerManager;
struct ToolchainProfile;

struct CompDbExportResult {
    bool success = false;
    bool fileModified = false;
    size_t entryCount = 0;
    std::filesystem::path outputPath;
    std::string error;
};

struct ImportedFolderGroup {
    std::string folderRel;
    std::vector<std::string> files;
    std::vector<std::string> includeDirs;
    std::vector<std::string> systemIncludeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> compileFlags;
    bool useFolderRule = false;
};

struct ImportedTargetSuggestion {
    std::string name;
    TargetKind kind = TargetKind::Executable;
    std::vector<std::string> deps;
    std::vector<std::string> files;
    std::vector<std::string> includeDirs;
    std::vector<std::string> systemIncludeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> compileFlags;
    std::string cxxStd;
    std::string cStd;
    std::string warnings;
    std::vector<ImportedFolderGroup> folders;
};

struct ImportPlan {
    bool success = false;
    std::string error;
    std::filesystem::path compdbPath;
    std::filesystem::path projectRoot;
    size_t totalEntriesParsed = 0;
    size_t excludedGeneratedCount = 0;

    // Project-wide factored baseline flags
    std::vector<std::string> projectIncludeDirs;
    std::vector<std::string> projectSystemIncludeDirs;
    std::vector<std::string> projectDefines;
    std::vector<std::string> projectCompileFlags;
    std::string cxxStd = "c++17";
    std::string cStd = "c11";
    std::string warnings = "-Wall";

    std::vector<ImportedTargetSuggestion> targets;
    std::vector<std::string> warningsList;
};

// V2-19 Export compile_commands.json & V2-24 Import existing compile_commands.json
class CompDbManager {
public:
    // V2-19: Exports per-file commands for clangd/IDEs atomically only if file content changes.
    static CompDbExportResult ExportCompileCommands(
        const Project& project,
        const CompilerManager& compiler_manager,
        const ToolchainProfile* active_profile_override = nullptr
    );

    // Parses a compiler command line string into argv (handles both POSIX and cl.exe quoting)
    static std::vector<std::string> SplitCommandLine(const std::string& command_str);

    // V2-24: Parses compile_commands.json, classifies flags, suggests CMake/folder targets,
    // factors common project/target/folder overrides, and flags generated build-dir sources.
    static ImportPlan AnalyzeCompileCommands(
        const std::filesystem::path& compdb_path,
        const std::filesystem::path& project_root
    );

    // V2-24: Backs up existing project.json (if present) and writes new v2 project.json from ImportPlan.
    static bool WriteImportedProject(
        const ImportPlan& plan,
        const std::filesystem::path& project_root,
        std::string& out_error
    );
};

} // namespace core
