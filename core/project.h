#pragma once

#include "source_rules.h"
#include "target.h"
#include "tool_registry.h"
#include "toolchain.h"

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>

namespace core {

// Configuration-specific flags, base preset, and defines (V2-4)
struct ConfigOverrides {
    std::string base;      // "", "debug", "release", "relWithDebInfo", "minSize"
    std::string optimize;  // "", "O0", "O1", "O2", "O3", "Os"
    std::string debugInfo; // "", "none", "line", "full"
    std::string runtime;   // "", "dynamic", "static"
    std::string lto;       // "", "off", "thin", "full"
    std::vector<std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
};

// V2-19 compile_commands.json export configuration
struct CompileCommandsConfig {
    bool enabled = true;
    std::string path = "compile_commands.json";
    bool allConfigs = false;
};

// Manages persistent recent project paths in %APPDATA%/GenieBuild/recent.json (Spec §5)
class RecentProjects {
public:
    static std::vector<std::string> Load();
    static void Add(const std::string& root_path);
    static void Remove(const std::string& root_path);
    static std::filesystem::path GetRecentFilePath();
};

// Manages project state, build file list, and serialization to/from .geniebuild/project.json.
// Note: core/ has zero dependencies on UI, GLFW, or ImGui.
class Project {
public:
    Project();
    explicit Project(const std::filesystem::path& root_path);
    ~Project();

    // Loads an existing project from root_path.
    // If <root>/.geniebuild/project.json exists, loads and restores it.
    // If not, runs a background scan and pre-fills the build list with .c/.cpp/.cc/.cxx files,
    // skipping .git, .geniebuild, build, thirdparty, 3rdparty, third_party, external, deps, vendor, node_modules, tests.
    bool Load(const std::filesystem::path& root_path);

    // Updates UI-thread state: swaps in completed background scan snapshot and flushes debounced save
    void Update();

    // Saves current settings to <root>/.geniebuild/project.json (debounced by 500ms)
    bool Save() const;

    // Saves current settings immediately to <root>/.geniebuild/project.json (via temp file + atomic rename)
    bool SaveImmediate() const;

    // Marks project settings dirty for debounced saving
    void MarkDirty() const;

    // Project root directory
    bool HasProject() const { return !m_root_path.empty(); }
    const std::filesystem::path& GetRootPath() const { return m_root_path; }
    void SetRootPath(const std::filesystem::path& root);

    // Compiler selection (e.g. "g++", "clang++", "cl")
    const std::string& GetCompiler() const { return m_compiler; }
    void SetCompiler(const std::string& compiler);

    // Paired C compiler selection (e.g. "gcc", "clang", "cl") (Spec §5.1.6)
    const std::string& GetCCompiler() const { return m_c_compiler; }
    void SetCCompiler(const std::string& c_compiler);

    // V2-14 Active toolchain profile & project-local custom profiles
    const std::string& GetToolchain() const { return m_toolchain; }
    void SetToolchain(const std::string& toolchain_name, const std::string& fallback_compiler = "", const std::string& fallback_c_compiler = "");
    const std::vector<ToolchainProfile>& GetProjectToolchains() const { return m_project_toolchains; }
    void SetProjectToolchains(const std::vector<ToolchainProfile>& toolchains);

    // Active configuration: "Debug" or "Release" (Spec §5.1.4)
    const std::string& GetConfiguration() const { return m_configuration; }
    void SetConfiguration(const std::string& config);

    // Language standards (Spec §5.1.3)
    const std::string& GetCxxStd() const { return m_cxx_std; }
    void SetCxxStd(const std::string& std_name);
    const std::string& GetCStd() const { return m_c_std; }
    void SetCStd(const std::string& std_name);

    // Parallel compile jobs (0 = auto-detect all cores / RAM-aware in V2-13)
    int GetJobs() const { return m_jobs; }
    void SetJobs(int jobs);

    // V2-7 Tool overrides & custom tools
    const std::map<std::string, std::string>& GetToolOverrides() const { return m_tool_overrides; }
    void SetToolOverrides(const std::map<std::string, std::string>& overrides);
    const std::vector<CustomToolSpec>& GetCustomTools() const { return m_custom_tools; }
    void SetCustomTools(const std::vector<CustomToolSpec>& tools);
    const std::string& GetAsmMode() const { return m_asm_mode; }
    void SetAsmMode(const std::string& mode);

    // V2-11 Dep log options
    bool GetIgnoreSystemHeaders() const { return m_ignore_system_headers; }
    void SetIgnoreSystemHeaders(bool ignore);
    bool GetKeepDepFiles() const { return m_keep_dep_files; }
    void SetKeepDepFiles(bool keep);

    // V2-12 Linker options
    const std::string& GetLinker() const { return m_linker; }
    void SetLinker(const std::string& linker);
    bool GetIncrementalLink() const { return m_incremental_link; }
    void SetIncrementalLink(bool inc);
    const std::string& GetDebugLink() const { return m_debug_link; }
    void SetDebugLink(const std::string& mode);
    bool GetSplitDwarf() const { return m_split_dwarf; }
    void SetSplitDwarf(bool split);

    // V2-13 Scheduler options
    int GetMemPerJobMb() const { return m_mem_per_job_mb; }
    void SetMemPerJobMb(int mb);
    int GetLinkJobs() const { return m_link_jobs; }
    void SetLinkJobs(int lj);
    bool GetKeepGoing() const { return m_keep_going; }
    void SetKeepGoing(bool kg);
    bool GetLowPriority() const { return m_low_priority; }
    void SetLowPriority(bool lp);

    // Warning level (e.g. "Off", "-Wall", "-Wall -Wextra")
    const std::string& GetWarnings() const { return m_warnings; }
    void SetWarnings(const std::string& warnings);

    // User include directories (Spec §5.1.3)
    const std::vector<std::string>& GetIncludeDirs() const { return m_include_dirs; }
    void SetIncludeDirs(const std::vector<std::string>& dirs);

    // Library search directories and library names (Spec §5.1.2)
    const std::vector<std::string>& GetLibDirs() const { return m_lib_dirs; }
    void SetLibDirs(const std::vector<std::string>& dirs);
    const std::vector<std::string>& GetLibs() const { return m_libs; }
    void SetLibs(const std::vector<std::string>& libs);
    const std::map<std::string, std::string>& GetLibConditions() const { return m_lib_conditions; }
    void SetLibConditions(const std::map<std::string, std::string>& conds);

    // Preprocessor definitions (Spec §5.1.3)
    const std::vector<std::string>& GetDefines() const { return m_defines; }
    void SetDefines(const std::vector<std::string>& defines);

    // DLLs to copy next to binary after successful link (Spec §5.1.2)
    const std::vector<std::string>& GetCopyDlls() const { return m_copy_dlls; }
    void SetCopyDlls(const std::vector<std::string>& dlls);

    // Raw compile and link flags (Spec §5.1.3)
    const std::vector<std::string>& GetCompileFlags() const { return m_compile_flags; }
    void SetCompileFlags(const std::vector<std::string>& flags);
    const std::vector<std::string>& GetLinkFlags() const { return m_link_flags; }
    void SetLinkFlags(const std::vector<std::string>& flags);

    // Per-configuration overrides & custom configuration management (V2-4)
    ConfigOverrides GetConfigOverrides(const std::string& config) const;
    void SetConfigOverrides(const std::string& config, const ConfigOverrides& overrides);
    const std::map<std::string, ConfigOverrides>& GetAllConfigs() const { return m_configs; }
    std::vector<std::string> GetConfigNames() const;
    bool AddConfiguration(const std::string& name, const ConfigOverrides& spec, std::string& out_error);
    bool DuplicateConfiguration(const std::string& src_name, const std::string& new_name, std::string& out_error);
    bool RenameConfiguration(const std::string& old_name, const std::string& new_name, std::string& out_error);
    bool DeleteConfiguration(const std::string& name, std::string& out_error);
    const std::vector<std::string>& GetSchemaWarnings() const { return m_schema_warnings; }

    // Resolves a relative or absolute path against the project root
    std::filesystem::path ResolvePath(const std::string& path_str) const;

    // Object directory for a configuration: .geniebuild/obj/<Config>/<target>
    std::filesystem::path GetObjectDir(const std::string& config) const;

    // Target binary path for a configuration: <outputDir>/<Config>/bin/<appName>.exe
    std::filesystem::path GetBinaryPath(const std::string& config) const;

    // Multi-target & source-rule APIs (V2-1, V2-2)
    int GetSchemaVersion() const { return m_schema_version; }
    const std::vector<Target>& GetTargets() const { return m_targets; }
    std::vector<Target> GetTargetsSnapshot() const;
    const Target* FindTarget(const std::string& name) const;
    const Target* GetPrimaryTarget() const;
    const std::string& GetStartupTarget() const { return m_startup_target; }
    void SetStartupTarget(const std::string& name);

    // Progressive disclosure (§4.3): true if >1 target or any target has rules/v2 fields
    bool IsMultiTargetOrRulesUI() const;

    bool AddTarget(const Target& target, std::string& out_error);
    bool RemoveTarget(const std::string& name, std::string& out_error);
    bool RenameTarget(const std::string& old_name, const std::string& new_name, std::string& out_error);
    bool DuplicateTarget(const std::string& name, std::string& out_new_name);
    bool UpdateTarget(const Target& updated);

    void AddSourceRuleToTarget(const std::string& target_name, const SourceRule& rule);
    void AddFilesToTarget(const std::string& target_name, const std::vector<std::string>& rel_paths);
    void RemoveFileFromTarget(const std::string& target_name, const std::string& rel_path);
    void RemoveFilesFromTarget(const std::string& target_name, const std::vector<std::string>& rel_paths);
    void SetTargetFileEnabled(const std::string& target_name, const std::string& rel_path, bool enabled);
    void SetTargetFolderEnabled(const std::string& target_name, const std::string& folder_rel_path, bool enabled);

    // Expands source rules across all targets (and updates missing file caches & file-to-target map)
    void ExpandAllSourceRules(bool mark_dirty_dirs = false);
    std::vector<std::string> GetTargetsOwningFile(const std::string& rel_path) const;
    std::unordered_map<std::string, std::vector<std::string>> GetMultiTargetFilesCopy() const;
    DirectoryCache& GetDirectoryCache() { return m_dir_cache; }

    // Output directory (defaults to ".geniebuild/out")
    const std::string& GetOutputDir() const { return m_output_dir; }
    void SetOutputDir(const std::string& output_dir);

    // Resolves the output directory to an absolute filesystem path
    std::filesystem::path GetAbsoluteOutputDir() const;

    // Sets output directory: converts to relative if inside project root, otherwise stores absolute
    void SetOutputDirPath(const std::filesystem::path& path);

    // List of source files queued for compilation (paths relative to project root)
    const std::vector<std::string>& GetFiles() const { return m_files; }

    // Fast O(1) set of queued files for tree badge lookups
    const std::unordered_set<std::string>& GetFilesSet() const { return m_files_set; }

    // Thread-safe snapshots of queued files (§2.8)
    std::vector<std::string> GetFilesCopy() const;
    std::unordered_set<std::string> GetFilesSetCopy() const;

    // Adds a file to the build list. Normalizes path with forward slashes and prevents duplicates.
    // Automatically saves the updated project file. Returns true if added, false if already present.
    bool AddFile(const std::string& relative_path);

    // Batch add files to the build list. Returns number of newly added files.
    size_t AddFiles(const std::vector<std::string>& relative_paths);

    // Removes a file from the build list. Automatically saves the updated project file.
    bool RemoveFile(const std::string& relative_path);

    // Batch remove files from the build list. Returns number of removed files.
    size_t RemoveFiles(const std::vector<std::string>& relative_paths);

    // Checks if a given relative path is already queued in the build list (O(1) hash lookup)
    bool ContainsFile(const std::string& relative_path) const;

    // Checks missing files against disk once and caches the result (no per-frame filesystem calls)
    void RefreshMissingFilesCache();

    // Query missing file status from cache (O(1) without touching disk)
    bool IsFileMissingCached(const std::string& relative_path) const;
    bool FileExistsOnDisk(const std::string& relative_path) const;

    // Checks whether any file in the build list is missing on disk (from cache)
    bool HasMissingFiles() const { return !m_missing_files_set.empty(); }

    // Returns a list of all files that are queued but missing on disk (from cache)
    const std::vector<std::string>& GetMissingFiles() const { return m_missing_files; }

    // Checkbox compile inclusion: files can be checked (compiled) or unchecked (excluded from build)
    bool IsFileEnabled(const std::string& relative_path) const;
    void SetFileEnabled(const std::string& relative_path, bool enabled);
    void ToggleFileEnabled(const std::string& relative_path);
    void SetFolderEnabled(const std::string& folder_rel_path, bool enabled);
    size_t GetEnabledFilesCount() const;
    std::vector<std::string> GetCompiledFiles() const;

    // Revision counter bumped whenever file list changes
    uint64_t Revision() const { return m_revision.load(); }

    // Clears all files from the build list
    void ClearFiles();

    // Pane widths (optional field in project.json for draggable splitters)
    const std::vector<float>& GetPaneWidths() const { return m_pane_widths; }
    void SetPaneWidths(const std::vector<float>& widths);

    // Huge-list warning confirmation setting (saved in project.json)
    bool GetConfirmLargeBuild() const { return m_confirm_large_build; }
    void SetConfirmLargeBuild(bool confirm) { m_confirm_large_build = confirm; MarkDirty(); }

    // V2-19 compile_commands.json export configuration
    const CompileCommandsConfig& GetCompileCommandsConfig() const { return m_compile_commands_cfg; }
    void SetCompileCommandsConfig(const CompileCommandsConfig& cfg);

    // Background auto-fill scan state
    bool IsScanning() const { return m_is_scanning.load(); }

    // Helper: checks if a directory is a build output folder (contains CMakeCache.txt, build.ninja, or CMakeFiles) (Spec §6.2 B2)
    static bool IsBuildOutputFolder(const std::filesystem::path& path);
    bool IsBuildOutputFolder() const { return m_is_build_output_folder; }

    // Helper: checks if a directory should be skipped during scan
    static bool ShouldSkipDirectory(const std::string& dir_name);

private:
    std::filesystem::path m_root_path;
    bool m_is_build_output_folder = false;
    int m_schema_version = 1;
    bool m_loaded_as_v1 = false;
    std::string m_startup_target;
    std::vector<Target> m_targets;
    std::unordered_map<std::string, std::vector<std::string>> m_file_to_targets;
    std::unordered_map<std::string, std::vector<std::string>> m_multi_target_files;
    mutable DirectoryCache m_dir_cache;
    FileWatcher m_file_watcher;

    std::string m_compiler = "g++";
    std::string m_c_compiler = "gcc";
    std::string m_toolchain;
    std::vector<ToolchainProfile> m_project_toolchains;
    std::string m_configuration = "Debug";
    std::string m_cxx_std = "c++17";
    std::string m_c_std = "c11";
    int m_jobs = 0;
    std::string m_warnings = "-Wall";
    std::vector<std::string> m_include_dirs;
    std::vector<std::string> m_lib_dirs;
    std::vector<std::string> m_libs;
    std::map<std::string, std::string> m_lib_conditions;
    std::vector<std::string> m_defines;
    std::vector<std::string> m_copy_dlls;
    std::vector<std::string> m_compile_flags;
    std::vector<std::string> m_link_flags;
    std::map<std::string, ConfigOverrides> m_configs;
    std::vector<std::string> m_schema_warnings;

    // V2-7 / V2-11 / V2-12 / V2-13 project fields
    std::map<std::string, std::string> m_tool_overrides;
    std::vector<CustomToolSpec> m_custom_tools;
    std::string m_asm_mode = "auto";
    bool m_ignore_system_headers = true;
    bool m_keep_dep_files = false;
    std::string m_linker = "default";
    bool m_incremental_link = true;
    std::string m_debug_link = "full";
    bool m_split_dwarf = false;
    int m_mem_per_job_mb = 2048;
    int m_link_jobs = 1;
    bool m_keep_going = false;
    bool m_low_priority = true;

    std::string m_output_dir = ".geniebuild/out";
    std::vector<std::string> m_files;
    std::unordered_set<std::string> m_files_set; // O(1) set kept in sync with m_files
    std::vector<float> m_pane_widths; // Saved pane widths for draggable splitters
    bool m_confirm_large_build = true; // Huge-list warning confirmation flag (Spec §3.7)
    CompileCommandsConfig m_compile_commands_cfg;

    // Cached missing files (checked once when list changes or on refresh)
    std::unordered_set<std::string> m_missing_files_set;
    std::vector<std::string> m_missing_files;
    std::unordered_set<std::string> m_disabled_files; // Files in project but unchecked/excluded from compile
    std::unordered_map<std::string, size_t> m_file_indices; // Fast O(1) index map for RemoveFile (§2.4)
    void RebuildIndexMap();
    std::atomic<uint64_t> m_revision{0};

    // Background scan support
    std::atomic<bool> m_is_scanning{false};
    std::atomic<bool> m_abort_scan{false};
    std::thread m_scan_thread;
    mutable std::mutex m_data_mutex;

    struct PendingScanResult {
        std::vector<std::string> files;
        std::unordered_set<std::string> files_set;
    };
    std::mutex m_scan_mutex;
    std::unique_ptr<PendingScanResult> m_pending_scan;
    std::atomic<bool> m_has_pending_scan{false};

    mutable bool m_save_dirty = false;
    mutable std::chrono::steady_clock::time_point m_last_dirty_time;
    void FlushDebouncedSave() const;

    // Normalizes path separators to forward slashes for cross-platform project.json files
    static std::string NormalizePath(const std::string& path_str);

    // Converts an absolute or relative path to a normalized relative path from project root
    std::string ToRelativePathString(const std::filesystem::path& path) const;

    // Scans project directory on background thread when no project.json exists
    void StartBackgroundScan();

    // Syncs flat compatibility caches (m_files, m_files_set, m_missing_files, etc.) from m_targets
    void SyncTargetsLocked();
    Target* FindTargetLocked(const std::string& name);
    const Target* FindTargetLocked(const std::string& name) const;
    Target& EnsureDefaultTargetLocked();
};

} // namespace core
