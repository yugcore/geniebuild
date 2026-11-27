#include "project.h"
#include "settings_resolver.h"
#include "fs_util.h"
#include "dep_log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <cctype>
#include <cstdlib>

namespace core {

// Helper: Convert string to lowercase for case-insensitive checks
static std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

// -----------------------------------------------------------------------------
// RecentProjects Implementation (Spec §5)
// -----------------------------------------------------------------------------
std::filesystem::path RecentProjects::GetRecentFilePath() {
    std::filesystem::path p;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        p = std::filesystem::path(appdata) / "GenieBuild" / "recent.json";
    }
#endif
    if (p.empty()) {
        const char* home = std::getenv("USERPROFILE");
        if (!home) home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            p = std::filesystem::path(home) / ".geniebuild" / "recent.json";
        } else {
            p = std::filesystem::temp_directory_path() / "GenieBuild" / "recent.json";
        }
    }
    return p;
}

std::vector<std::string> RecentProjects::Load() {
    std::vector<std::string> result;
    std::filesystem::path file_path = GetRecentFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec) || ec) {
        return result;
    }
    try {
        std::ifstream file(file_path);
        if (file.is_open()) {
            nlohmann::json j;
            file >> j;
            if (j.is_array()) {
                for (const auto& item : j) {
                    if (item.is_string()) {
                        std::string path_str = item.get<std::string>();
                        std::error_code check_ec;
                        if (std::filesystem::exists(path_str, check_ec) && !check_ec &&
                            std::filesystem::is_directory(path_str, check_ec)) {
                            result.push_back(path_str);
                        }
                    }
                }
            }
        }
    } catch (...) {}
    if (result.size() > 8) {
        result.resize(8);
    }
    return result;
}

void RecentProjects::Add(const std::string& root_path) {
    if (root_path.empty()) return;
    std::error_code ec;
    const std::filesystem::path root_lp = fs_util::ToWinLongPath(root_path);
    if (!fs_util::ExistsLong(root_path) || !std::filesystem::is_directory(root_lp, ec)) {
        return;
    }
    std::string norm = root_path;
    std::replace(norm.begin(), norm.end(), '\\', '/');

    std::vector<std::string> list = Load();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& p) {
        std::string cur = p;
        std::replace(cur.begin(), cur.end(), '\\', '/');
        return cur == norm;
    }), list.end());

    list.insert(list.begin(), norm);
    if (list.size() > 8) {
        list.resize(8);
    }

    try {
        std::filesystem::path file_path = GetRecentFilePath();
        std::filesystem::create_directories(file_path.parent_path(), ec);
        nlohmann::json j = list;
        std::ofstream file(file_path);
        if (file.is_open()) {
            file << j.dump(2) << std::endl;
        }
    } catch (...) {}
}

void RecentProjects::Remove(const std::string& root_path) {
    std::string norm = root_path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    std::vector<std::string> list = Load();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& p) {
        std::string cur = p;
        std::replace(cur.begin(), cur.end(), '\\', '/');
        return cur == norm;
    }), list.end());
    try {
        std::filesystem::path file_path = GetRecentFilePath();
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
        nlohmann::json j = list;
        std::ofstream file(file_path);
        if (file.is_open()) {
            file << j.dump(2) << std::endl;
        }
    } catch (...) {}
}

// Spec Milestone 6 / Part 1 Item 6 & Round 2 §6.2 B3: Extended auto-fill skip list:
// .git, .geniebuild, build, thirdparty, 3rdparty, third_party, external, deps, vendor, node_modules, tests,
// CMakeFiles, _deps, out, bin, obj, x64, Debug, Release, and cmake-build-*
bool Project::ShouldSkipDirectory(const std::string& dir_name) {
    if (dir_name.empty()) {
        return false;
    }
    if (dir_name[0] == '.') {
        return true; // hidden or dot folder (.git, .geniebuild, .vscode, .vs, etc.)
    }
    const std::string lower = ToLower(dir_name);
    if (lower.rfind("cmake-build-", 0) == 0) {
        return true;
    }
    static const std::unordered_set<std::string> s_skip_set = {
        "build", "thirdparty", "3rdparty", "third_party",
        "external", "deps", "vendor", "node_modules", "tests",
        "cmakefiles", "_deps", "out", "bin", "obj", "x64", "debug", "release"
    };
    return s_skip_set.find(lower) != s_skip_set.end();
}

bool Project::IsBuildOutputFolder(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    const std::filesystem::path lp = fs_util::ToWinLongPath(path);
    if (!std::filesystem::exists(lp, ec) || ec || !std::filesystem::is_directory(lp, ec)) {
        return false;
    }
    if (fs_util::ExistsLong(path / "CMakeCache.txt") ||
        fs_util::ExistsLong(path / "build.ninja") ||
        fs_util::ExistsLong(path / "CMakeFiles")) {
        return true;
    }
    return false;
}

Project::Project() = default;

Project::Project(const std::filesystem::path& root_path) {
    Load(root_path);
}

Project::~Project() {
    m_file_watcher.Stop();
    m_abort_scan.store(true);
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    if (m_save_dirty) {
        SaveImmediate();
        m_save_dirty = false;
    }
}

std::string Project::NormalizePath(const std::string& path_str) {
    std::string result = path_str;
    // Replace Windows backslashes with forward slashes for portable project.json files
    std::replace(result.begin(), result.end(), '\\', '/');

    // Strip leading "./" if present
    if (result.rfind("./", 0) == 0) {
        result = result.substr(2);
    }
    return result;
}

std::string Project::ToRelativePathString(const std::filesystem::path& path) const {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(path, m_root_path, ec);
    if (!ec) {
        return NormalizePath(rel.generic_u8string());
    }
    return NormalizePath(path.generic_u8string());
}

void Project::SetRootPath(const std::filesystem::path& root) {
    m_file_watcher.Stop();
    m_abort_scan.store(true);
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    std::error_code ec;
    std::filesystem::path abs_root = std::filesystem::absolute(root, ec);
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_root_path = ec ? root : abs_root;
        m_schema_version = 2;
        m_loaded_as_v1 = false;
        m_targets.clear();
        m_startup_target.clear();
        if (m_configs.empty()) {
            ConfigOverrides d; d.base = "debug"; d.defines = {"_DEBUG"};
            ConfigOverrides r; r.base = "release"; r.defines = {"NDEBUG"};
            m_configs["Debug"] = std::move(d);
            m_configs["Release"] = std::move(r);
        }
        SyncTargetsLocked();
    }
    m_revision++;
}

void Project::SetCompiler(const std::string& compiler) {
    if (m_compiler != compiler) {
        m_compiler = compiler;
        // Auto-pair C compiler if still on default
        if (m_compiler.find("clang") != std::string::npos) {
            m_c_compiler = "clang";
        } else if (m_compiler == "cl") {
            m_c_compiler = "cl";
        } else {
            m_c_compiler = "gcc";
        }
        ExpandAllSourceRules(false);
        Save();
    }
}

void Project::SetCCompiler(const std::string& c_compiler) {
    if (m_c_compiler != c_compiler) {
        m_c_compiler = c_compiler;
        Save();
    }
}

void Project::SetToolchain(const std::string& toolchain_name, const std::string& fallback_compiler, const std::string& fallback_c_compiler) {
    bool changed = (m_toolchain != toolchain_name);
    m_toolchain = toolchain_name;
    if (!fallback_compiler.empty() && m_compiler != fallback_compiler) {
        m_compiler = fallback_compiler;
        changed = true;
    }
    if (!fallback_c_compiler.empty() && m_c_compiler != fallback_c_compiler) {
        m_c_compiler = fallback_c_compiler;
        changed = true;
    }
    if (changed) {
        ExpandAllSourceRules(false);
        Save();
    }
}

void Project::SetProjectToolchains(const std::vector<ToolchainProfile>& toolchains) {
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_project_toolchains = toolchains;
        for (auto& tc : m_project_toolchains) tc.isProjectLocal = true;
    }
    Save();
}

void Project::SetConfiguration(const std::string& config) {
    if (m_configuration != config) {
        m_configuration = config;
        ExpandAllSourceRules(false);
        Save();
    }
}

void Project::SetCompileCommandsConfig(const CompileCommandsConfig& cfg) {
    m_compile_commands_cfg = cfg;
    Save();
}

void Project::SetCxxStd(const std::string& std_name) {
    if (m_cxx_std != std_name) {
        m_cxx_std = std_name;
        Save();
    }
}

void Project::SetCStd(const std::string& std_name) {
    if (m_c_std != std_name) {
        m_c_std = std_name;
        Save();
    }
}

void Project::SetJobs(int jobs) {
    if (m_jobs != jobs) {
        m_jobs = jobs;
        Save();
    }
}

void Project::SetToolOverrides(const std::map<std::string, std::string>& overrides) {
    m_tool_overrides = overrides;
    Save();
}

void Project::SetCustomTools(const std::vector<CustomToolSpec>& tools) {
    m_custom_tools = tools;
    ExpandAllSourceRules(false);
    Save();
}

void Project::SetAsmMode(const std::string& mode) {
    if (m_asm_mode != mode) {
        m_asm_mode = mode;
        Save();
    }
}

void Project::SetIgnoreSystemHeaders(bool ignore) {
    if (m_ignore_system_headers != ignore) {
        m_ignore_system_headers = ignore;
        Save();
    }
}

void Project::SetKeepDepFiles(bool keep) {
    if (m_keep_dep_files != keep) {
        m_keep_dep_files = keep;
        Save();
    }
}

void Project::SetLinker(const std::string& linker) {
    if (m_linker != linker) {
        m_linker = linker;
        Save();
    }
}

void Project::SetIncrementalLink(bool inc) {
    if (m_incremental_link != inc) {
        m_incremental_link = inc;
        Save();
    }
}

void Project::SetDebugLink(const std::string& mode) {
    if (m_debug_link != mode) {
        m_debug_link = mode;
        Save();
    }
}

void Project::SetSplitDwarf(bool split) {
    if (m_split_dwarf != split) {
        m_split_dwarf = split;
        Save();
    }
}

void Project::SetMemPerJobMb(int mb) {
    if (mb < 128) mb = 128;
    if (m_mem_per_job_mb != mb) {
        m_mem_per_job_mb = mb;
        Save();
    }
}

void Project::SetLinkJobs(int lj) {
    if (lj < 1) lj = 1;
    if (m_link_jobs != lj) {
        m_link_jobs = lj;
        Save();
    }
}

void Project::SetKeepGoing(bool kg) {
    if (m_keep_going != kg) {
        m_keep_going = kg;
        Save();
    }
}

void Project::SetLowPriority(bool lp) {
    if (m_low_priority != lp) {
        m_low_priority = lp;
        Save();
    }
}

void Project::SetWarnings(const std::string& warnings) {
    if (m_warnings != warnings) {
        m_warnings = warnings;
        Save();
    }
}

void Project::SetIncludeDirs(const std::vector<std::string>& dirs) {
    m_include_dirs = dirs;
    Save();
}

void Project::SetLibDirs(const std::vector<std::string>& dirs) {
    m_lib_dirs = dirs;
    Save();
}

void Project::SetLibs(const std::vector<std::string>& libs) {
    m_libs = libs;
    Save();
}

void Project::SetLibConditions(const std::map<std::string, std::string>& conds) {
    m_lib_conditions = conds;
    Save();
}

void Project::SetDefines(const std::vector<std::string>& defines) {
    m_defines = defines;
    Save();
}

void Project::SetCopyDlls(const std::vector<std::string>& dlls) {
    m_copy_dlls = dlls;
    Save();
}

void Project::SetCompileFlags(const std::vector<std::string>& flags) {
    m_compile_flags = flags;
    Save();
}

void Project::SetLinkFlags(const std::vector<std::string>& flags) {
    m_link_flags = flags;
    Save();
}

ConfigOverrides Project::GetConfigOverrides(const std::string& config) const {
    auto it = m_configs.find(config);
    if (it != m_configs.end()) {
        return it->second;
    }
    if (config == "Debug") {
        ConfigOverrides d;
        d.base = "debug";
        d.defines = {"_DEBUG"};
        return d;
    }
    if (config == "Release") {
        ConfigOverrides r;
        r.base = "release";
        r.defines = {"NDEBUG"};
        return r;
    }
    return {};
}

void Project::SetConfigOverrides(const std::string& config, const ConfigOverrides& overrides) {
    m_configs[config] = overrides;
    m_revision++;
    Save();
}

std::vector<std::string> Project::GetConfigNames() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::vector<std::string> names;
    // Keep Debug and Release first if present, then alphabetical custom configs
    if (m_configs.count("Debug")) names.push_back("Debug");
    if (m_configs.count("Release")) names.push_back("Release");
    for (const auto& kv : m_configs) {
        if (kv.first != "Debug" && kv.first != "Release") {
            names.push_back(kv.first);
        }
    }
    if (names.empty()) {
        names.push_back("Debug");
        names.push_back("Release");
    }
    return names;
}

bool Project::AddConfiguration(const std::string& name, const ConfigOverrides& spec, std::string& out_error) {
    out_error.clear();
    if (name.empty()) {
        out_error = "Configuration name cannot be empty.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        if (m_configs.find(name) != m_configs.end()) {
            out_error = "Configuration '" + name + "' already exists.";
            return false;
        }
        m_configs[name] = spec;
    }
    m_revision++;
    Save();
    return true;
}

bool Project::DuplicateConfiguration(const std::string& src_name, const std::string& new_name, std::string& out_error) {
    out_error.clear();
    if (new_name.empty()) {
        out_error = "Configuration name cannot be empty.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        auto it = m_configs.find(src_name);
        if (it == m_configs.end()) {
            out_error = "Source configuration '" + src_name + "' not found.";
            return false;
        }
        if (m_configs.find(new_name) != m_configs.end()) {
            out_error = "Configuration '" + new_name + "' already exists.";
            return false;
        }
        m_configs[new_name] = it->second;
        for (auto& t : m_targets) {
            auto tcit = t.configs.find(src_name);
            if (tcit != t.configs.end()) {
                t.configs[new_name] = tcit->second;
            }
            for (auto& ov : t.overrides) {
                auto ocit = ov.configs.find(src_name);
                if (ocit != ov.configs.end()) {
                    ov.configs[new_name] = ocit->second;
                }
            }
        }
    }
    m_revision++;
    Save();
    return true;
}

bool Project::RenameConfiguration(const std::string& old_name, const std::string& new_name, std::string& out_error) {
    out_error.clear();
    if (new_name.empty()) {
        out_error = "Configuration name cannot be empty.";
        return false;
    }
    if (old_name == new_name) return true;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        auto it = m_configs.find(old_name);
        if (it == m_configs.end()) {
            out_error = "Configuration '" + old_name + "' not found.";
            return false;
        }
        if (m_configs.find(new_name) != m_configs.end()) {
            out_error = "Configuration '" + new_name + "' already exists.";
            return false;
        }
        ConfigOverrides moved = std::move(it->second);
        m_configs.erase(it);
        m_configs[new_name] = std::move(moved);

        if (m_configuration == old_name) {
            m_configuration = new_name;
        }
        for (auto& t : m_targets) {
            auto tcit = t.configs.find(old_name);
            if (tcit != t.configs.end()) {
                SettingsBlock sb = std::move(tcit->second);
                t.configs.erase(tcit);
                t.configs[new_name] = std::move(sb);
            }
            for (auto& ov : t.overrides) {
                auto ocit = ov.configs.find(old_name);
                if (ocit != ov.configs.end()) {
                    OverrideConfigSettings ocs = std::move(ocit->second);
                    ov.configs.erase(ocit);
                    ov.configs[new_name] = std::move(ocs);
                }
            }
        }
    }
    m_revision++;
    Save();
    return true;
}

bool Project::DeleteConfiguration(const std::string& name, std::string& out_error) {
    out_error.clear();
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        if (m_configs.size() <= 1) {
            out_error = "Cannot delete the last remaining configuration.";
            return false;
        }
        if (m_configuration == name) {
            out_error = "Cannot delete the active configuration. Switch to another configuration first.";
            return false;
        }
        auto it = m_configs.find(name);
        if (it == m_configs.end()) {
            out_error = "Configuration '" + name + "' not found.";
            return false;
        }
        m_configs.erase(it);
        for (auto& t : m_targets) {
            t.configs.erase(name);
            for (auto& ov : t.overrides) {
                ov.configs.erase(name);
            }
        }
    }
    m_revision++;
    Save();
    return true;
}

std::filesystem::path Project::ResolvePath(const std::string& path_str) const {
    std::filesystem::path p(path_str);
    if (p.is_absolute()) {
        return p;
    }
    return m_root_path / p;
}

std::filesystem::path Project::GetObjectDir(const std::string& config) const {
    std::string cfg = config.empty() ? m_configuration : config;
    const Target* primary = GetPrimaryTarget();
    if (primary && !primary->name.empty()) {
        return primary->GetObjectDir(m_root_path, cfg);
    }
    return m_root_path / ".geniebuild" / "obj" / cfg;
}

std::filesystem::path Project::GetBinaryPath(const std::string& config) const {
    std::string cfg = config.empty() ? m_configuration : config;
    std::string comp_low = ToLower(m_compiler);
    bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
    const Target* primary = GetPrimaryTarget();
    if (primary && primary->kind != TargetKind::Interface) {
        return primary->GetBinaryPath(GetAbsoluteOutputDir(), cfg, is_msvc);
    }
    std::string bin_name = m_root_path.filename().generic_u8string();
    if (bin_name.empty()) {
        bin_name = "app";
    }
#if defined(_WIN32)
    bin_name += ".exe";
#endif
    return GetAbsoluteOutputDir() / cfg / "bin" / bin_name;
}

void Project::SetOutputDir(const std::string& output_dir) {
    if (m_output_dir != output_dir) {
        m_output_dir = output_dir;
        Save();
    }
}

std::filesystem::path Project::GetAbsoluteOutputDir() const {
    std::filesystem::path p(m_output_dir);
    if (p.is_relative()) {
        return m_root_path / p;
    }
    return p;
}

void Project::SetOutputDirPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path abs_path = std::filesystem::absolute(path, ec);
    if (ec) {
        abs_path = path;
    }

    // Check if inside project root
    std::string abs_str = abs_path.generic_string();
    std::string root_str = m_root_path.generic_string();

    if (abs_str.rfind(root_str, 0) == 0) {
        std::filesystem::path rel = std::filesystem::relative(abs_path, m_root_path, ec);
        if (!ec) {
            SetOutputDir(NormalizePath(rel.generic_string()));
            return;
        }
    }
    SetOutputDir(NormalizePath(abs_path.generic_string()));
}

void Project::RefreshMissingFilesCache() {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_missing_files_set.clear();
    m_missing_files.clear();

    size_t total_files = 0;
    for (const auto& t : m_targets) {
        total_files += t.expandedFiles.size();
    }

    if (total_files > 64) {
        std::vector<std::string> unique_abs;
        unique_abs.reserve(total_files);
        std::unordered_map<std::string, std::string> rel_to_norm;
        rel_to_norm.reserve(total_files);
        for (const auto& t : m_targets) {
            for (const auto& file : t.expandedFiles) {
                if (rel_to_norm.find(file) == rel_to_norm.end()) {
                    std::string abs_s = ResolvePath(file).generic_u8string();
                    rel_to_norm.emplace(file, DepLog::NormalizeDepPath(abs_s));
                    unique_abs.push_back(std::move(abs_s));
                }
            }
        }
        auto stat_map = DepLog::BatchStatFiles(unique_abs, 0);
        std::unordered_set<std::string> checked;
        for (auto& t : m_targets) {
            t.missingFilesSet.clear();
            t.missingFiles.clear();
            for (const auto& file : t.expandedFiles) {
                const std::string& norm = rel_to_norm[file];
                if (stat_map.find(norm) == stat_map.end()) {
                    t.missingFilesSet.insert(file);
                    t.missingFiles.push_back(file);
                    if (checked.insert(file).second) {
                        m_missing_files_set.insert(file);
                        m_missing_files.push_back(file);
                    }
                }
            }
        }
        return;
    }

    std::unordered_set<std::string> checked;
    for (auto& t : m_targets) {
        t.missingFilesSet.clear();
        t.missingFiles.clear();
        for (const auto& file : t.expandedFiles) {
            const std::filesystem::path full = ResolvePath(file);
            if (!fs_util::ExistsLong(full)) {
                t.missingFilesSet.insert(file);
                t.missingFiles.push_back(file);
                if (checked.insert(file).second) {
                    m_missing_files_set.insert(file);
                    m_missing_files.push_back(file);
                }
            }
        }
    }
}

bool Project::IsFileMissingCached(const std::string& relative_path) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_missing_files_set.find(relative_path) != m_missing_files_set.end();
}

bool Project::FileExistsOnDisk(const std::string& relative_path) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_missing_files_set.find(relative_path) == m_missing_files_set.end();
}

std::vector<std::string> Project::GetFilesCopy() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_files;
}

std::unordered_set<std::string> Project::GetFilesSetCopy() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_files_set;
}

void Project::RebuildIndexMap() {
    m_file_indices.clear();
    m_file_indices.reserve(m_files.size());
    for (size_t i = 0; i < m_files.size(); ++i) {
        m_file_indices[m_files[i]] = i;
    }
}

static void ReadStringArray(const nlohmann::json& json_obj, const char* key, std::vector<std::string>& out) {
    out.clear();
    if (json_obj.contains(key) && json_obj[key].is_array()) {
        for (const auto& item : json_obj[key]) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
}

static void ReadLibsWithConditions(
    const nlohmann::json& json_obj,
    const char* key,
    std::vector<std::string>& out_libs,
    std::map<std::string, std::string>& out_conds
) {
    out_libs.clear();
    out_conds.clear();
    if (json_obj.contains(key) && json_obj[key].is_array()) {
        for (const auto& item : json_obj[key]) {
            if (item.is_string()) {
                out_libs.push_back(item.get<std::string>());
            } else if (item.is_object()) {
                std::string name;
                if (item.contains("name") && item["name"].is_string()) {
                    name = item["name"].get<std::string>();
                } else if (item.contains("lib") && item["lib"].is_string()) {
                    name = item["lib"].get<std::string>();
                }
                if (!name.empty()) {
                    out_libs.push_back(name);
                    if (item.contains("if") && item["if"].is_string()) {
                        std::string cond = item["if"].get<std::string>();
                        if (!cond.empty()) {
                            out_conds[name] = cond;
                        }
                    }
                }
            }
        }
    }
}

static nlohmann::json WriteLibsWithConditions(
    const std::vector<std::string>& libs,
    const std::map<std::string, std::string>& conds
) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& lib : libs) {
        auto it = conds.find(lib);
        if (it != conds.end() && !it->second.empty()) {
            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = lib;
            entry["if"] = it->second;
            arr.push_back(std::move(entry));
        } else {
            arr.push_back(lib);
        }
    }
    return arr;
}

static void ReadSettingsBlock(const nlohmann::json& obj, SettingsBlock& blk) {
    if (!obj.is_object()) return;
    ReadStringArray(obj, "includeDirs", blk.includeDirs);
    ReadStringArray(obj, "systemIncludeDirs", blk.systemIncludeDirs);
    ReadStringArray(obj, "defines", blk.defines);
    ReadStringArray(obj, "compileFlags", blk.compileFlags);
    ReadStringArray(obj, "linkFlags", blk.linkFlags);
    ReadStringArray(obj, "libDirs", blk.libDirs);
    ReadLibsWithConditions(obj, "libs", blk.libs, blk.libConditions);
    ReadStringArray(obj, "copyDlls", blk.copyDlls);
    if (obj.contains("cxxStd") && obj["cxxStd"].is_string()) blk.cxxStd = obj["cxxStd"].get<std::string>();
    if (obj.contains("cStd") && obj["cStd"].is_string()) blk.cStd = obj["cStd"].get<std::string>();
    if (obj.contains("warnings") && obj["warnings"].is_string()) blk.warnings = obj["warnings"].get<std::string>();
    if (obj.contains("runtime") && obj["runtime"].is_string()) blk.runtime = obj["runtime"].get<std::string>();
}

static nlohmann::json WriteSettingsBlock(const SettingsBlock& blk) {
    nlohmann::json j = nlohmann::json::object();
    if (!blk.includeDirs.empty()) j["includeDirs"] = blk.includeDirs;
    if (!blk.systemIncludeDirs.empty()) j["systemIncludeDirs"] = blk.systemIncludeDirs;
    if (!blk.defines.empty()) j["defines"] = blk.defines;
    if (!blk.compileFlags.empty()) j["compileFlags"] = blk.compileFlags;
    if (!blk.linkFlags.empty()) j["linkFlags"] = blk.linkFlags;
    if (!blk.libDirs.empty()) j["libDirs"] = blk.libDirs;
    if (!blk.libs.empty()) j["libs"] = WriteLibsWithConditions(blk.libs, blk.libConditions);
    if (!blk.copyDlls.empty()) j["copyDlls"] = blk.copyDlls;
    if (!blk.cxxStd.empty()) j["cxxStd"] = blk.cxxStd;
    if (!blk.cStd.empty()) j["cStd"] = blk.cStd;
    if (!blk.warnings.empty()) j["warnings"] = blk.warnings;
    if (!blk.runtime.empty()) j["runtime"] = blk.runtime;
    return j;
}

static void ReadOverrideConfigSettings(const nlohmann::json& obj, OverrideConfigSettings& oc) {
    if (!obj.is_object()) return;
    ReadStringArray(obj, "defines", oc.defines);
    ReadStringArray(obj, "compileFlags", oc.compileFlags);
    ReadStringArray(obj, "linkFlags", oc.linkFlags);
    if (obj.contains("optimize") && obj["optimize"].is_string()) oc.optimize = obj["optimize"].get<std::string>();
    if (obj.contains("debugInfo") && obj["debugInfo"].is_string()) oc.debugInfo = obj["debugInfo"].get<std::string>();
    if (obj.contains("warnings") && obj["warnings"].is_string()) oc.warnings = obj["warnings"].get<std::string>();
    if (obj.contains("cxxStd") && obj["cxxStd"].is_string()) oc.cxxStd = obj["cxxStd"].get<std::string>();
    if (obj.contains("cStd") && obj["cStd"].is_string()) oc.cStd = obj["cStd"].get<std::string>();
}

static nlohmann::json WriteOverrideConfigSettings(const OverrideConfigSettings& oc) {
    nlohmann::json j = nlohmann::json::object();
    if (!oc.optimize.empty()) j["optimize"] = oc.optimize;
    if (!oc.debugInfo.empty()) j["debugInfo"] = oc.debugInfo;
    if (!oc.warnings.empty()) j["warnings"] = oc.warnings;
    if (!oc.cxxStd.empty()) j["cxxStd"] = oc.cxxStd;
    if (!oc.cStd.empty()) j["cStd"] = oc.cStd;
    if (!oc.defines.empty()) j["defines"] = oc.defines;
    if (!oc.compileFlags.empty()) j["compileFlags"] = oc.compileFlags;
    if (!oc.linkFlags.empty()) j["linkFlags"] = oc.linkFlags;
    return j;
}

Target* Project::FindTargetLocked(const std::string& name) {
    for (auto& t : m_targets) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

const Target* Project::FindTargetLocked(const std::string& name) const {
    for (const auto& t : m_targets) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

Target& Project::EnsureDefaultTargetLocked() {
    if (!m_targets.empty()) {
        if (!m_startup_target.empty()) {
            if (Target* st = FindTargetLocked(m_startup_target)) {
                return *st;
            }
        }
        return m_targets[0];
    }
    Target t;
    t.name = m_root_path.filename().generic_u8string();
    if (t.name.empty()) t.name = "app";
    t.kind = TargetKind::Executable;
    m_targets.push_back(std::move(t));
    m_startup_target = m_targets[0].name;
    return m_targets[0];
}

const Target* Project::FindTarget(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return FindTargetLocked(name);
}

const Target* Project::GetPrimaryTarget() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (m_targets.empty()) return nullptr;
    if (!m_startup_target.empty()) {
        if (const Target* st = FindTargetLocked(m_startup_target)) {
            return st;
        }
    }
    return &m_targets[0];
}

std::vector<Target> Project::GetTargetsSnapshot() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_targets;
}

void Project::SetStartupTarget(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        if (m_startup_target == name) return;
        if (!FindTargetLocked(name)) return;
        m_startup_target = name;
        SyncTargetsLocked();
    }
    m_revision++;
    Save();
}

bool Project::IsMultiTargetOrRulesUI() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (m_targets.size() > 1) return true;
    for (const auto& t : m_targets) {
        if (t.HasV2Fields()) return true;
    }
    return false;
}

void Project::SyncTargetsLocked() {
    m_files.clear();
    m_files_set.clear();
    m_disabled_files.clear();
    m_file_to_targets.clear();

    for (const auto& t : m_targets) {
        for (const auto& f : t.expandedFiles) {
            m_file_to_targets[f].push_back(t.name);
            if (m_files_set.insert(f).second) {
                m_files.push_back(f);
            }
        }
    }

    if (!m_targets.empty()) {
        const Target* primary = nullptr;
        if (!m_startup_target.empty()) primary = FindTargetLocked(m_startup_target);
        if (!primary) primary = &m_targets[0];
        for (const auto& d : primary->disabledFiles) {
            m_disabled_files.insert(d);
        }
    }

    RebuildIndexMap();
}

void Project::ExpandAllSourceRules(bool mark_dirty_dirs) {
    if (m_root_path.empty()) return;
    if (mark_dirty_dirs) {
        m_dir_cache.MarkAllDirty();
    }

    std::vector<TargetRuleInput> inputs;
    std::filesystem::path root_copy;
    ConditionContext cond_ctx = BuildConditionContext(*this);
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        root_copy = m_root_path;

        inputs.reserve(m_targets.size());
        for (const auto& t : m_targets) {
            TargetRuleInput in;
            in.name = t.name;
            in.rules = t.sources;
            in.explicit_files = t.files;
            in.target_excludes = t.exclude;
            in.target_condition = t.condition;
            in.condition_ctx = cond_ctx;
            in.condition_os = cond_ctx.os;
            in.condition_compiler = cond_ctx.compilerFamily;
            for (const auto& ct : m_custom_tools) {
                if (!ct.ext.empty()) in.extra_extensions.push_back(ct.ext);
            }
            inputs.push_back(std::move(in));
        }
    }

    RuleExpansionResult res = ExpandRules(root_copy, inputs, m_dir_cache);

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        for (auto& t : m_targets) {
            auto it = res.per_target.find(t.name);
            if (it != res.per_target.end()) {
                t.expandedFiles = std::move(it->second.source_files);
                t.expandedFilesSet.clear();
                t.expandedFilesSet.insert(t.expandedFiles.begin(), t.expandedFiles.end());
                t.headerFiles = std::move(it->second.header_files);
                t.ruleMatchCounts = std::move(it->second.rule_match_counts);
            } else {
                t.expandedFiles = t.files;
                std::sort(t.expandedFiles.begin(), t.expandedFiles.end());
                t.expandedFilesSet.clear();
                t.expandedFilesSet.insert(t.expandedFiles.begin(), t.expandedFiles.end());
                t.headerFiles.clear();
                t.ruleMatchCounts.assign(t.sources.size(), 0);
            }
        }
        m_multi_target_files = std::move(res.multi_target_files);
        SyncTargetsLocked();
    }

    RefreshMissingFilesCache();
    m_revision++;
}

std::vector<std::string> Project::GetTargetsOwningFile(const std::string& rel_path) const {
    std::string norm = NormalizePath(rel_path);
    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto it = m_file_to_targets.find(norm);
    if (it != m_file_to_targets.end()) {
        return it->second;
    }
    return {};
}

std::unordered_map<std::string, std::vector<std::string>> Project::GetMultiTargetFilesCopy() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_multi_target_files;
}

bool Project::AddTarget(const Target& target, std::string& out_error) {
    out_error.clear();
    if (target.name.empty()) {
        out_error = "Target name cannot be empty.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        if (FindTargetLocked(target.name)) {
            out_error = "A target named '" + target.name + "' already exists.";
            return false;
        }
        m_targets.push_back(target);
        if (m_startup_target.empty() && target.kind == TargetKind::Executable) {
            m_startup_target = target.name;
        }
    }
    ExpandAllSourceRules(false);
    MarkDirty();
    return true;
}

bool Project::SetTargetsBatch(
    const std::vector<Target>& targets,
    const std::string& startup_target,
    std::string& out_error,
    bool expand_rules
) {
    out_error.clear();
    if (targets.empty()) {
        out_error = "Target list cannot be empty.";
        return false;
    }
    std::unordered_set<std::string> seen_names;
    for (const auto& t : targets) {
        if (t.name.empty()) {
            out_error = "Target name cannot be empty.";
            return false;
        }
        if (!seen_names.insert(t.name).second) {
            out_error = "Duplicate target named '" + t.name + "'.";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_targets = targets;
        if (!startup_target.empty() && seen_names.count(startup_target)) {
            m_startup_target = startup_target;
        } else {
            m_startup_target.clear();
            for (const auto& t : m_targets) {
                if (t.kind == TargetKind::Executable) {
                    m_startup_target = t.name;
                    break;
                }
            }
            if (m_startup_target.empty()) {
                m_startup_target = m_targets[0].name;
            }
        }
        if (!expand_rules) {
            SyncTargetsLocked();
        }
    }
    if (expand_rules) {
        ExpandAllSourceRules(false);
    } else {
        m_revision++;
    }
    MarkDirty();
    return true;
}

bool Project::RemoveTarget(const std::string& name, std::string& out_error) {
    out_error.clear();
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        if (m_targets.size() <= 1) {
            out_error = "Cannot delete the last remaining target.";
            return false;
        }
        auto it = std::find_if(m_targets.begin(), m_targets.end(), [&](const Target& t) { return t.name == name; });
        if (it == m_targets.end()) {
            out_error = "Target '" + name + "' not found.";
            return false;
        }
        m_targets.erase(it);

        // Remove references from other targets' deps/publicDeps
        for (auto& t : m_targets) {
            t.deps.erase(std::remove(t.deps.begin(), t.deps.end(), name), t.deps.end());
            t.publicDeps.erase(std::remove(t.publicDeps.begin(), t.publicDeps.end(), name), t.publicDeps.end());
        }
        if (m_startup_target == name) {
            m_startup_target.clear();
            for (const auto& t : m_targets) {
                if (t.kind == TargetKind::Executable) {
                    m_startup_target = t.name;
                    break;
                }
            }
            if (m_startup_target.empty() && !m_targets.empty()) {
                m_startup_target = m_targets[0].name;
            }
        }
    }
    ExpandAllSourceRules(false);
    MarkDirty();
    return true;
}

bool Project::RenameTarget(const std::string& old_name, const std::string& new_name, std::string& out_error) {
    out_error.clear();
    if (new_name.empty()) {
        out_error = "Target name cannot be empty.";
        return false;
    }
    if (old_name == new_name) return true;

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* t = FindTargetLocked(old_name);
        if (!t) {
            out_error = "Target '" + old_name + "' not found.";
            return false;
        }
        if (FindTargetLocked(new_name)) {
            out_error = "A target named '" + new_name + "' already exists.";
            return false;
        }
        // Clean old outputs & object folder when renaming
        std::error_code ec;
        for (const auto& cfg_kv : m_configs) {
            bool is_msvc = (ToLower(m_compiler) == "cl" || ToLower(m_compiler) == "cl.exe");
            std::filesystem::remove_all(t->GetObjectDir(m_root_path, cfg_kv.first), ec);
            std::filesystem::path old_bin = t->GetBinaryPath(GetAbsoluteOutputDir(), cfg_kv.first, is_msvc);
            if (!old_bin.empty()) std::filesystem::remove(old_bin, ec);
            std::filesystem::path old_imp = t->GetImportLibPath(GetAbsoluteOutputDir(), cfg_kv.first, is_msvc);
            if (!old_imp.empty()) std::filesystem::remove(old_imp, ec);
        }

        t->name = new_name;
        for (auto& other : m_targets) {
            for (auto& d : other.deps) {
                if (d == old_name) d = new_name;
            }
            for (auto& pd : other.publicDeps) {
                if (pd == old_name) pd = new_name;
            }
        }
        if (m_startup_target == old_name) {
            m_startup_target = new_name;
        }
    }
    ExpandAllSourceRules(false);
    MarkDirty();
    return true;
}

bool Project::DuplicateTarget(const std::string& name, std::string& out_new_name) {
    Target copy;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        const Target* orig = FindTargetLocked(name);
        if (!orig) return false;
        copy = *orig;
        int suffix = 2;
        std::string cand = name + "_copy";
        while (FindTargetLocked(cand)) {
            cand = name + "_copy" + std::to_string(suffix++);
        }
        copy.name = cand;
        if (!copy.outputName.empty()) {
            copy.outputName = cand;
        }
        out_new_name = cand;
        m_targets.push_back(std::move(copy));
    }
    ExpandAllSourceRules(false);
    MarkDirty();
    return true;
}

bool Project::UpdateTarget(const Target& updated) {
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* existing = FindTargetLocked(updated.name);
        if (!existing) return false;
        *existing = updated;
    }
    ExpandAllSourceRules(false);
    MarkDirty();
    return true;
}

void Project::AddSourceRuleToTarget(const std::string& target_name, const SourceRule& rule) {
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* t = target_name.empty() ? &EnsureDefaultTargetLocked() : FindTargetLocked(target_name);
        if (!t) return;
        for (const auto& existing : t->sources) {
            if (existing.include == rule.include && existing.condition == rule.condition) {
                return;
            }
        }
        t->sources.push_back(rule);
    }
    ExpandAllSourceRules(false);
    MarkDirty();
}

void Project::AddFilesToTarget(const std::string& target_name, const std::vector<std::string>& rel_paths) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* t = target_name.empty() ? &EnsureDefaultTargetLocked() : FindTargetLocked(target_name);
        if (!t) return;
        for (const auto& p : rel_paths) {
            std::string norm = NormalizePath(p);
            if (norm.empty()) continue;
            if (std::find(t->files.begin(), t->files.end(), norm) == t->files.end()) {
                t->files.push_back(norm);
                changed = true;
            }
        }
    }
    if (changed) {
        ExpandAllSourceRules(false);
        MarkDirty();
    }
}

void Project::RemoveFileFromTarget(const std::string& target_name, const std::string& rel_path) {
    RemoveFilesFromTarget(target_name, { rel_path });
}

void Project::RemoveFilesFromTarget(const std::string& target_name, const std::vector<std::string>& rel_paths) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* t = target_name.empty() ? &EnsureDefaultTargetLocked() : FindTargetLocked(target_name);
        if (!t) return;
        for (const auto& p : rel_paths) {
            std::string norm = NormalizePath(p);
            if (norm.empty()) continue;
            auto it = std::find(t->files.begin(), t->files.end(), norm);
            if (it != t->files.end()) {
                t->files.erase(it);
                t->disabledFiles.erase(norm);
                changed = true;
            } else if (t->expandedFilesSet.find(norm) != t->expandedFilesSet.end()) {
                // Came from a glob rule: add to target-level exclude
                if (std::find(t->exclude.begin(), t->exclude.end(), norm) == t->exclude.end()) {
                    t->exclude.push_back(norm);
                    changed = true;
                }
                t->disabledFiles.erase(norm);
            }
        }
    }
    if (changed) {
        ExpandAllSourceRules(false);
        MarkDirty();
    }
}

void Project::SetTargetFileEnabled(const std::string& target_name, const std::string& rel_path, bool enabled) {
    std::string norm = NormalizePath(rel_path);
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* t = target_name.empty() ? &EnsureDefaultTargetLocked() : FindTargetLocked(target_name);
        if (!t || t->expandedFilesSet.find(norm) == t->expandedFilesSet.end()) return;
        if (enabled) t->disabledFiles.erase(norm);
        else t->disabledFiles.insert(norm);
        SyncTargetsLocked();
    }
    m_revision++;
    Save();
}

void Project::SetTargetFolderEnabled(const std::string& target_name, const std::string& folder_rel_path, bool enabled) {
    std::string prefix = NormalizePath(folder_rel_path);
    if (!prefix.empty() && prefix.back() != '/') {
        prefix += "/";
    }
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target* t = target_name.empty() ? &EnsureDefaultTargetLocked() : FindTargetLocked(target_name);
        if (!t) return;
        for (const auto& file : t->expandedFiles) {
            if (prefix.empty() || file.rfind(prefix, 0) == 0) {
                if (enabled) t->disabledFiles.erase(file);
                else t->disabledFiles.insert(file);
            }
        }
        SyncTargetsLocked();
    }
    m_revision++;
    Save();
}

bool Project::Load(const std::filesystem::path& root_path) {
    if (m_save_dirty) {
        SaveImmediate();
        m_save_dirty = false;
    }

    m_file_watcher.Stop();
    m_dir_cache.Clear();

    // Abort and join any previous active scan thread
    m_abort_scan.store(true);
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    m_abort_scan.store(false);

    std::error_code ec;
    if (root_path.empty()) {
        m_root_path.clear();
        m_is_build_output_folder = false;
        m_schema_version = 1;
        m_loaded_as_v1 = false;
        {
            std::lock_guard<std::mutex> lock(m_data_mutex);
            m_targets.clear();
            m_startup_target.clear();
            m_file_to_targets.clear();
            m_multi_target_files.clear();
            m_files.clear();
            m_files_set.clear();
            m_file_indices.clear();
            m_disabled_files.clear();
            m_missing_files_set.clear();
            m_missing_files.clear();
            m_pane_widths.clear();
            m_output_dir = ".geniebuild/out";
        }
        m_is_scanning.store(false);
        m_revision++;
        return true;
    }

    m_root_path = std::filesystem::absolute(root_path, ec);
    if (ec) {
        m_root_path = root_path;
    }
    m_is_build_output_folder = IsBuildOutputFolder(m_root_path);

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_schema_version = 1;
        m_loaded_as_v1 = false;
        m_targets.clear();
        m_startup_target.clear();
        m_file_to_targets.clear();
        m_multi_target_files.clear();
        m_files.clear();
        m_files_set.clear();
        m_disabled_files.clear();
        m_missing_files_set.clear();
        m_missing_files.clear();
        m_pane_widths.clear();
        m_compiler = "g++";
        m_c_compiler = "gcc";
        m_configuration = "Debug";
        m_cxx_std = "c++17";
        m_c_std = "c11";
        m_jobs = 0;
        m_warnings = "-Wall";
        m_include_dirs.clear();
        m_lib_dirs.clear();
        m_libs.clear();
        m_lib_conditions.clear();
        m_defines.clear();
        m_copy_dlls.clear();
        m_compile_flags.clear();
        m_link_flags.clear();
        m_configs.clear();
        {
            ConfigOverrides d; d.base = "debug"; d.defines = {"_DEBUG"};
            ConfigOverrides r; r.base = "release"; r.defines = {"NDEBUG"};
            m_configs["Debug"] = std::move(d);
            m_configs["Release"] = std::move(r);
        }
        m_schema_warnings.clear();
        m_tool_overrides.clear();
        m_custom_tools.clear();
        m_asm_mode = "auto";
        m_toolchain.clear();
        m_project_toolchains.clear();
        m_compile_commands_cfg = CompileCommandsConfig{};
        m_ignore_system_headers = true;
        m_keep_dep_files = false;
        m_linker = "default";
        m_incremental_link = true;
        m_debug_link = "full";
        m_split_dwarf = false;
        m_mem_per_job_mb = 2048;
        m_link_jobs = 1;
        m_keep_going = false;
        m_low_priority = true;
        m_output_dir = ".geniebuild/out";
    }

    const std::filesystem::path project_file = m_root_path / ".geniebuild" / "project.json";

    // If project.json exists, restore it synchronously
    if (fs_util::ExistsLong(project_file)) {
        try {
            std::ifstream file(fs_util::ToWinLongPath(project_file));
            if (file.is_open()) {
                nlohmann::json j;
                file >> j;

                std::lock_guard<std::mutex> lock(m_data_mutex);

                if (j.contains("version") && j["version"].is_number_integer()) {
                    m_schema_version = j["version"].get<int>();
                } else {
                    m_schema_version = 1;
                }

                // Load toolchain & compiler choices (V2-14)
                if (j.contains("toolchain") && j["toolchain"].is_string()) {
                    m_toolchain = j["toolchain"].get<std::string>();
                }
                if (j.contains("compileCommands") && j["compileCommands"].is_object()) {
                    const auto& ccj = j["compileCommands"];
                    if (ccj.contains("enabled") && ccj["enabled"].is_boolean()) {
                        m_compile_commands_cfg.enabled = ccj["enabled"].get<bool>();
                    }
                    if (ccj.contains("path") && ccj["path"].is_string()) {
                        m_compile_commands_cfg.path = ccj["path"].get<std::string>();
                    }
                    if (ccj.contains("allConfigs") && ccj["allConfigs"].is_boolean()) {
                        m_compile_commands_cfg.allConfigs = ccj["allConfigs"].get<bool>();
                    }
                }
                if (j.contains("toolchains") && j["toolchains"].is_array()) {
                    for (const auto& tcj : j["toolchains"]) {
                        ToolchainProfile tp = ToolchainManager::ProfileFromJson(tcj, true);
                        if (!tp.name.empty()) m_project_toolchains.push_back(std::move(tp));
                    }
                }
                if (j.contains("compiler") && j["compiler"].is_string()) {
                    m_compiler = j["compiler"].get<std::string>();
                }

                // Paired C compiler choice (Spec §5.1.6)
                if (j.contains("cCompiler") && j["cCompiler"].is_string()) {
                    m_c_compiler = j["cCompiler"].get<std::string>();
                } else {
                    if (m_compiler.find("clang") != std::string::npos) m_c_compiler = "clang";
                    else if (m_compiler == "cl") m_c_compiler = "cl";
                    else m_c_compiler = "gcc";
                }

                // Active configuration (Debug / Release) (Spec §5.1.4)
                if (j.contains("configuration") && j["configuration"].is_string()) {
                    m_configuration = j["configuration"].get<std::string>();
                }

                // Language standards (Spec §5.1.3)
                if (j.contains("cxxStd") && j["cxxStd"].is_string()) {
                    m_cxx_std = j["cxxStd"].get<std::string>();
                }
                if (j.contains("cStd") && j["cStd"].is_string()) {
                    m_c_std = j["cStd"].get<std::string>();
                }

                // Compile jobs (0 = auto-detect cores) (Spec §5.1.5)
                if (j.contains("jobs") && j["jobs"].is_number()) {
                    m_jobs = j["jobs"].get<int>();
                }

                // V2-7 / V2-11 / V2-12 / V2-13 root fields
                if (j.contains("toolOverrides") && j["toolOverrides"].is_object()) {
                    for (auto it = j["toolOverrides"].begin(); it != j["toolOverrides"].end(); ++it) {
                        if (it.value().is_string()) m_tool_overrides[it.key()] = it.value().get<std::string>();
                    }
                }
                if (j.contains("asm") && j["asm"].is_string()) m_asm_mode = j["asm"].get<std::string>();
                if (j.contains("customTools") && j["customTools"].is_array()) {
                    for (const auto& ctj : j["customTools"]) {
                        if (!ctj.is_object()) continue;
                        CustomToolSpec ct;
                        if (ctj.contains("ext") && ctj["ext"].is_string()) ct.ext = ctj["ext"].get<std::string>();
                        if (ctj.contains("program") && ctj["program"].is_string()) ct.program = ctj["program"].get<std::string>();
                        if (ctj.contains("args") && ctj["args"].is_string()) ct.args = ctj["args"].get<std::string>();
                        if (ctj.contains("output") && ctj["output"].is_string()) ct.output = ctj["output"].get<std::string>();
                        if (ctj.contains("link") && ctj["link"].is_boolean()) ct.link = ctj["link"].get<bool>();
                        if (!ct.ext.empty()) m_custom_tools.push_back(std::move(ct));
                    }
                }
                if (j.contains("ignoreSystemHeaders") && j["ignoreSystemHeaders"].is_boolean()) {
                    m_ignore_system_headers = j["ignoreSystemHeaders"].get<bool>();
                }
                if (j.contains("keepDepFiles") && j["keepDepFiles"].is_boolean()) {
                    m_keep_dep_files = j["keepDepFiles"].get<bool>();
                }
                if (j.contains("linker") && j["linker"].is_string()) m_linker = j["linker"].get<std::string>();
                if (j.contains("incrementalLink") && j["incrementalLink"].is_boolean()) m_incremental_link = j["incrementalLink"].get<bool>();
                if (j.contains("debugLink") && j["debugLink"].is_string()) m_debug_link = j["debugLink"].get<std::string>();
                if (j.contains("splitDwarf") && j["splitDwarf"].is_boolean()) m_split_dwarf = j["splitDwarf"].get<bool>();
                if (j.contains("memPerJob")) {
                    if (j["memPerJob"].is_number_integer()) {
                        m_mem_per_job_mb = std::max(128, j["memPerJob"].get<int>());
                    } else if (j["memPerJob"].is_string()) {
                        std::string ms = ToLower(j["memPerJob"].get<std::string>());
                        try {
                            if (!ms.empty() && ms.back() == 'g') m_mem_per_job_mb = std::max(128, static_cast<int>(std::stod(ms.substr(0, ms.size() - 1)) * 1024.0));
                            else if (!ms.empty() && ms.back() == 'm') m_mem_per_job_mb = std::max(128, std::stoi(ms.substr(0, ms.size() - 1)));
                            else m_mem_per_job_mb = std::max(128, std::stoi(ms));
                        } catch (...) {}
                    }
                }
                if (j.contains("linkJobs") && j["linkJobs"].is_number_integer()) {
                    m_link_jobs = std::max(1, j["linkJobs"].get<int>());
                }
                if (j.contains("keepGoing") && j["keepGoing"].is_boolean()) m_keep_going = j["keepGoing"].get<bool>();
                if (j.contains("lowPriority") && j["lowPriority"].is_boolean()) m_low_priority = j["lowPriority"].get<bool>();

                // Warnings
                if (j.contains("warnings") && j["warnings"].is_string()) {
                    m_warnings = j["warnings"].get<std::string>();
                }

                ReadStringArray(j, "includeDirs", m_include_dirs);
                ReadStringArray(j, "libDirs", m_lib_dirs);
                ReadLibsWithConditions(j, "libs", m_libs, m_lib_conditions);
                ReadStringArray(j, "defines", m_defines);
                ReadStringArray(j, "copyDlls", m_copy_dlls);
                ReadStringArray(j, "compileFlags", m_compile_flags);
                ReadStringArray(j, "linkFlags", m_link_flags);

                if (j.contains("configs") && j["configs"].is_object() && !j["configs"].empty()) {
                    m_configs.clear();
                    for (auto it = j["configs"].begin(); it != j["configs"].end(); ++it) {
                        ConfigOverrides ov;
                        if (it.value().is_object()) {
                            const auto& cobj = it.value();
                            if (cobj.contains("base") && cobj["base"].is_string()) ov.base = cobj["base"].get<std::string>();
                            if (cobj.contains("optimize") && cobj["optimize"].is_string()) ov.optimize = cobj["optimize"].get<std::string>();
                            if (cobj.contains("debugInfo") && cobj["debugInfo"].is_string()) ov.debugInfo = cobj["debugInfo"].get<std::string>();
                            if (cobj.contains("runtime") && cobj["runtime"].is_string()) ov.runtime = cobj["runtime"].get<std::string>();
                            if (cobj.contains("lto") && cobj["lto"].is_string()) ov.lto = cobj["lto"].get<std::string>();
                            ReadStringArray(cobj, "defines", ov.defines);
                            ReadStringArray(cobj, "compileFlags", ov.compileFlags);
                            ReadStringArray(cobj, "linkFlags", ov.linkFlags);
                        }
                        if (ov.base.empty()) {
                            ov.base = BaseConfigPresetToString(BaseConfigPresetFromString("", it.key()));
                        }
                        m_configs[it.key()] = std::move(ov);
                    }
                }

                // Huge-list warning confirmation setting (Spec §3.7)
                if (j.contains("confirmLargeBuild") && j["confirmLargeBuild"].is_boolean()) {
                    m_confirm_large_build = j["confirmLargeBuild"].get<bool>();
                } else {
                    m_confirm_large_build = true;
                }

                // Load outputDir (defaults to ".geniebuild/out")
                if (j.contains("outputDir") && j["outputDir"].is_string()) {
                    m_output_dir = j["outputDir"].get<std::string>();
                } else {
                    m_output_dir = ".geniebuild/out";
                }

                // Part 3 Item 6: Load optional pane widths
                m_pane_widths.clear();
                if (j.contains("paneWidths") && j["paneWidths"].is_array()) {
                    for (const auto& w : j["paneWidths"]) {
                        if (w.is_number()) {
                            m_pane_widths.push_back(w.get<float>());
                        }
                    }
                }

                if (j.contains("startupTarget") && j["startupTarget"].is_string()) {
                    m_startup_target = j["startupTarget"].get<std::string>();
                }

                // Load v2 targets array, or migrate from v1 (§2.4)
                if (j.contains("targets") && j["targets"].is_array() && !j["targets"].empty()) {
                    for (const auto& tj : j["targets"]) {
                        if (!tj.is_object()) continue;
                        Target t;
                        if (tj.contains("name") && tj["name"].is_string()) {
                            t.name = tj["name"].get<std::string>();
                        }
                        if (t.name.empty()) continue;
                        if (tj.contains("kind") && tj["kind"].is_string()) {
                            t.kind = TargetKindFromString(tj["kind"].get<std::string>());
                        }
                        if (tj.contains("outputName") && tj["outputName"].is_string()) {
                            t.outputName = tj["outputName"].get<std::string>();
                        }
                        if (tj.contains("if") && tj["if"].is_string()) {
                            t.condition = tj["if"].get<std::string>();
                        }
                        if (tj.contains("exportAll") && tj["exportAll"].is_boolean()) {
                            t.exportAll = tj["exportAll"].get<bool>();
                        }
                        if (tj.contains("freestanding") && tj["freestanding"].is_boolean()) {
                            t.freestanding = tj["freestanding"].get<bool>();
                        }
                        if (tj.contains("linkerScript") && tj["linkerScript"].is_string()) {
                            t.linkerScript = tj["linkerScript"].get<std::string>();
                        }
                        if (tj.contains("entry") && tj["entry"].is_string()) {
                            t.entry = tj["entry"].get<std::string>();
                        }
                        ReadStringArray(tj, "cxxFlags", t.cxxFlags);
                        if (tj.contains("linkLibgcc") && tj["linkLibgcc"].is_boolean()) {
                            t.linkLibgcc = tj["linkLibgcc"].get<bool>();
                        }
                        auto read_run_cfg = [&](const nlohmann::json& rj, RunConfig& rc) {
                            if (!rj.is_object()) return;
                            if (rj.contains("command") && rj["command"].is_string()) rc.command = rj["command"].get<std::string>();
                            ReadStringArray(rj, "args", rc.args);
                            if (rj.contains("cwd") && rj["cwd"].is_string()) rc.cwd = rj["cwd"].get<std::string>();
                            if (rj.contains("env") && rj["env"].is_object()) {
                                for (auto it = rj["env"].begin(); it != rj["env"].end(); ++it) {
                                    if (it.value().is_string()) rc.env[it.key()] = it.value().get<std::string>();
                                }
                            }
                            if (rj.contains("buildBeforeRun") && rj["buildBeforeRun"].is_boolean()) rc.buildBeforeRun = rj["buildBeforeRun"].get<bool>();
                        };
                        if (tj.contains("run")) read_run_cfg(tj["run"], t.run);
                        if (tj.contains("debug")) read_run_cfg(tj["debug"], t.debug);

                        if (tj.contains("sources") && tj["sources"].is_array()) {
                            for (const auto& sj : tj["sources"]) {
                                if (!sj.is_object()) continue;
                                SourceRule r;
                                if (sj.contains("include") && sj["include"].is_string()) {
                                    r.include = sj["include"].get<std::string>();
                                }
                                ReadStringArray(sj, "exclude", r.exclude);
                                if (sj.contains("if") && sj["if"].is_string()) {
                                    r.condition = sj["if"].get<std::string>();
                                }
                                if (sj.contains("followLinks") && sj["followLinks"].is_boolean()) {
                                    r.follow_links = sj["followLinks"].get<bool>();
                                }
                                if (!r.include.empty()) {
                                    t.sources.push_back(std::move(r));
                                }
                            }
                        }

                        std::vector<std::string> raw_files;
                        ReadStringArray(tj, "files", raw_files);
                        for (const auto& f : raw_files) {
                            std::string norm = NormalizePath(f);
                            if (!norm.empty() && std::find(t.files.begin(), t.files.end(), norm) == t.files.end()) {
                                t.files.push_back(norm);
                            }
                        }
                        ReadStringArray(tj, "exclude", t.exclude);
                        std::vector<std::string> disabled_vec;
                        ReadStringArray(tj, "disabledFiles", disabled_vec);
                        for (const auto& df : disabled_vec) {
                            t.disabledFiles.insert(NormalizePath(df));
                        }

                        ReadStringArray(tj, "deps", t.deps);
                        ReadStringArray(tj, "publicDeps", t.publicDeps);
                        if (tj.contains("public")) ReadSettingsBlock(tj["public"], t.pub);
                        if (tj.contains("private")) ReadSettingsBlock(tj["private"], t.priv);
                        if (tj.contains("configs") && tj["configs"].is_object()) {
                            for (auto it = tj["configs"].begin(); it != tj["configs"].end(); ++it) {
                                SettingsBlock sb;
                                ReadSettingsBlock(it.value(), sb);
                                t.configs[it.key()] = std::move(sb);
                            }
                        }
                        if (tj.contains("overrides") && tj["overrides"].is_array()) {
                            static const std::unordered_set<std::string> kKnownOvKeys = {
                                "match", "if", "includeDirs", "systemIncludeDirs", "defines",
                                "compileFlags", "linkFlags", "cxxStd", "cStd", "warnings",
                                "optimize", "debugInfo", "excludeFromPch", "excludeFromUnity", "configs"
                            };
                            for (const auto& oj : tj["overrides"]) {
                                if (!oj.is_object()) continue;
                                for (auto oit = oj.begin(); oit != oj.end(); ++oit) {
                                    if (kKnownOvKeys.find(oit.key()) == kKnownOvKeys.end()) {
                                        std::string sug = SuggestOverrideFieldName(oit.key());
                                        std::string msg = "Target '" + t.name + "' override has unknown field '" + oit.key() + "'";
                                        if (!sug.empty()) msg += " (did you mean '" + sug + "'?)";
                                        m_schema_warnings.push_back(std::move(msg));
                                    }
                                }
                                FlagOverride fov;
                                if (oj.contains("match") && oj["match"].is_string()) fov.match = oj["match"].get<std::string>();
                                if (oj.contains("if") && oj["if"].is_string()) fov.condition = oj["if"].get<std::string>();
                                ReadSettingsBlock(oj, fov.settings);
                                if (oj.contains("optimize") && oj["optimize"].is_string()) fov.optimize = oj["optimize"].get<std::string>();
                                if (oj.contains("debugInfo") && oj["debugInfo"].is_string()) fov.debugInfo = oj["debugInfo"].get<std::string>();
                                if (oj.contains("excludeFromPch") && oj["excludeFromPch"].is_boolean()) fov.excludeFromPch = oj["excludeFromPch"].get<bool>();
                                if (oj.contains("excludeFromUnity") && oj["excludeFromUnity"].is_boolean()) fov.excludeFromUnity = oj["excludeFromUnity"].get<bool>();
                                if (oj.contains("configs") && oj["configs"].is_object()) {
                                    for (auto cit = oj["configs"].begin(); cit != oj["configs"].end(); ++cit) {
                                        OverrideConfigSettings ocs;
                                        ReadOverrideConfigSettings(cit.value(), ocs);
                                        fov.configs[cit.key()] = std::move(ocs);
                                    }
                                }
                                t.overrides.push_back(std::move(fov));
                            }
                        }
                        if (tj.contains("steps") && tj["steps"].is_array()) {
                            for (const auto& stj : tj["steps"]) {
                                if (!stj.is_object()) continue;
                                BuildStep st;
                                if (stj.contains("name") && stj["name"].is_string()) st.name = stj["name"].get<std::string>();
                                if (stj.contains("phase") && stj["phase"].is_string()) st.phase = StepPhaseFromString(stj["phase"].get<std::string>());
                                if (stj.contains("foreach") && stj["foreach"].is_string()) st.foreachPattern = stj["foreach"].get<std::string>();
                                if (stj.contains("inputs")) {
                                    if (stj["inputs"].is_array()) ReadStringArray(stj, "inputs", st.inputs);
                                    else if (stj["inputs"].is_string()) st.inputs.push_back(stj["inputs"].get<std::string>());
                                }
                                if (stj.contains("command") && stj["command"].is_string()) st.command = stj["command"].get<std::string>();
                                if (stj.contains("outputs")) {
                                    if (stj["outputs"].is_array()) ReadStringArray(stj, "outputs", st.outputs);
                                    else if (stj["outputs"].is_string()) {
                                        std::string s = stj["outputs"].get<std::string>();
                                        if (!s.empty()) st.outputs.push_back(std::move(s));
                                    }
                                }
                                if (stj.contains("depfile") && stj["depfile"].is_string()) st.depfile = stj["depfile"].get<std::string>();
                                if (stj.contains("if") && stj["if"].is_string()) st.condition = stj["if"].get<std::string>();
                                if (stj.contains("generatesSources") && stj["generatesSources"].is_boolean()) st.generatesSources = stj["generatesSources"].get<bool>();
                                if (stj.contains("shell") && stj["shell"].is_boolean()) st.shell = stj["shell"].get<bool>();
                                if (stj.contains("always") && stj["always"].is_boolean()) st.always = stj["always"].get<bool>();
                                t.steps.push_back(std::move(st));
                            }
                        }
                        if (tj.contains("pch") && tj["pch"].is_object()) {
                            const auto& pj = tj["pch"];
                            if (pj.contains("header") && pj["header"].is_string()) t.pch.header = pj["header"].get<std::string>();
                            if (pj.contains("cHeader") && pj["cHeader"].is_string()) t.pch.cHeader = pj["cHeader"].get<std::string>();
                            if (pj.contains("source") && pj["source"].is_string()) t.pch.source = pj["source"].get<std::string>();
                            ReadStringArray(pj, "exclude", t.pch.exclude);
                        }
                        if (tj.contains("unity") && tj["unity"].is_object()) {
                            const auto& uj = tj["unity"];
                            if (uj.contains("enabled") && uj["enabled"].is_boolean()) t.unity.enabled = uj["enabled"].get<bool>();
                            if (uj.contains("batchSize") && uj["batchSize"].is_number_unsigned()) {
                                t.unity.batchSize = std::max<size_t>(1, uj["batchSize"].get<size_t>());
                            }
                            ReadStringArray(uj, "exclude", t.unity.exclude);
                            if (uj.contains("adaptive") && uj["adaptive"].is_boolean()) t.unity.adaptive = uj["adaptive"].get<bool>();
                        }
                        m_targets.push_back(std::move(t));
                    }
                    if (m_startup_target.empty() || !FindTargetLocked(m_startup_target)) {
                        for (const auto& t : m_targets) {
                            if (t.kind == TargetKind::Executable) {
                                m_startup_target = t.name;
                                break;
                            }
                        }
                        if (m_startup_target.empty() && !m_targets.empty()) {
                            m_startup_target = m_targets[0].name;
                        }
                    }
                } else {
                    // v1 migration (§2.4): synthesize single executable target
                    m_loaded_as_v1 = true;
                    Target t;
                    t.name = m_root_path.filename().generic_u8string();
                    if (t.name.empty()) t.name = "app";
                    t.kind = TargetKind::Executable;

                    if (j.contains("files") && j["files"].is_array()) {
                        for (const auto& item : j["files"]) {
                            if (item.is_string()) {
                                std::string normalized = NormalizePath(item.get<std::string>());
                                if (!normalized.empty() && std::find(t.files.begin(), t.files.end(), normalized) == t.files.end()) {
                                    t.files.push_back(normalized);
                                }
                            }
                        }
                    }
                    if (j.contains("disabledFiles") && j["disabledFiles"].is_array()) {
                        for (const auto& item : j["disabledFiles"]) {
                            if (item.is_string()) {
                                t.disabledFiles.insert(NormalizePath(item.get<std::string>()));
                            }
                        }
                    }
                    m_startup_target = t.name;
                    m_targets.push_back(std::move(t));
                }
            }
            ExpandAllSourceRules(false);
            RecentProjects::Add(m_root_path.generic_u8string());
            m_file_watcher.Start(m_root_path, &m_dir_cache);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not parse project.json (" << e.what() << "). Starting with empty settings.\n";
            std::lock_guard<std::mutex> lock(m_data_mutex);
            m_targets.clear();
            EnsureDefaultTargetLocked();
            SyncTargetsLocked();
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        EnsureDefaultTargetLocked();
        SyncTargetsLocked();
    }

    RecentProjects::Add(m_root_path.generic_u8string());
    m_file_watcher.Start(m_root_path, &m_dir_cache);

    // Spec §6.2 B2: If a folder contains CMakeCache.txt, build.ninja, or CMakeFiles, skip auto-fill scan
    if (m_is_build_output_folder) {
        std::cout << "[INFO] " << m_root_path.generic_u8string() << " is a build output directory. Skipping auto-fill scan.\n";
        m_revision++;
        return true;
    }

    // Part 1 Item 4: Run the initial auto-fill scan on a background thread with "Scanning..." state
    StartBackgroundScan();
    return true;
}

void Project::StartBackgroundScan() {
    m_is_scanning.store(true);
    m_scan_thread = std::thread([this, root = m_root_path]() {
        auto is_source_file = [](const std::filesystem::path& path) -> bool {
            const std::string ext = ToLower(path.extension().generic_u8string());
            return (ext == ".cpp" || ext == ".c" || ext == ".cc" || ext == ".cxx");
        };

        std::vector<std::string> collected_files;
        std::unordered_set<std::string> collected_set;

        std::error_code ec;
        if (std::filesystem::exists(root, ec) && std::filesystem::is_directory(root, ec)) {
            std::filesystem::recursive_directory_iterator it(
                root,
                std::filesystem::directory_options::skip_permission_denied,
                ec
            );
            std::filesystem::recursive_directory_iterator end;

            while (it != end && !ec) {
                if (m_abort_scan.load()) {
                    m_is_scanning.store(false);
                    return;
                }

                const auto& entry = *it;
                std::error_code entry_ec;
                const auto status = entry.status(entry_ec);

                if (std::filesystem::is_directory(status)) {
                    if (ShouldSkipDirectory(entry.path().filename().generic_u8string())) {
                        it.disable_recursion_pending();
                    }
                } else if (std::filesystem::is_regular_file(status)) {
                    if (is_source_file(entry.path())) {
                        std::filesystem::path rel = std::filesystem::relative(entry.path(), root, entry_ec);
                        if (!entry_ec) {
                            std::string rel_str = NormalizePath(rel.generic_u8string());
                            if (!rel_str.empty() && collected_set.find(rel_str) == collected_set.end()) {
                                collected_files.push_back(rel_str);
                                collected_set.insert(rel_str);
                            }
                        }
                    }
                }

                it.increment(ec);
            }
        }

        std::sort(collected_files.begin(), collected_files.end());

        // Thread safety (§2.8): Post result to pending scan queue for UI thread swap-in
        if (!m_abort_scan.load()) {
            auto pending = std::make_unique<PendingScanResult>();
            pending->files = std::move(collected_files);
            pending->files_set = std::move(collected_set);
            {
                std::lock_guard<std::mutex> lock(m_scan_mutex);
                m_pending_scan = std::move(pending);
            }
            m_has_pending_scan.store(true);
        }

        m_is_scanning.store(false);
    });
}

void Project::Update() {
    // 1. Thread safety (§2.8): Swap in completed background scan snapshot on UI thread
    if (m_has_pending_scan.load()) {
        std::unique_ptr<PendingScanResult> pending;
        {
            std::lock_guard<std::mutex> lock(m_scan_mutex);
            pending = std::move(m_pending_scan);
            m_has_pending_scan.store(false);
        }
        if (pending) {
            {
                std::lock_guard<std::mutex> lock(m_data_mutex);
                Target& def = EnsureDefaultTargetLocked();
                def.files = std::move(pending->files);
            }
            ExpandAllSourceRules(false);
            MarkDirty();
        }
    }

    // 2. Re-expand source rules when FileWatcher detects debounced filesystem changes (V2-1)
    if (m_file_watcher.ConsumePendingChanges()) {
        ExpandAllSourceRules(false);
    }

    // 3. Debounced save (§2.4): Flush dirty state if >= 500ms elapsed
    FlushDebouncedSave();
}

bool Project::Save() const {
    MarkDirty();
    return true;
}

void Project::MarkDirty() const {
    m_save_dirty = true;
    m_last_dirty_time = std::chrono::steady_clock::now();
}

void Project::FlushDebouncedSave() const {
    if (m_save_dirty) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_dirty_time).count() >= 500) {
            SaveImmediate();
            m_save_dirty = false;
        }
    }
}

bool Project::SaveImmediate() const {
    if (m_root_path.empty()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::error_code ec;
    const std::filesystem::path dir = m_root_path / ".geniebuild";
    fs_util::CreateDirsLong(dir, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path project_file = dir / "project.json";
    const std::filesystem::path v1_backup = dir / "project.v1.backup.json";
    const std::filesystem::path tmp_file = dir / "project.json.tmp";

    // Backup v1 file once before first v2 save (§2.4)
    if (m_loaded_as_v1 && fs_util::ExistsLong(project_file) && !fs_util::ExistsLong(v1_backup)) {
        fs_util::CopyFileLong(project_file, v1_backup, ec);
    }

    nlohmann::json j;
    j["version"] = 2;
    if (!m_startup_target.empty()) {
        j["startupTarget"] = m_startup_target;
    }
    if (!m_toolchain.empty()) {
        j["toolchain"] = m_toolchain;
    }
    if (!m_project_toolchains.empty()) {
        nlohmann::json tc_arr = nlohmann::json::array();
        for (const auto& tp : m_project_toolchains) {
            tc_arr.push_back(ToolchainManager::ProfileToJson(tp));
        }
        j["toolchains"] = std::move(tc_arr);
    }
    j["compiler"] = m_compiler;
    j["cCompiler"] = m_c_compiler;
    j["outputDir"] = m_output_dir;
    j["configuration"] = m_configuration;
    j["cxxStd"] = m_cxx_std;
    j["cStd"] = m_c_std;
    j["jobs"] = m_jobs;
    j["keepGoing"] = m_keep_going;
    j["lowPriority"] = m_low_priority;
    j["linkJobs"] = m_link_jobs;
    if (m_mem_per_job_mb != 2048) j["memPerJob"] = std::to_string(m_mem_per_job_mb) + "M";
    if (!m_ignore_system_headers) j["ignoreSystemHeaders"] = false;
    if (m_keep_dep_files) j["keepDepFiles"] = true;
    if (m_linker != "default") j["linker"] = m_linker;
    if (!m_incremental_link) j["incrementalLink"] = false;
    if (m_debug_link != "full") j["debugLink"] = m_debug_link;
    if (m_split_dwarf) j["splitDwarf"] = true;
    if (m_asm_mode != "auto") j["asm"] = m_asm_mode;
    if (!m_tool_overrides.empty()) j["toolOverrides"] = m_tool_overrides;
    if (!m_custom_tools.empty()) {
        nlohmann::json ct_arr = nlohmann::json::array();
        for (const auto& ct : m_custom_tools) {
            nlohmann::json ctj = nlohmann::json::object();
            ctj["ext"] = ct.ext;
            ctj["program"] = ct.program;
            ctj["args"] = ct.args;
            ctj["output"] = ct.output;
            ctj["link"] = ct.link;
            ct_arr.push_back(std::move(ctj));
        }
        j["customTools"] = std::move(ct_arr);
    }
    j["warnings"] = m_warnings;
    j["includeDirs"] = m_include_dirs;
    j["libDirs"] = m_lib_dirs;
    j["libs"] = WriteLibsWithConditions(m_libs, m_lib_conditions);
    j["defines"] = m_defines;
    j["copyDlls"] = m_copy_dlls;
    j["compileFlags"] = m_compile_flags;
    j["linkFlags"] = m_link_flags;

    nlohmann::json configs_json = nlohmann::json::object();
    for (const auto& [cfg_name, ov] : m_configs) {
        nlohmann::json cj = nlohmann::json::object();
        if (!ov.base.empty()) cj["base"] = ov.base;
        if (!ov.optimize.empty()) cj["optimize"] = ov.optimize;
        if (!ov.debugInfo.empty()) cj["debugInfo"] = ov.debugInfo;
        if (!ov.runtime.empty()) cj["runtime"] = ov.runtime;
        if (!ov.lto.empty()) cj["lto"] = ov.lto;
        cj["defines"] = ov.defines;
        cj["compileFlags"] = ov.compileFlags;
        cj["linkFlags"] = ov.linkFlags;
        configs_json[cfg_name] = std::move(cj);
    }
    j["configs"] = configs_json;
    j["confirmLargeBuild"] = m_confirm_large_build;
    if (!m_compile_commands_cfg.enabled || m_compile_commands_cfg.path != "compile_commands.json" || m_compile_commands_cfg.allConfigs) {
        nlohmann::json ccj = nlohmann::json::object();
        ccj["enabled"] = m_compile_commands_cfg.enabled;
        ccj["path"] = m_compile_commands_cfg.path;
        ccj["allConfigs"] = m_compile_commands_cfg.allConfigs;
        j["compileCommands"] = std::move(ccj);
    }

    nlohmann::json targets_json = nlohmann::json::array();
    for (const auto& t : m_targets) {
        nlohmann::json tj = nlohmann::json::object();
        tj["name"] = t.name;
        tj["kind"] = TargetKindToString(t.kind);
        if (!t.outputName.empty()) tj["outputName"] = t.outputName;
        if (!t.condition.empty()) tj["if"] = t.condition;
        if (t.exportAll) tj["exportAll"] = true;
        if (t.freestanding) tj["freestanding"] = true;
        if (!t.linkerScript.empty()) tj["linkerScript"] = t.linkerScript;
        if (!t.entry.empty()) tj["entry"] = t.entry;
        if (!t.cxxFlags.empty()) tj["cxxFlags"] = t.cxxFlags;
        if (t.linkLibgcc) tj["linkLibgcc"] = true;
        auto write_run_cfg = [](const RunConfig& rc) {
            nlohmann::json rj = nlohmann::json::object();
            if (!rc.command.empty()) rj["command"] = rc.command;
            if (!rc.args.empty()) rj["args"] = rc.args;
            if (!rc.cwd.empty()) rj["cwd"] = rc.cwd;
            if (!rc.env.empty()) rj["env"] = rc.env;
            if (!rc.buildBeforeRun) rj["buildBeforeRun"] = false;
            return rj;
        };
        if (!t.run.IsEmpty()) tj["run"] = write_run_cfg(t.run);
        if (!t.debug.IsEmpty()) tj["debug"] = write_run_cfg(t.debug);

        if (!t.sources.empty()) {
            nlohmann::json src_arr = nlohmann::json::array();
            for (const auto& r : t.sources) {
                nlohmann::json rj = nlohmann::json::object();
                rj["include"] = r.include;
                if (!r.exclude.empty()) rj["exclude"] = r.exclude;
                if (!r.condition.empty()) rj["if"] = r.condition;
                if (r.follow_links) rj["followLinks"] = true;
                src_arr.push_back(std::move(rj));
            }
            tj["sources"] = std::move(src_arr);
        }

        if (!t.files.empty()) tj["files"] = t.files;
        if (!t.exclude.empty()) tj["exclude"] = t.exclude;
        if (!t.disabledFiles.empty()) {
            tj["disabledFiles"] = std::vector<std::string>(t.disabledFiles.begin(), t.disabledFiles.end());
        }
        if (!t.deps.empty()) tj["deps"] = t.deps;
        if (!t.publicDeps.empty()) tj["publicDeps"] = t.publicDeps;
        if (!t.pub.IsEmpty()) tj["public"] = WriteSettingsBlock(t.pub);
        if (!t.priv.IsEmpty()) tj["private"] = WriteSettingsBlock(t.priv);
        if (!t.configs.empty()) {
            nlohmann::json tc = nlohmann::json::object();
            for (const auto& [cname, sb] : t.configs) {
                if (!sb.IsEmpty()) tc[cname] = WriteSettingsBlock(sb);
            }
            if (!tc.empty()) tj["configs"] = std::move(tc);
        }
        if (!t.overrides.empty()) {
            nlohmann::json ov_arr = nlohmann::json::array();
            for (const auto& fov : t.overrides) {
                nlohmann::json oj = WriteSettingsBlock(fov.settings);
                oj["match"] = fov.match;
                if (!fov.condition.empty()) oj["if"] = fov.condition;
                if (!fov.optimize.empty()) oj["optimize"] = fov.optimize;
                if (!fov.debugInfo.empty()) oj["debugInfo"] = fov.debugInfo;
                if (fov.excludeFromPch) oj["excludeFromPch"] = true;
                if (fov.excludeFromUnity) oj["excludeFromUnity"] = true;
                if (!fov.configs.empty()) {
                    nlohmann::json ocfgs = nlohmann::json::object();
                    for (const auto& [cname, ocs] : fov.configs) {
                        if (!ocs.IsEmpty()) ocfgs[cname] = WriteOverrideConfigSettings(ocs);
                    }
                    if (!ocfgs.empty()) oj["configs"] = std::move(ocfgs);
                }
                ov_arr.push_back(std::move(oj));
            }
            tj["overrides"] = std::move(ov_arr);
        }
        if (!t.steps.empty()) {
            nlohmann::json st_arr = nlohmann::json::array();
            for (const auto& st : t.steps) {
                nlohmann::json stj = nlohmann::json::object();
                stj["name"] = st.name;
                stj["phase"] = StepPhaseToString(st.phase);
                if (!st.foreachPattern.empty()) stj["foreach"] = st.foreachPattern;
                if (!st.inputs.empty()) stj["inputs"] = st.inputs;
                stj["command"] = st.command;
                if (st.outputs.size() == 1) stj["outputs"] = st.outputs[0];
                else if (st.outputs.size() > 1) stj["outputs"] = st.outputs;
                if (!st.depfile.empty()) stj["depfile"] = st.depfile;
                if (!st.condition.empty()) stj["if"] = st.condition;
                if (st.generatesSources) stj["generatesSources"] = true;
                if (st.shell) stj["shell"] = true;
                if (st.always) stj["always"] = true;
                st_arr.push_back(std::move(stj));
            }
            tj["steps"] = std::move(st_arr);
        }
        if (!t.pch.IsEmpty()) {
            nlohmann::json pj = nlohmann::json::object();
            if (!t.pch.header.empty()) pj["header"] = t.pch.header;
            if (!t.pch.cHeader.empty()) pj["cHeader"] = t.pch.cHeader;
            if (!t.pch.source.empty()) pj["source"] = t.pch.source;
            if (!t.pch.exclude.empty()) pj["exclude"] = t.pch.exclude;
            tj["pch"] = std::move(pj);
        }
        if (t.unity.enabled || t.unity.batchSize != 16 || !t.unity.exclude.empty() || !t.unity.adaptive) {
            nlohmann::json uj = nlohmann::json::object();
            uj["enabled"] = t.unity.enabled;
            uj["batchSize"] = t.unity.batchSize;
            if (!t.unity.exclude.empty()) uj["exclude"] = t.unity.exclude;
            uj["adaptive"] = t.unity.adaptive;
            tj["unity"] = std::move(uj);
        }
        targets_json.push_back(std::move(tj));
    }
    j["targets"] = std::move(targets_json);

    if (m_pane_widths.size() >= 3) {
        j["paneWidths"] = m_pane_widths;
    }

    // Atomic write via temp file + rename to avoid corruption (§2.4 & §5)
    try {
        {
            std::ofstream file(fs_util::ToWinLongPath(tmp_file));
            if (!file.is_open()) return false;
            file << j.dump(2) << std::endl;
        }
        std::error_code rename_ec;
        fs_util::RenameReplaceLong(tmp_file, project_file, rename_ec);
        if (!rename_ec) {
            m_save_dirty = false;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool Project::AddFile(const std::string& relative_path) {
    const std::string normalized = NormalizePath(relative_path);
    if (normalized.empty()) {
        return false;
    }

    std::string target_name;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        target_name = t.name;
        if (std::find(t.files.begin(), t.files.end(), normalized) != t.files.end()) {
            return false;
        }
        t.files.push_back(normalized);
    }

    ExpandAllSourceRules(false);
    MarkDirty();
    return true;
}

size_t Project::AddFiles(const std::vector<std::string>& relative_paths) {
    size_t added_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        std::unordered_set<std::string> existing(t.files.begin(), t.files.end());
        for (const auto& path : relative_paths) {
            std::string normalized = NormalizePath(path);
            if (!normalized.empty() && existing.insert(normalized).second) {
                t.files.push_back(normalized);
                added_count++;
            }
        }
    }
    if (added_count > 0) {
        ExpandAllSourceRules(false);
        MarkDirty();
    }
    return added_count;
}

bool Project::RemoveFile(const std::string& relative_path) {
    const std::string normalized = NormalizePath(relative_path);
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        auto it = std::find(t.files.begin(), t.files.end(), normalized);
        if (it != t.files.end()) {
            t.files.erase(it);
            t.disabledFiles.erase(normalized);
            removed = true;
        } else if (t.expandedFilesSet.find(normalized) != t.expandedFilesSet.end()) {
            if (std::find(t.exclude.begin(), t.exclude.end(), normalized) == t.exclude.end()) {
                t.exclude.push_back(normalized);
            }
            t.disabledFiles.erase(normalized);
            removed = true;
        }
    }
    if (removed) {
        ExpandAllSourceRules(false);
        MarkDirty();
    }
    return removed;
}

size_t Project::RemoveFiles(const std::vector<std::string>& relative_paths) {
    size_t removed_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        std::unordered_set<std::string> to_remove_set;
        to_remove_set.reserve(relative_paths.size());
        for (const auto& p : relative_paths) {
            to_remove_set.insert(NormalizePath(p));
        }

        std::vector<std::string> remaining;
        remaining.reserve(t.files.size());
        for (const auto& f : t.files) {
            if (to_remove_set.find(f) != to_remove_set.end()) {
                t.disabledFiles.erase(f);
                removed_count++;
            } else {
                remaining.push_back(f);
            }
        }
        t.files = std::move(remaining);

        for (const auto& f : to_remove_set) {
            if (t.expandedFilesSet.find(f) != t.expandedFilesSet.end() &&
                std::find(t.files.begin(), t.files.end(), f) == t.files.end()) {
                if (std::find(t.exclude.begin(), t.exclude.end(), f) == t.exclude.end()) {
                    t.exclude.push_back(f);
                    removed_count++;
                }
                t.disabledFiles.erase(f);
            }
        }
    }
    if (removed_count > 0) {
        ExpandAllSourceRules(false);
        MarkDirty();
    }
    return removed_count;
}

bool Project::ContainsFile(const std::string& relative_path) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_files_set.find(relative_path) != m_files_set.end();
}

void Project::ClearFiles() {
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        t.files.clear();
        t.sources.clear();
        t.exclude.clear();
        t.disabledFiles.clear();
    }
    ExpandAllSourceRules(false);
    MarkDirty();
}

void Project::SetPaneWidths(const std::vector<float>& widths) {
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_pane_widths = widths;
    }
    Save();
}

bool Project::IsFileEnabled(const std::string& relative_path) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::string norm = NormalizePath(relative_path);
    return (m_files_set.find(norm) != m_files_set.end()) && (m_disabled_files.find(norm) == m_disabled_files.end());
}

void Project::SetFileEnabled(const std::string& relative_path, bool enabled) {
    std::string norm = NormalizePath(relative_path);
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        if (t.expandedFilesSet.find(norm) == t.expandedFilesSet.end()) return;
        if (enabled) t.disabledFiles.erase(norm);
        else t.disabledFiles.insert(norm);
        SyncTargetsLocked();
    }
    m_revision++;
    Save();
}

void Project::ToggleFileEnabled(const std::string& relative_path) {
    std::string norm = NormalizePath(relative_path);
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        if (t.expandedFilesSet.find(norm) == t.expandedFilesSet.end()) return;
        if (t.disabledFiles.find(norm) != t.disabledFiles.end()) {
            t.disabledFiles.erase(norm);
        } else {
            t.disabledFiles.insert(norm);
        }
        SyncTargetsLocked();
    }
    m_revision++;
    Save();
}

void Project::SetFolderEnabled(const std::string& folder_rel_path, bool enabled) {
    std::string prefix = NormalizePath(folder_rel_path);
    if (!prefix.empty() && prefix.back() != '/') {
        prefix += "/";
    }
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        Target& t = EnsureDefaultTargetLocked();
        for (const auto& file : t.expandedFiles) {
            if (prefix.empty() || file.rfind(prefix, 0) == 0) {
                if (enabled) t.disabledFiles.erase(file);
                else t.disabledFiles.insert(file);
            }
        }
        SyncTargetsLocked();
    }
    m_revision++;
    Save();
}

size_t Project::GetEnabledFilesCount() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    size_t count = 0;
    for (const auto& f : m_files) {
        if (m_disabled_files.find(f) == m_disabled_files.end()) {
            count++;
        }
    }
    return count;
}

std::vector<std::string> Project::GetCompiledFiles() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::vector<std::string> result;
    result.reserve(m_files.size());
    for (const auto& f : m_files) {
        if (m_disabled_files.find(f) == m_disabled_files.end()) {
            result.push_back(f);
        }
    }
    return result;
}

} // namespace core
