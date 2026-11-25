#include "build_runner.h"
#include "settings_resolver.h"

#include <reproc++/reproc.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace core {

static std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

static bool EndsWithCaseInsensitive(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    std::string tail = ToLower(str.substr(str.size() - suffix.size()));
    return tail == ToLower(suffix);
}

static std::string Hash8Fnv1a(const std::string& s) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    uint32_t folded = static_cast<uint32_t>((hash >> 32) ^ (hash & 0xFFFFFFFFu));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", folded);
    return std::string(buf);
}

static std::filesystem::path NormalizeForCompare(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs_p = p.is_absolute() ? p : std::filesystem::absolute(p, ec);
    if (ec) abs_p = p;
    std::filesystem::path canon = std::filesystem::weakly_canonical(abs_p, ec);
    std::filesystem::path norm = (!ec && !canon.empty()) ? canon : abs_p.lexically_normal();
#if defined(_WIN32)
    std::string s = norm.generic_u8string();
    if (s.size() >= 2 && s[1] == ':' && std::isalpha(static_cast<unsigned char>(s[0]))) {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        return std::filesystem::path(s);
    }
#endif
    return norm;
}

void BuildRunner::ClassifyDiagnosticLine(
    const std::string& line,
    bool& out_has_error,
    bool& out_has_warning
) {
    std::string lower = ToLower(line);

    // Matches MSVC / tool codes like "error C2065:", "error D8021 :", "warning C4101:", "error LNK2019:"
    auto has_diag_code = [](const std::string& s, const std::string& kind) -> bool {
        const std::string prefix = kind + " ";
        size_t pos = 0;
        while ((pos = s.find(prefix, pos)) != std::string::npos) {
            if (pos > 0 && std::isalnum(static_cast<unsigned char>(s[pos - 1]))) {
                pos += prefix.size();
                continue;
            }
            size_t i = pos + prefix.size();
            size_t alpha_count = 0;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && alpha_count < 5) {
                ++i;
                ++alpha_count;
            }
            if (alpha_count >= 1 && alpha_count <= 4) {
                size_t digit_count = 0;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                    ++i;
                    ++digit_count;
                }
                if (digit_count >= 1) {
                    while (i < s.size() && s[i] == ' ') {
                        ++i;
                    }
                    if (i < s.size() && s[i] == ':') {
                        return true;
                    }
                }
            }
            pos += prefix.size();
        }
        return false;
    };

    // Matches LNK\d+ (e.g. LNK1104, LNK2019)
    auto has_lnk_code = [](const std::string& s) -> bool {
        size_t pos = 0;
        while ((pos = s.find("LNK", pos)) != std::string::npos) {
            if (pos > 0 && std::isalnum(static_cast<unsigned char>(s[pos - 1]))) {
                pos += 3;
                continue;
            }
            size_t i = pos + 3;
            size_t digits = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                ++i;
                ++digits;
            }
            if (digits >= 1 && (i == s.size() || !std::isalnum(static_cast<unsigned char>(s[i])))) {
                return true;
            }
            pos += 3;
        }
        return false;
    };

    const bool is_warning_diag =
        (lower.find(": warning:") != std::string::npos) ||
        (lower.rfind("warning:", 0) == 0) ||
        has_diag_code(lower, "warning");

    const bool is_error_diag =
        (lower.find(": error:") != std::string::npos) ||
        (lower.rfind("error:", 0) == 0) ||
        (lower.find(": fatal error:") != std::string::npos) ||
        (lower.rfind("fatal error:", 0) == 0) ||
        has_diag_code(lower, "error") ||
        (!is_warning_diag && has_lnk_code(line)) ||
        (lower.find(": cannot find -l") != std::string::npos) ||
        (lower.find(": undefined reference to ") != std::string::npos) ||
        (lower.find(": multiple definition of ") != std::string::npos);

    out_has_error = is_error_diag;
    out_has_warning = !is_error_diag && is_warning_diag;
}

static OutLine MakeClassifiedRawLine(std::string text) {
    OutLine ol;
    ol.kind = OutLineKind::Raw;
    BuildRunner::ClassifyDiagnosticLine(text, ol.has_error, ol.has_warning);
    ol.text = std::move(text);
    return ol;
}

std::filesystem::path BuildRunner::ComputeObjectPath(
    const Project& project,
    const std::filesystem::path& obj_dir,
    const std::string& file_rel,
    bool is_msvc,
    std::string& out_obj_key
) {
    const std::string obj_ext = is_msvc ? ".obj" : ".o";
    std::filesystem::path root_norm = NormalizeForCompare(project.GetRootPath());
    std::filesystem::path src_norm = NormalizeForCompare(project.ResolvePath(file_rel));
    std::filesystem::path rel = src_norm.lexically_relative(root_norm);

    bool inside_root = !rel.empty() && !rel.is_absolute();
    if (inside_root) {
        std::string rel_s = rel.generic_u8string();
        if (rel_s == "." || rel_s == ".." || rel_s.rfind("../", 0) == 0 || rel_s.find(':') != std::string::npos) {
            inside_root = false;
        } else {
            for (const auto& part : rel) {
                if (part == "..") {
                    inside_root = false;
                    break;
                }
            }
        }
    }

    std::string rel_obj;
    if (inside_root) {
        rel_obj = rel.generic_u8string() + obj_ext;
        std::string candidate_full = (obj_dir / rel_obj).generic_u8string();
        if (candidate_full.size() > 220 || rel_obj.size() > 160) {
            std::string fname = src_norm.filename().generic_u8string();
            if (fname.empty()) fname = "source";
            if (fname.size() > 80) fname = fname.substr(0, 80);
            rel_obj = "deep/" + Hash8Fnv1a(rel.generic_u8string()) + "_" + fname + obj_ext;
        }
    } else {
        std::string fname = src_norm.filename().generic_u8string();
        if (fname.empty()) fname = "source";
        if (fname.size() > 80) fname = fname.substr(0, 80);
        std::string hash_input = src_norm.generic_u8string();
        if (hash_input.empty()) hash_input = file_rel;
        rel_obj = "ext/" + Hash8Fnv1a(hash_input) + "_" + fname + obj_ext;
    }

    out_obj_key = rel_obj;
    return obj_dir / rel_obj;
}

static std::string ComputeFileContentHash(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return "missing";
    }
    uint64_t hash = 14695981039346656037ULL;
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ULL;
        }
    }
    char out[32];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(out);
}

static bool IsAnyResolvedLibraryNewer(
    const Project& project,
    const EffectiveTargetSettings& eff,
    bool is_msvc,
    std::filesystem::file_time_type bin_time
) {
    std::vector<std::filesystem::path> resolved_lib_dirs;
    resolved_lib_dirs.reserve(eff.libDirs.size());
    for (const auto& dir : eff.libDirs) {
        resolved_lib_dirs.push_back(project.ResolvePath(dir));
    }

    auto is_lib_newer = [&](const std::string& raw_lib) -> bool {
        if (raw_lib.empty()) return false;
        std::error_code ec;

        // Direct path to a library file
        if (raw_lib.find('/') != std::string::npos || raw_lib.find('\\') != std::string::npos) {
            std::filesystem::path p = project.ResolvePath(raw_lib);
            if (std::filesystem::exists(p, ec) && !ec && std::filesystem::is_regular_file(p, ec)) {
                auto t = std::filesystem::last_write_time(p, ec);
                if (!ec && t > bin_time) {
                    return true;
                }
            }
            return false;
        }

        std::string lib = raw_lib;
        if (lib.rfind("-l", 0) == 0) {
            lib = lib.substr(2);
        }
        if (lib.empty()) return false;

        std::vector<std::string> candidates;
        if (lib[0] == ':') {
            std::string exact = lib.substr(1);
            if (!exact.empty()) candidates.push_back(exact);
        } else if (EndsWithCaseInsensitive(lib, ".lib") ||
                   EndsWithCaseInsensitive(lib, ".a") ||
                   EndsWithCaseInsensitive(lib, ".so") ||
                   EndsWithCaseInsensitive(lib, ".dylib") ||
                   EndsWithCaseInsensitive(lib, ".dll")) {
            candidates.push_back(lib);
        } else if (is_msvc) {
            candidates = {
                lib + ".lib",
                "lib" + lib + ".lib"
            };
        } else {
            candidates = {
                "lib" + lib + ".dll.a",
                lib + ".dll.a",
                "lib" + lib + ".a",
                lib + ".a",
                lib + ".lib",
                "lib" + lib + ".lib",
                "lib" + lib + ".so",
                "lib" + lib + ".dylib",
                lib + ".dll"
            };
        }

        for (const auto& dir : resolved_lib_dirs) {
            bool found_in_dir = false;
            for (const auto& cand : candidates) {
                std::filesystem::path p = dir / cand;
                if (std::filesystem::exists(p, ec) && !ec && std::filesystem::is_regular_file(p, ec)) {
                    found_in_dir = true;
                    auto t = std::filesystem::last_write_time(p, ec);
                    if (!ec && t > bin_time) {
                        return true;
                    }
                }
            }
            if (found_in_dir) {
                return false;
            }
        }
        return false;
    };

    for (const auto& lib_entry : eff.libs) {
        if (is_lib_newer(lib_entry)) {
            return true;
        }
    }

    for (const auto& flag : eff.linkFlags) {
        for (const auto& token : CompilerManager::SplitFlags(flag)) {
            if ((token.rfind("-l", 0) == 0 && token.size() > 2) ||
                EndsWithCaseInsensitive(token, ".lib") ||
                EndsWithCaseInsensitive(token, ".a") ||
                EndsWithCaseInsensitive(token, ".so") ||
                EndsWithCaseInsensitive(token, ".dylib")) {
                if (is_lib_newer(token)) {
                    return true;
                }
            }
        }
    }

    return false;
}

// Helper: executes an external process with reproc and captures stdout+stderr into out_lines
static int ExecuteProcess(
    const BuildCommand& cmd,
    const std::filesystem::path& working_dir,
    std::vector<OutLine>& out_lines
) {
    reproc::process process;
    reproc::options options;
    std::string wd = working_dir.generic_u8string();
    options.working_directory = wd.c_str();

    options.redirect.out.type = reproc::redirect::pipe;
    options.redirect.err.type = reproc::redirect::stdout_;

    std::vector<std::string> reproc_args;
    reproc_args.push_back(cmd.program);
    for (const auto& arg : cmd.arguments) {
        reproc_args.push_back(arg);
    }

    std::error_code ec = process.start(reproc_args, options);
    if (ec) {
        OutLine ol;
        ol.kind = OutLineKind::Raw;
        ol.text = "Failed to launch process '" + cmd.program + "': " + ec.message();
        ol.has_error = true;
        out_lines.push_back(ol);
        return -1;
    }

    uint8_t buffer[4096];
    std::string accumulator;

    while (true) {
        auto [bytes_read, read_ec] = process.read(reproc::stream::out, buffer, sizeof(buffer));
        if (read_ec) {
            break;
        }

        if (bytes_read > 0) {
            accumulator.append(reinterpret_cast<char*>(buffer), bytes_read);

            size_t pos = 0;
            while ((pos = accumulator.find('\n')) != std::string::npos) {
                std::string line = accumulator.substr(0, pos);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                accumulator.erase(0, pos + 1);

                out_lines.push_back(MakeClassifiedRawLine(std::move(line)));
            }
        }
    }

    if (!accumulator.empty()) {
        if (accumulator.back() == '\r') {
            accumulator.pop_back();
        }
        out_lines.push_back(MakeClassifiedRawLine(std::move(accumulator)));
    }

    auto [status, wait_ec] = process.wait(reproc::infinite);
    if (!wait_ec) {
        return status;
    }
    return -1;
}

// Parses gcc/clang Makefile dependency file (.d)
static std::vector<std::string> ParseMakefileDeps(const std::filesystem::path& dep_path) {
    std::vector<std::string> deps;
    std::ifstream file(dep_path);
    if (!file.is_open()) return deps;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t colon_pos = content.find(':');
    if (colon_pos == std::string::npos) return deps;

    std::string rest = content.substr(colon_pos + 1);
    std::string cur;
    bool escaped = false;

    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (escaped) {
            if (c == '\n' || c == '\r') {
                // Continuation line, ignore
            } else {
                cur += c;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                deps.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        deps.push_back(cur);
    }
    return deps;
}

// Parses MSVC /sourceDependencies JSON output
static std::vector<std::string> ParseMsvcDeps(const std::filesystem::path& json_path) {
    std::vector<std::string> deps;
    try {
        std::ifstream file(json_path);
        if (!file.is_open()) return deps;
        nlohmann::json j;
        file >> j;
        if (j.contains("Data") && j["Data"].is_object() &&
            j["Data"].contains("Includes") && j["Data"]["Includes"].is_array()) {
            for (const auto& item : j["Data"]["Includes"]) {
                if (item.is_string()) {
                    deps.push_back(item.get<std::string>());
                }
            }
        }
    } catch (...) {}
    return deps;
}

BuildRunner::BuildRunner() = default;

BuildRunner::~BuildRunner() {
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

BuildResult BuildRunner::GetLastResult() const {
    std::lock_guard<std::mutex> lock(m_result_mutex);
    return m_last_result;
}

double BuildRunner::GetElapsedSeconds() const {
    std::lock_guard<std::mutex> lock(m_result_mutex);
    if (m_status.load() == BuildStatus::Running) {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double>(now - m_start_time).count();
    }
    return m_last_result.duration_seconds;
}

std::string BuildRunner::GetExecutedCommand() const {
    std::lock_guard<std::mutex> lock(m_result_mutex);
    return m_executed_command;
}

std::vector<OutLine> BuildRunner::DrainOutput() {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    std::vector<OutLine> drained;
    drained.swap(m_output_queue);
    return drained;
}

bool BuildRunner::Clean(const Project& project, std::string& out_message, const std::string& target_name) {
    if (IsRunning()) {
        out_message = "Cannot clean: A build is currently running.";
        return false;
    }
    std::error_code ec;
    std::string config = project.GetConfiguration();
    std::string comp_lower = ToLower(project.GetCompiler());
    bool is_msvc = (comp_lower == "cl" || comp_lower == "cl.exe");
    std::filesystem::path abs_out = project.GetAbsoluteOutputDir();

    size_t removed = 0;
    const auto& targets = project.GetTargets();
    if (!targets.empty()) {
        for (const auto& t : targets) {
            if (!target_name.empty() && t.name != target_name) continue;
            std::filesystem::path obj_dir = t.GetObjectDir(project.GetRootPath(), config);
            if (std::filesystem::exists(obj_dir, ec)) {
                removed += std::filesystem::remove_all(obj_dir, ec);
            }
            std::filesystem::path bin_p = t.GetBinaryPath(abs_out, config, is_msvc);
            if (!bin_p.empty() && std::filesystem::exists(bin_p, ec)) {
                if (std::filesystem::remove(bin_p, ec)) removed++;
            }
            std::filesystem::path imp_p = t.GetImportLibPath(abs_out, config, is_msvc);
            if (!imp_p.empty() && std::filesystem::exists(imp_p, ec)) {
                if (std::filesystem::remove(imp_p, ec)) removed++;
            }
        }
    } else {
        std::filesystem::path obj_dir = project.GetObjectDir(config);
        std::filesystem::path bin_path = project.GetBinaryPath(config);
        if (std::filesystem::exists(obj_dir, ec)) {
            removed += std::filesystem::remove_all(obj_dir, ec);
        }
        if (std::filesystem::exists(bin_path, ec)) {
            if (std::filesystem::remove(bin_path, ec)) removed++;
        }
    }
    out_message = "Cleaned configuration '" + config + "'" +
                  (target_name.empty() ? "" : (" (target '" + target_name + "')")) +
                  " (" + std::to_string(removed) + " objects/binaries deleted).";
    return true;
}

bool BuildRunner::StartRebuild(
    Project& project,
    const CompilerManager& compiler_manager,
    std::string& out_error,
    const std::string& target_name
) {
    std::string clean_msg;
    Clean(project, clean_msg, target_name);
    return StartBuild(project, compiler_manager, out_error, target_name);
}

bool BuildRunner::StartBuild(
    Project& project,
    const CompilerManager& compiler_manager,
    std::string& out_error,
    const std::string& target_name
) {
    if (IsRunning()) {
        out_error = "A build is already in progress.";
        return false;
    }

    // Re-expand dirty directories right before build so disk changes are always seen (V2-1)
    project.ExpandAllSourceRules(true);

    // Resolve target dependency graph & check for cycles / missing deps / duplicate outputs (V2-2)
    std::vector<Target> targets_snapshot = project.GetTargetsSnapshot();
    std::vector<const Target*> build_order;
    if (!ResolveTargetBuildOrder(targets_snapshot, target_name, build_order, out_error)) {
        return false;
    }

    // Guard 1: At least one non-interface target must have compiled source files
    size_t total_sources = 0;
    size_t total_enabled = 0;
    size_t missing_count = 0;
    for (const Target* t : build_order) {
        if (t->kind == TargetKind::Interface) continue;
        total_sources += t->expandedFiles.size();
        total_enabled += t->GetEnabledFilesCount();
        missing_count += t->missingFiles.size();
    }

    if (total_sources == 0) {
        out_error = "Cannot build: No source files in the build target(s).\nPlease add source files or glob rules.";
        return false;
    }
    if (total_enabled == 0) {
        out_error = "Cannot build: No source files are enabled for compilation.\nPlease check at least one file in the Targets pane.";
        return false;
    }

    // Guard 2: Missing files guard
    if (missing_count > 0) {
        out_error = "Cannot build: " + std::to_string(missing_count) +
                    " file(s) in the build list are missing on disk.\nPlease remove missing files to proceed.";
        return false;
    }

    // Guard 3: Missing compiler guard
    const CompilerInfo* compiler = compiler_manager.FindCompiler(project.GetCompiler());
    if (!compiler) {
        out_error = "Cannot build: Selected compiler '" + project.GetCompiler() + "' was not found on PATH.\nPlease install a supported compiler or choose an available one.";
        return false;
    }

    // Guard 4: MSVC Environment guard (Spec §5.1)
    std::string comp_lower = ToLower(project.GetCompiler());
    const bool is_msvc = (comp_lower == "cl" || comp_lower == "cl.exe");
    if (is_msvc) {
        if (!CompilerManager::CheckMsvcEnvironment(out_error)) {
            return false;
        }
    }

    // Guard 5: Missing library directory guard across effective target settings
    std::error_code ec;
    std::unordered_map<std::string, const Target*> target_map;
    for (const auto& t : targets_snapshot) target_map[t.name] = &t;

    // Guard 6: MSVC runtime CRT consistency check (V2-4)
    if (!CheckRuntimeConsistency(project, build_order, target_map, project.GetConfiguration(), out_error)) {
        return false;
    }

    for (const Target* t : build_order) {
        EffectiveTargetSettings eff = ResolveEffectiveTargetSettings(project, *t, target_map, project.GetConfiguration(), is_msvc);
        for (const auto& lib_dir : eff.libDirs) {
            std::filesystem::path resolved = project.ResolvePath(lib_dir);
            if (!std::filesystem::exists(resolved, ec) || !std::filesystem::is_directory(resolved, ec)) {
                out_error = "Cannot build target '" + t->name + "': Library search directory not found:\n" + lib_dir +
                            "\nResolved: " + resolved.generic_u8string() + "\nPlease check Settings → Libraries.";
                return false;
            }
        }
    }

    // Prepare log file path (.geniebuild/last_build.log)
    std::filesystem::path gb_dir = project.GetRootPath() / ".geniebuild";
    std::filesystem::create_directories(gb_dir, ec);
    std::filesystem::path log_path = gb_dir / "last_build.log";

    // Wait for any previous finished thread to join cleanly
    if (m_worker.joinable()) {
        m_worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_output_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_executed_command = "";
        m_last_result = BuildResult{};
        m_start_time = std::chrono::high_resolution_clock::now();
    }

    m_status.store(BuildStatus::Running);

    // Launch unified multi-target build engine on background thread
    m_worker = std::thread(
        &BuildRunner::WorkerRun,
        this,
        &project,
        &compiler_manager,
        log_path,
        std::move(targets_snapshot),
        target_name
    );

    return true;
}

void BuildRunner::WorkerRun(
    const Project* project_ptr,
    const CompilerManager* compiler_manager_ptr,
    std::filesystem::path log_path,
    std::vector<Target> targets_snapshot,
    std::string target_filter
) {
    if (!project_ptr || !compiler_manager_ptr) return;
    const Project& project = *project_ptr;
    const CompilerManager& compiler_manager = *compiler_manager_ptr;

    const auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::string> all_logged_lines;

    const std::string config = project.GetConfiguration();
    const CompilerInfo* cxx_comp = compiler_manager.FindCompiler(project.GetCompiler());
    const CompilerInfo* c_comp = compiler_manager.FindCompiler(project.GetCCompiler());
    if (!c_comp) c_comp = cxx_comp;

    std::string comp_lower = ToLower(project.GetCompiler());
    const bool is_msvc = (comp_lower == "cl" || comp_lower == "cl.exe");
    const std::filesystem::path abs_out = project.GetAbsoluteOutputDir();

    std::string resolve_err;
    std::vector<const Target*> build_order;
    if (!ResolveTargetBuildOrder(targets_snapshot, target_filter, build_order, resolve_err)) {
        OutLine err_ol;
        err_ol.kind = OutLineKind::Status;
        err_ol.text = "✗ " + resolve_err;
        err_ol.has_error = true;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_output_queue.push_back(err_ol);
        }
        all_logged_lines.push_back(err_ol.text);
        std::ofstream log_file(log_path);
        if (log_file.is_open()) {
            for (const auto& l : all_logged_lines) log_file << l << "\n";
        }
        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            m_last_result = { 1, 0.0, false };
        }
        m_status.store(BuildStatus::Finished);
        return;
    }

    std::unordered_map<std::string, const Target*> target_map;
    for (const auto& t : targets_snapshot) target_map[t.name] = &t;

    const bool multi_target_output = (build_order.size() > 1);
    std::error_code ec;

    // Header timestamp cache per build (§5.1.5: each header checked on disk only once!)
    std::unordered_map<std::string, std::filesystem::file_time_type> header_time_cache;
    auto get_header_time = [&](const std::string& path_str) -> std::filesystem::file_time_type {
        auto it = header_time_cache.find(path_str);
        if (it != header_time_cache.end()) {
            return it->second;
        }
        std::error_code time_ec;
        std::filesystem::path resolved = project.ResolvePath(path_str);
        auto t = std::filesystem::last_write_time(resolved, time_ec);
        if (time_ec) {
            t = std::filesystem::file_time_type::min();
        }
        header_time_cache[path_str] = t;
        return t;
    };

    struct TargetBuildPlan {
        const Target* target = nullptr;
        EffectiveTargetSettings eff;
        std::filesystem::path obj_dir;
        std::filesystem::path state_path;
        std::unordered_map<std::string, std::string> prev_state;
        std::unordered_map<std::string, std::string> current_state;
        std::vector<std::filesystem::path> object_paths;
        size_t compiled_task_count = 0;
    };

    std::vector<TargetBuildPlan> plans(build_order.size());

    struct Task {
        size_t plan_idx = 0;
        std::string target_name;
        std::string src_rel;
        std::filesystem::path obj_path;
        std::string obj_key;
        BuildCommand cmd;
        std::string cmd_hash;
    };

    std::vector<Task> tasks;
    size_t up_to_date_count = 0;

    auto load_state_map = [](const std::filesystem::path& state_path) {
        std::unordered_map<std::string, std::string> m;
        std::ifstream state_file(state_path);
        if (state_file.is_open()) {
            try {
                nlohmann::json j;
                state_file >> j;
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        if (it.value().is_string()) {
                            m[it.key()] = it.value().get<std::string>();
                        }
                    }
                }
            } catch (...) {}
        }
        return m;
    };

    auto save_state_map = [](const std::filesystem::path& state_path, const std::unordered_map<std::string, std::string>& m) {
        try {
            nlohmann::json j_state = m;
            std::ofstream sf(state_path);
            if (sf.is_open()) {
                sf << j_state.dump(2) << std::endl;
            }
        } catch (...) {}
    };

    // 1. Plan all compile tasks across all targets (compile nodes are ready immediately for max parallelism)
    for (size_t pi = 0; pi < build_order.size(); ++pi) {
        const Target* t = build_order[pi];
        TargetBuildPlan& plan = plans[pi];
        plan.target = t;

        bool needs_pic = (t->kind == TargetKind::Static) && IsStaticTargetConsumedByShared(targets_snapshot, t->name);
        plan.eff = ResolveEffectiveTargetSettings(project, *t, target_map, config, is_msvc, needs_pic);

        if (t->kind == TargetKind::Interface) {
            continue;
        }

        plan.obj_dir = t->GetObjectDir(project.GetRootPath(), config);
        std::filesystem::create_directories(plan.obj_dir, ec);
        plan.state_path = plan.obj_dir / "state.json";
        plan.prev_state = load_state_map(plan.state_path);
        plan.current_state = plan.prev_state;

        std::vector<std::string> compiled_files = t->GetCompiledFiles();

        // Emit schema/override validation warnings once per build (V2-3)
        for (const auto& wmsg : ValidateTargetOverrides(*t)) {
            OutLine warn_ol;
            warn_ol.kind = OutLineKind::Raw;
            warn_ol.text = "> Warning: " + wmsg;
            warn_ol.has_warning = true;
            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                m_output_queue.push_back(warn_ol);
            }
            all_logged_lines.push_back(warn_ol.text);
        }

        // Resolve per-file overrides using signature grouping (V2-3)
        auto grouped_settings = ResolveTargetFilesGrouped(
            project, *t, plan.eff, compiled_files, config, project.GetCompiler()
        );

        for (const auto& file_rel : compiled_files) {
            std::filesystem::path src_full = project.ResolvePath(file_rel);
            std::string ext = ToLower(src_full.extension().generic_u8string());
            bool is_c = (ext == ".c");

            const CompilerInfo* comp_info = is_c ? c_comp : cxx_comp;
            if (!comp_info) comp_info = cxx_comp;

            std::string obj_key;
            std::filesystem::path obj_file = ComputeObjectPath(project, plan.obj_dir, file_rel, is_msvc, obj_key);
            std::filesystem::create_directories(obj_file.parent_path(), ec);
            plan.object_paths.push_back(obj_file);

            auto eff_it = grouped_settings.find(file_rel);
            BuildCommand cmd = (eff_it != grouped_settings.end() && eff_it->second)
                ? CompilerManager::FormulateCompileCommand(*comp_info, file_rel, obj_file, project, *eff_it->second)
                : CompilerManager::FormulateCompileCommand(*comp_info, file_rel, obj_file, project, plan.eff);
            std::string cmd_hash = CompilerManager::ComputeCommandHash(cmd);

            bool needs_compile = false;

            if (!std::filesystem::exists(obj_file, ec) || ec) {
                needs_compile = true;
            } else {
                auto src_time = std::filesystem::last_write_time(src_full, ec);
                auto obj_time = std::filesystem::last_write_time(obj_file, ec);

                if (!ec && src_time > obj_time) {
                    needs_compile = true;
                } else if (plan.prev_state.find(obj_key) == plan.prev_state.end() || plan.prev_state[obj_key] != cmd_hash) {
                    needs_compile = true;
                } else {
                    std::vector<std::string> deps;
                    std::filesystem::path dep_path(obj_file.generic_u8string() + (is_msvc ? ".json" : ".d"));
                    if (is_msvc) {
                        deps = ParseMsvcDeps(dep_path);
                    } else {
                        deps = ParseMakefileDeps(dep_path);
                    }

                    for (const auto& dep : deps) {
                        if (dep == file_rel || dep == src_full.generic_u8string()) {
                            continue;
                        }
                        if (get_header_time(dep) > obj_time) {
                            needs_compile = true;
                            break;
                        }
                    }
                }
            }

            if (needs_compile) {
                plan.compiled_task_count++;
                tasks.push_back(Task{ pi, t->name, file_rel, obj_file, obj_key, cmd, cmd_hash });
            } else {
                up_to_date_count++;
            }
        }
    }

    const size_t total_tasks = tasks.size();
    bool build_ok = true;
    std::atomic<int> first_exit_code{0};

    // 2. Parallel compile phase across all targets
    if (total_tasks > 0) {
        OutLine first_cmd_line;
        first_cmd_line.kind = OutLineKind::Command;
        first_cmd_line.text = tasks[0].cmd.display_command;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_output_queue.push_back(first_cmd_line);
        }
        all_logged_lines.push_back("> " + tasks[0].cmd.display_command);

        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            m_executed_command = tasks[0].cmd.display_command;
        }

        int num_workers = project.GetJobs();
        if (num_workers <= 0) {
            num_workers = static_cast<int>(std::thread::hardware_concurrency());
        }
        if (num_workers <= 0) num_workers = 4;
        if (num_workers > static_cast<int>(total_tasks)) {
            num_workers = static_cast<int>(total_tasks);
        }

        std::atomic<size_t> next_task_idx{0};
        std::atomic<bool> compile_failed{false};
        std::atomic<size_t> completed_counter{0};
        std::mutex state_mutex;

        std::vector<std::thread> workers;
        workers.reserve(num_workers);

        for (int w = 0; w < num_workers; ++w) {
            workers.emplace_back([&]() {
                while (!compile_failed.load()) {
                    size_t idx = next_task_idx.fetch_add(1);
                    if (idx >= total_tasks) {
                        break;
                    }

                    const auto& task = tasks[idx];
                    std::vector<OutLine> local_lines;
                    int code = ExecuteProcess(task.cmd, project.GetRootPath(), local_lines);

                    size_t done_now = completed_counter.fetch_add(1) + 1;

                    if (code != 0) {
                        int expected = 0;
                        first_exit_code.compare_exchange_strong(expected, code);
                        compile_failed.store(true);
                    }

                    {
                        std::lock_guard<std::mutex> lock(m_queue_mutex);
                        OutLine prog;
                        prog.kind = OutLineKind::Progress;
                        if (multi_target_output) {
                            prog.text = "[" + std::to_string(done_now) + "/" + std::to_string(total_tasks) + "] [" + task.target_name + "] " + task.src_rel;
                        } else {
                            prog.text = "[" + std::to_string(done_now) + "/" + std::to_string(total_tasks) + "] " + task.src_rel;
                        }
                        m_output_queue.push_back(prog);
                        all_logged_lines.push_back(prog.text);

                        for (const auto& l : local_lines) {
                            m_output_queue.push_back(l);
                            all_logged_lines.push_back(l.text);
                        }
                    }

                    if (code == 0) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        plans[task.plan_idx].current_state[task.obj_key] = task.cmd_hash;
                    }
                }
            });
        }

        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        if (compile_failed.load()) {
            build_ok = false;
        }

        for (const auto& plan : plans) {
            if (plan.compiled_task_count > 0) {
                save_state_map(plan.state_path, plan.current_state);
            }
        }
    }

    // 3. Archive / Link step in topological dependency order (V2-2 + E1 + import-lib hash)
    size_t total_links = 0;
    std::unordered_set<std::string> rebuilt_static_outputs;
    std::vector<std::filesystem::path> runtime_bin_dirs;
    std::vector<std::string> all_copy_dlls;

    if (build_ok) {
        for (auto& plan : plans) {
            const Target& t = *plan.target;
            if (t.kind == TargetKind::Interface || plan.object_paths.empty()) {
                continue;
            }

            std::filesystem::path binary_path = t.GetBinaryPath(abs_out, config, is_msvc);
            std::filesystem::create_directories(binary_path.parent_path(), ec);
            if (t.kind == TargetKind::Executable || t.kind == TargetKind::Shared) {
                runtime_bin_dirs.push_back(binary_path.parent_path());
            }
            for (const auto& d : plan.eff.copyDlls) {
                if (std::find(all_copy_dlls.begin(), all_copy_dlls.end(), d) == all_copy_dlls.end()) {
                    all_copy_dlls.push_back(d);
                }
            }

            BuildCommand link_cmd_preview = CompilerManager::FormulateLinkCommand(
                *cxx_comp, t, plan.eff, plan.object_paths, binary_path, project, false
            );
            // Fold shared dependency import-library content hashes into link_hash (V2-2)
            std::vector<std::filesystem::path> hash_items = plan.object_paths;
            for (const auto& [dep_name, implib_p] : plan.eff.sharedDepImportLibs) {
                std::string imp_hash = ComputeFileContentHash(implib_p);
                hash_items.push_back(std::filesystem::path("__implib__:" + dep_name + ":" + imp_hash));
            }
            std::string link_hash = CompilerManager::ComputeLinkHash(link_cmd_preview, hash_items);

            bool needs_link = false;
            bool binary_exists = std::filesystem::exists(binary_path, ec) && !ec;
            if (t.kind == TargetKind::Shared) {
                std::filesystem::path implib_p = t.GetImportLibPath(abs_out, config, is_msvc);
                if (!implib_p.empty() && (!std::filesystem::exists(implib_p, ec) || ec)) {
                    binary_exists = false;
                }
            }

            auto prev_link_it = plan.prev_state.find("link_hash");
            if (!binary_exists || plan.compiled_task_count > 0 ||
                prev_link_it == plan.prev_state.end() || prev_link_it->second != link_hash) {
                needs_link = true;
            } else {
                auto bin_time = std::filesystem::last_write_time(binary_path, ec);
                if (ec) {
                    needs_link = true;
                } else {
                    for (const auto& obj_p : plan.object_paths) {
                        std::error_code obj_ec;
                        auto obj_t = std::filesystem::last_write_time(obj_p, obj_ec);
                        if (obj_ec || obj_t > bin_time) {
                            needs_link = true;
                            break;
                        }
                    }
                    if (!needs_link) {
                        for (const auto& static_lib : plan.eff.staticDepLibraries) {
                            if (rebuilt_static_outputs.find(static_lib.generic_u8string()) != rebuilt_static_outputs.end()) {
                                needs_link = true;
                                break;
                            }
                            std::error_code sl_ec;
                            auto sl_t = std::filesystem::last_write_time(static_lib, sl_ec);
                            if (sl_ec || sl_t > bin_time) {
                                needs_link = true;
                                break;
                            }
                        }
                    }
                    if (!needs_link && IsAnyResolvedLibraryNewer(project, plan.eff, is_msvc, bin_time)) {
                        needs_link = true;
                    }
                }
            }

            if (!needs_link) {
                continue;
            }

            total_links++;
            if (t.kind == TargetKind::Static && std::filesystem::exists(binary_path, ec)) {
                // Ensure removed objects do not persist inside an existing static archive
                std::filesystem::remove(binary_path, ec);
            }

            BuildCommand link_cmd = CompilerManager::FormulateLinkCommand(
                *cxx_comp, t, plan.eff, plan.object_paths, binary_path, project, true
            );

            if (total_tasks == 0 && total_links == 1) {
                std::lock_guard<std::mutex> lock(m_result_mutex);
                m_executed_command = link_cmd.display_command;
            }

            OutLine link_line;
            link_line.kind = OutLineKind::Command;
            link_line.text = link_cmd.display_command;
            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                m_output_queue.push_back(link_line);
            }
            all_logged_lines.push_back("> " + link_cmd.display_command);

            std::vector<OutLine> link_output;
            int link_code = ExecuteProcess(link_cmd, project.GetRootPath(), link_output);

            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                for (const auto& l : link_output) {
                    m_output_queue.push_back(l);
                    all_logged_lines.push_back(l.text);
                }
            }

            if (link_code == 0) {
                plan.current_state["link_hash"] = link_hash;
                save_state_map(plan.state_path, plan.current_state);
                if (t.kind == TargetKind::Static) {
                    rebuilt_static_outputs.insert(binary_path.generic_u8string());
                }
            } else {
                build_ok = false;
                int expected = 0;
                first_exit_code.compare_exchange_strong(expected, link_code);
                plan.current_state.erase("link_hash");
                save_state_map(plan.state_path, plan.current_state);

                bool lib_not_found = false;
                for (const auto& l : link_output) {
                    std::string low = ToLower(l.text);
                    if (low.find("cannot find -l") != std::string::npos || low.find("lnk1104") != std::string::npos) {
                        lib_not_found = true;
                        break;
                    }
                }
                if (lib_not_found) {
                    OutLine hint;
                    hint.kind = OutLineKind::Raw;
                    hint.text = "> Hint: Check Settings → Libraries to ensure library names and search directories are correct.";
                    hint.has_warning = true;
                    std::lock_guard<std::mutex> lock(m_queue_mutex);
                    m_output_queue.push_back(hint);
                    all_logged_lines.push_back(hint.text);
                }
                break;
            }
        }
    }

    // 4. Post-link DLL copying (§5.1.2)
    if (build_ok && !all_copy_dlls.empty()) {
        std::filesystem::path bin_dir = runtime_bin_dirs.empty()
            ? (abs_out / config / "bin")
            : runtime_bin_dirs[0];
        for (const auto& dll_rel : all_copy_dlls) {
            std::filesystem::path src_dll = project.ResolvePath(dll_rel);
            if (!std::filesystem::exists(src_dll, ec) || ec) {
                OutLine warn;
                warn.kind = OutLineKind::Raw;
                warn.text = "> Warning: DLL to copy not found: " + dll_rel + " (resolved: " + src_dll.generic_u8string() + ")";
                warn.has_warning = true;
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                m_output_queue.push_back(warn);
                all_logged_lines.push_back(warn.text);
            } else {
                std::filesystem::path dst_dll = bin_dir / src_dll.filename();
                bool should_copy = true;
                if (std::filesystem::exists(dst_dll, ec) && !ec) {
                    auto src_t = std::filesystem::last_write_time(src_dll, ec);
                    auto dst_t = std::filesystem::last_write_time(dst_dll, ec);
                    if (!ec && src_t <= dst_t) {
                        should_copy = false;
                    }
                }
                if (should_copy) {
                    std::filesystem::create_directories(bin_dir, ec);
                    std::filesystem::copy_file(src_dll, dst_dll, std::filesystem::copy_options::overwrite_existing, ec);
                    OutLine copy_msg;
                    copy_msg.kind = OutLineKind::Raw;
                    copy_msg.text = "> Copied " + src_dll.filename().generic_u8string();
                    std::lock_guard<std::mutex> lock(m_queue_mutex);
                    m_output_queue.push_back(copy_msg);
                    all_logged_lines.push_back(copy_msg.text);
                }
            }
        }
    }

    const auto end_time = std::chrono::high_resolution_clock::now();
    const double duration = std::chrono::duration<double>(end_time - start_time).count();

    const int final_exit_code = first_exit_code.load();

    // Summary line formatted with counts and duration (Spec §5.1.5)
    std::ostringstream ss_summary;
    if (build_ok) {
        if (total_tasks == 0 && total_links == 0) {
            ss_summary << "✓ Up to date · " << std::fixed << std::setprecision(2) << duration << "s";
        } else {
            ss_summary << "✓ Build succeeded · " << total_tasks << " compiled, "
                       << up_to_date_count << " up to date · "
                       << std::fixed << std::setprecision(2) << duration << "s";
        }
    } else {
        ss_summary << "✗ Build failed · exit " << final_exit_code << " · "
                   << std::fixed << std::setprecision(2) << duration << "s";
    }

    std::string summary_line = ss_summary.str();
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        OutLine blank;
        blank.kind = OutLineKind::Raw;
        blank.text = "";
        m_output_queue.push_back(blank);

        OutLine sum;
        sum.kind = OutLineKind::Status;
        sum.text = summary_line;
        sum.has_error = !build_ok;
        m_output_queue.push_back(sum);
    }
    all_logged_lines.push_back("");
    all_logged_lines.push_back(summary_line);

    // Save full build log to .geniebuild/last_build.log
    std::ofstream log_file(log_path);
    if (log_file.is_open()) {
        for (const auto& l : all_logged_lines) {
            log_file << l << "\n";
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_last_result.exit_code = build_ok ? 0 : final_exit_code;
        m_last_result.duration_seconds = duration;
        m_last_result.success = build_ok;
    }

    m_status.store(BuildStatus::Finished);
}

} // namespace core
