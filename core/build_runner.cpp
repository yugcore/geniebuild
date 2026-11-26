#include "build_runner.h"
#include "settings_resolver.h"
#include "condition.h"
#include "build_steps.h"
#include "dep_log.h"
#include "tool_registry.h"
#include "toolchain.h"
#include "msvc_env.h"
#include "pch.h"
#include "unity.h"
#include "compiler_cache.h"
#include "scheduler.h"
#include "child_process.h"
#include "fs_util.h"
#include "compdb.h"
#include "build_log.h"
#include "hints.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <condition_variable>
#include <deque>
#include <queue>

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
        std::filesystem::path safe = fs_util::ComputeSafeObjectPath(obj_dir, rel_obj, rel.generic_u8string());
        std::error_code rec;
        std::filesystem::path safe_rel = std::filesystem::relative(safe, obj_dir, rec);
        out_obj_key = (!rec && !safe_rel.empty()) ? safe_rel.generic_u8string() : rel_obj;
        return safe;
    } else {
        std::string fname = src_norm.filename().generic_u8string();
        if (fname.empty()) fname = "source";
        if (fname.size() > 80) fname = fname.substr(0, 80);
        std::string hash_input = src_norm.generic_u8string();
        if (hash_input.empty()) hash_input = file_rel;
        rel_obj = "ext/" + Hash8Fnv1a(hash_input) + "_" + fname + obj_ext;
    }

    out_obj_key = rel_obj;
    return fs_util::ComputeSafeObjectPath(obj_dir, rel_obj, file_rel);
}

static std::string ComputeFileContentHash(const std::filesystem::path& p) {
    std::ifstream f(fs_util::ToWinLongPath(p), std::ios::binary);
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
            std::filesystem::path p = fs_util::ToWinLongPath(project.ResolvePath(raw_lib));
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
                std::filesystem::path p = fs_util::ToWinLongPath(dir / cand);
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

// Helper: executes an external process via ChildProcess (Job Object / PGID) and captures stdout+stderr
static int ExecuteProcess(
    const BuildCommand& cmd,
    const std::filesystem::path& working_dir,
    std::vector<OutLine>& out_lines,
    const std::unordered_map<std::string, std::string>* extra_env = nullptr,
    bool low_priority = false,
    std::mutex* procs_mutex = nullptr,
    std::unordered_set<ChildProcess*>* active_procs = nullptr,
    std::atomic<bool>* cancel_flag = nullptr
) {
    if (cancel_flag && cancel_flag->load()) {
        return -1;
    }

    ChildProcess process;
    ProcessOptions options;
    options.workingDir = working_dir;
    options.extraEnv = extra_env;
    options.pipeStdin = false;
    options.belowNormalPriority = low_priority;

    std::string start_err;
    if (!process.Start(cmd, options, &start_err)) {
        OutLine ol;
        ol.kind = OutLineKind::Raw;
        ol.text = start_err.empty() ? ("Failed to launch process '" + cmd.program + "'") : start_err;
        ol.has_error = true;
        out_lines.push_back(std::move(ol));
        return -1;
    }

    if (procs_mutex && active_procs) {
        std::lock_guard<std::mutex> lk(*procs_mutex);
        active_procs->insert(&process);
        if (cancel_flag && cancel_flag->load()) {
            process.TerminateTree();
        }
    }

    uint8_t buffer[4096];
    std::string accumulator;

    while (true) {
        bool eof = false;
        std::ptrdiff_t bytes_read = process.ReadStdout(buffer, sizeof(buffer), eof);
        if (bytes_read > 0) {
            accumulator.append(reinterpret_cast<char*>(buffer), static_cast<size_t>(bytes_read));

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
        if (eof || bytes_read <= 0) {
            break;
        }
    }

    if (!accumulator.empty()) {
        if (accumulator.back() == '\r') {
            accumulator.pop_back();
        }
        out_lines.push_back(MakeClassifiedRawLine(std::move(accumulator)));
    }

    int status = process.WaitExit(-1);
    if (procs_mutex && active_procs) {
        std::lock_guard<std::mutex> lk(*procs_mutex);
        active_procs->erase(&process);
    }
    return status;
}

// Parses gcc/clang Makefile dependency file (.d)
static std::vector<std::string> ParseMakefileDeps(const std::filesystem::path& dep_path) {
    std::vector<std::string> deps;
    std::ifstream file(fs_util::ToWinLongPath(dep_path));
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
        std::ifstream file(fs_util::ToWinLongPath(json_path));
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

BuildRunner::BuildRunner() : m_build_log(std::make_shared<BuildLog>()) {}

BuildRunner::~BuildRunner() {
    CancelBuild();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void BuildRunner::CancelBuild() {
    if (!IsRunning()) return;
    m_cancel_requested.store(true);
    {
        std::lock_guard<std::mutex> lk(m_active_procs_mutex);
        if (m_worker_abort_flag) {
            m_worker_abort_flag->store(true);
        }
        for (ChildProcess* p : m_active_procs) {
            if (p) p->TerminateTree();
        }
        if (m_worker_dag_cv && m_worker_dag_mutex) {
            std::lock_guard<std::mutex> dlk(*m_worker_dag_mutex);
            m_worker_dag_cv->notify_all();
        }
    }
}

BuildProgress BuildRunner::GetProgress() const {
    BuildProgress p;
    p.completedJobs = m_prog_completed.load();
    p.totalJobs = m_prog_total.load();
    p.compiledCount = m_prog_compiled.load();
    p.upToDateCount = m_prog_uptodate.load();
    p.failedCount = m_prog_failed.load();
    p.elapsedSeconds = GetElapsedSeconds();

    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_progress_mutex);
        p.activeJobs.reserve(m_running_slots.size());
        for (const auto& [sid, slot] : m_running_slots) {
            (void)sid;
            ActiveJobInfo info;
            info.label = slot.label;
            info.elapsedSeconds = std::chrono::duration<double>(now - slot.startTime).count();
            p.activeJobs.push_back(std::move(info));
        }
        std::sort(p.activeJobs.begin(), p.activeJobs.end(), [](const ActiveJobInfo& a, const ActiveJobInfo& b) {
            return a.elapsedSeconds > b.elapsedSeconds;
        });

        int64_t rem_ms = std::max<int64_t>(0, m_remaining_est_ms.load());
        int par = std::max(1, m_active_workers.load());
        if (p.totalJobs > 0 && p.completedJobs < p.totalJobs) {
            double raw_eta = (static_cast<double>(rem_ms) / 1000.0) / static_cast<double>(par);
            // Blend observed throughput after progress > 5% when dep-log history is uniform/new
            if (p.completedJobs >= 2 && p.elapsedSeconds > 0.25) {
                double rate_eta = (static_cast<double>(p.totalJobs - p.completedJobs) / static_cast<double>(p.completedJobs)) * p.elapsedSeconds;
                raw_eta = (raw_eta > 0.1) ? (0.55 * raw_eta + 0.45 * rate_eta) : rate_eta;
            }
            if (m_smoothed_eta_s < 0.0) {
                m_smoothed_eta_s = raw_eta;
            } else {
                m_smoothed_eta_s = 0.25 * raw_eta + 0.75 * m_smoothed_eta_s;
            }
            p.etaSeconds = m_smoothed_eta_s;
        } else {
            p.etaSeconds = 0.0;
        }
    }
    return p;
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

BuildCacheSummary BuildRunner::GetLastCacheSummary() const {
    std::lock_guard<std::mutex> lock(m_result_mutex);
    return m_last_result.cache_summary;
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
    const std::string cfg_prefix = SanitizeConfigDirName(config);
    std::filesystem::path obj_root = project.GetRootPath() / ".geniebuild" / "obj";
    if (!targets.empty()) {
        for (const auto& t : targets) {
            if (!target_name.empty() && t.name != target_name) continue;
            // Remove both legacy obj/<Config>/<target> and V2-14 toolchain-hashed obj/<Config>-<tc8>/<target>
            if (std::filesystem::exists(obj_root, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(obj_root, ec)) {
                    if (!entry.is_directory(ec)) continue;
                    std::string dname = entry.path().filename().generic_u8string();
                    if (dname == cfg_prefix || dname.rfind(cfg_prefix + "-", 0) == 0) {
                        std::filesystem::path sub_obj = entry.path() / t.name;
                        if (std::filesystem::exists(sub_obj, ec)) {
                            removed += std::filesystem::remove_all(sub_obj, ec);
                        }
                    }
                }
            }
            std::filesystem::path gen_dir = t.GetGenDir(project.GetRootPath(), config);
            if (std::filesystem::exists(gen_dir, ec)) {
                removed += std::filesystem::remove_all(gen_dir, ec);
            }
            std::filesystem::path pch_dir = project.GetRootPath() / ".geniebuild" / "pch" / SanitizeConfigDirName(config) / t.name;
            if (std::filesystem::exists(pch_dir, ec)) {
                removed += std::filesystem::remove_all(pch_dir, ec);
            }
            std::filesystem::path unity_dir = project.GetRootPath() / ".geniebuild" / "unity" / SanitizeConfigDirName(config) / t.name;
            if (std::filesystem::exists(unity_dir, ec)) {
                removed += std::filesystem::remove_all(unity_dir, ec);
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

    // Validate project-level conditional libraries (V2-5)
    for (const auto& [lib_name, cond_expr] : project.GetLibConditions()) {
        std::string cerr;
        if (!cond_expr.empty() && !ValidateCondition(cond_expr, cerr)) {
            out_error = "Project library '" + lib_name + "' has invalid condition '" + cond_expr + "': " + cerr;
            return false;
        }
    }

    // Resolve active toolchain profile (V2-14 & V2-17)
    ToolchainResolution tc_res = compiler_manager.ResolveProjectToolchain(project);
    const ToolchainProfile* active_tc = tc_res.found ? &tc_res.profile : nullptr;

    // Resolve target dependency graph & check for cycles / missing deps / duplicate outputs / V2-5 conditions
    ConditionContext cond_ctx = BuildConditionContext(project, active_tc);
    std::vector<Target> targets_snapshot = project.GetTargetsSnapshot();
    std::vector<const Target*> build_order;
    if (!ResolveTargetBuildOrder(targets_snapshot, target_name, build_order, out_error, &cond_ctx)) {
        return false;
    }

    // V2-6 Security: check folder trust when project contains custom build steps
    const size_t custom_step_count = TrustStore::CountCustomCommands(project);
    if (custom_step_count > 0 && !TrustStore::IsTrusted(project.GetRootPath())) {
        out_error = "Cannot build: This project runs " + std::to_string(custom_step_count) +
                    " custom command(s) during builds and is not trusted.\n"
                    "Trust this folder in the Trust prompt (or pass --trust in CLI) to build.";
        return false;
    }

    // V2-6 Pre-flight: expand steps to count generated sources, check missing step tools
    std::string comp_lower = ToLower(project.GetCompiler());
    const bool is_msvc = active_tc ? active_tc->IsMsvcSyntax() : (comp_lower == "cl" || comp_lower == "cl.exe");
    const bool is_clang = active_tc ? active_tc->IsClangDriver() : (comp_lower.find("clang") != std::string::npos);
    const std::string config = project.GetConfiguration();
    const std::filesystem::path abs_out = project.GetAbsoluteOutputDir();
    std::unordered_map<std::string, std::string> target_bin_map;
    std::vector<std::string> all_planned_outputs;
    for (const auto& t : targets_snapshot) {
        std::filesystem::path bp = t.GetBinaryPath(abs_out, config, is_msvc);
        if (!bp.empty()) {
            std::string bstr = bp.generic_u8string();
            target_bin_map[t.name] = bstr;
            all_planned_outputs.push_back(bstr);
            std::error_code rel_ec;
            std::filesystem::path rel = std::filesystem::relative(bp, project.GetRootPath(), rel_ec);
            if (!rel_ec) {
                std::string rs = rel.generic_u8string();
                std::replace(rs.begin(), rs.end(), '\\', '/');
                if (rs.rfind("..", 0) != 0) {
                    target_bin_map[t.name] = rs;
                    all_planned_outputs.push_back(rs);
                }
            }
        }
    }

    size_t total_active_steps = 0;
    size_t total_generated_sources = 0;
    std::vector<ExpandedStepJob> preflight_step_jobs;
    for (const Target* t : build_order) {
        std::vector<std::string> virtual_outs;
        for (size_t si = 0; si < t->steps.size(); ++si) {
            const auto& st = t->steps[si];
            if (!EvaluateCondition(st.condition, cond_ctx).active) continue;
            total_active_steps++;
            auto jobs = ExpandStepJobs(project, *t, si, st, project.GetDirectoryCache(), target_bin_map, &virtual_outs, nullptr, active_tc);
            for (auto& j : jobs) {
                for (const auto& o : j.outputs) {
                    virtual_outs.push_back(o);
                    all_planned_outputs.push_back(o);
                    if (st.phase == StepPhase::PreBuild && st.generatesSources && IsStepGeneratedSourcePath(o)) {
                        total_generated_sources++;
                    }
                }
                preflight_step_jobs.push_back(std::move(j));
            }
        }
    }

    // Check missing step tools once per unique program name before starting the build (V2-6 edge case)
    std::unordered_map<std::string, bool> tool_check_cache;
    for (const auto& sj : preflight_step_jobs) {
        if (sj.shell || sj.cmd.program.empty()) continue;
        auto it = tool_check_cache.find(sj.cmd.program);
        bool avail = false;
        if (it != tool_check_cache.end()) {
            avail = it->second;
        } else {
            std::string resolved;
            avail = IsStepProgramAvailable(sj.cmd.program, project.GetRootPath(), all_planned_outputs, resolved);
            tool_check_cache[sj.cmd.program] = avail;
        }
        if (!avail) {
            out_error = "Cannot build target '" + sj.targetName + "': Step '" + sj.stepName +
                        "' needs program '" + sj.cmd.program + "', which was not found on PATH or in project.";
            return false;
        }
    }

    // Guard 1: At least one non-interface target must have compiled source files (or generated sources / steps)
    size_t total_sources = 0;
    size_t total_enabled = 0;
    size_t missing_count = 0;
    for (const Target* t : build_order) {
        if (t->kind == TargetKind::Interface) continue;
        total_sources += t->expandedFiles.size();
        total_enabled += t->GetEnabledFilesCount();
        missing_count += t->missingFiles.size();
    }

    if (total_sources + total_generated_sources == 0 && total_active_steps == 0) {
        out_error = "Cannot build: No source files in the build target(s).\nPlease add source files or glob rules.";
        return false;
    }
    if (total_sources > 0 && total_enabled + total_generated_sources == 0 && total_active_steps == 0) {
        out_error = "Cannot build: No source files are enabled for compilation.\nPlease check at least one file in the Targets pane.";
        return false;
    }

    // Guard 2: Missing files guard
    if (missing_count > 0) {
        out_error = "Cannot build: " + std::to_string(missing_count) +
                    " file(s) in the build list are missing on disk.\nPlease remove missing files to proceed.";
        return false;
    }

    // Guard 3: Missing compiler / toolchain guard
    if (!tc_res.found) {
        out_error = "Cannot build: Selected compiler '" + project.GetCompiler() + "' was not found on PATH.\nPlease install a supported compiler or choose an available one.";
        return false;
    }

    // Guard 4: MSVC Environment guard (skipped when V2-17 captured VS environment is attached)
    const bool has_captured_msvc_env = active_tc && active_tc->env.count("INCLUDE") && active_tc->env.count("LIB");
    if (is_msvc && !has_captured_msvc_env) {
        if (!CompilerManager::CheckMsvcEnvironment(out_error)) {
            return false;
        }
    }

    // Guard 4b: V2-12 Linker options & availability check
    if (!CompilerManager::ValidateLinkerOptions(project, is_msvc, is_clang, out_error)) {
        return false;
    }

    // Guard 5: Missing library directory guard across effective target settings & V2-7 tool guard
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
            std::filesystem::path resolved_lp = fs_util::ToWinLongPath(resolved);
            if (!std::filesystem::exists(resolved_lp, ec) || !std::filesystem::is_directory(resolved_lp, ec)) {
                out_error = "Cannot build target '" + t->name + "': Library search directory not found:\n" + lib_dir +
                            "\nResolved: " + resolved.generic_u8string() + "\nPlease check Settings → Libraries.";
                return false;
            }
        }
        if (t->kind != TargetKind::Interface) {
            std::filesystem::path obj_dir = t->GetObjectDir(project.GetRootPath(), config, active_tc ? active_tc->idHash8 : "");
            std::filesystem::path gen_dir = t->GetGenDir(project.GetRootPath(), config);
            std::string comp_prog = active_tc && !active_tc->cxx.empty() ? active_tc->cxx : project.GetCompiler();
            for (const auto& f : t->GetCompiledFiles()) {
                SourceFileKind fk = ToolRegistry::ClassifySourceFile(f, project.GetCustomTools());
                if (fk != SourceFileKind::C && fk != SourceFileKind::Cxx) {
                    ToolCompileSpec tspec = ToolRegistry::FormulateExtraSourceCommand(
                        f, *t, eff, project, obj_dir, gen_dir,
                        comp_prog, is_msvc,
                        project.GetToolOverrides(), project.GetCustomTools(), project.GetAsmMode()
                    );
                    if (!tspec.supported) {
                        out_error = "Cannot build target '" + t->name + "': " + tspec.errorMessage;
                        return false;
                    }
                }
            }
        }
    }

    // Prepare log file path (.geniebuild/last_build.log)
    std::filesystem::path gb_dir = project.GetRootPath() / ".geniebuild";
    fs_util::CreateDirsLong(gb_dir, ec);
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
    m_cancel_requested.store(false);
    m_prog_completed.store(0);
    m_prog_total.store(0);
    m_prog_compiled.store(0);
    m_prog_uptodate.store(0);
    m_prog_failed.store(0);
    m_remaining_est_ms.store(0);
    {
        std::lock_guard<std::mutex> lk(m_progress_mutex);
        m_running_slots.clear();
        m_smoothed_eta_s = -1.0;
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
    const std::string config = project.GetConfiguration();
    if (!m_build_log) m_build_log = std::make_shared<BuildLog>();
    m_build_log->BeginSession(project.GetRootPath(), config);
    auto append_log = [&](const std::vector<OutLine>& lines) {
        if (m_build_log && !lines.empty()) {
            m_build_log->AppendLines(lines);
        }
    };
    ToolchainResolution tc_res = compiler_manager.ResolveProjectToolchain(project);
    const ToolchainProfile* active_tc = tc_res.found ? &tc_res.profile : nullptr;
    const std::string tc_hash8 = active_tc ? active_tc->idHash8 : "";
    const CompilerInfo active_cxx_info = active_tc
        ? compiler_manager.MakeCompilerInfoFromProfile(*active_tc, false)
        : CompilerInfo{};
    const CompilerInfo active_c_info = active_tc
        ? compiler_manager.MakeCompilerInfoFromProfile(*active_tc, true)
        : CompilerInfo{};
    const CompilerInfo* cxx_comp = active_tc ? &active_cxx_info : compiler_manager.FindCompiler(project.GetCompiler());
    const CompilerInfo* c_comp = active_tc ? &active_c_info : compiler_manager.FindCompiler(project.GetCCompiler());
    if (!c_comp) c_comp = cxx_comp;

    const std::unordered_map<std::string, std::string> tc_base_env = active_tc
        ? MsvcEnvDetector::BuildMergedProcessEnv(active_tc->env)
        : std::unordered_map<std::string, std::string>{};
    auto merge_with_tc_env = [&](const std::unordered_map<std::string, std::string>& extra) {
        std::unordered_map<std::string, std::string> merged = tc_base_env;
        for (const auto& [k, v] : extra) merged[k] = v;
        return merged;
    };

    std::string comp_lower = ToLower(project.GetCompiler());
    const bool is_msvc = active_tc ? active_tc->IsMsvcSyntax() : (comp_lower == "cl" || comp_lower == "cl.exe");
    const bool is_clang = active_tc ? active_tc->IsClangDriver() : (comp_lower.find("clang") != std::string::npos);
    const bool keep_going = project.GetKeepGoing();
    const bool low_priority = project.GetLowPriority();
    const int max_link_jobs = std::max(1, project.GetLinkJobs());
    const bool ignore_sys_headers = project.GetIgnoreSystemHeaders();
    const bool keep_dep_files = project.GetKeepDepFiles();
    const std::filesystem::path abs_out = project.GetAbsoluteOutputDir();

    // V2-10 Compiler cache detection & pre-build stats (profile launcher overrides cache prefs when set)
    CompilerCachePrefs cache_prefs = CompilerCacheManager::LoadPrefs();
    if (active_tc && !active_tc->launcher.empty() && active_tc->launcher != "auto") {
        cache_prefs.launcher = active_tc->launcher;
    }
    DetectedCompilerCache compiler_cache = CompilerCacheManager::DetectCache(cache_prefs, is_msvc, false);
    CacheStatsSnapshot cache_before;
    if (compiler_cache.active) {
        cache_before = CompilerCacheManager::QueryStats(compiler_cache);
    }

    // V2-13 Compute RAM-aware worker budget
    WorkerBudget budget = Scheduler::ComputeWorkerBudget(project.GetJobs(), project.GetMemPerJobMb());
    m_active_workers.store(budget.workers);
    m_ram_capped.store(budget.ramCapped);
    m_throttled_low_memory.store(false);

    std::string resolve_err;
    ConditionContext cond_ctx = BuildConditionContext(project, active_tc);
    std::vector<const Target*> build_order;
    if (!ResolveTargetBuildOrder(targets_snapshot, target_filter, build_order, resolve_err, &cond_ctx)) {
        OutLine err_ol;
        err_ol.kind = OutLineKind::Status;
        err_ol.text = "✗ " + resolve_err;
        err_ol.has_error = true;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_output_queue.push_back(err_ol);
        }
        append_log({ err_ol });
        if (m_build_log) m_build_log->EndSession();
        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            m_last_result = BuildResult{};
            m_last_result.exit_code = 1;
            m_last_result.success = false;
            m_last_result.active_worker_count = budget.workers;
            m_last_result.ram_capped = budget.ramCapped;
        }
        m_status.store(BuildStatus::Finished);
        return;
    }

    std::unordered_map<std::string, const Target*> target_map;
    for (const auto& t : targets_snapshot) target_map[t.name] = &t;

    std::unordered_map<std::string, size_t> plan_idx_by_name;
    for (size_t i = 0; i < build_order.size(); ++i) {
        plan_idx_by_name[build_order[i]->name] = i;
    }

    const bool multi_target_output = (build_order.size() > 1);
    std::error_code ec;

    // V2-11 / V2-14 Open binary dependency log (.geniebuild/obj/<Config>-<tc8>/deps.bin)
    std::vector<std::filesystem::path> legacy_states;
    for (const Target* t : build_order) {
        legacy_states.push_back(t->GetObjectDir(project.GetRootPath(), config, tc_hash8) / "state.json");
    }
    std::string dep_cfg_seg = SanitizeConfigDirName(config) + (tc_hash8.empty() ? "" : ("-" + tc_hash8));
    std::filesystem::path deps_bin_path = project.GetRootPath() / ".geniebuild" / "obj" / dep_cfg_seg / "deps.bin";
    DepLog dep_log;
    dep_log.Open(deps_bin_path, legacy_states);

    struct TargetBuildPlan {
        const Target* target = nullptr;
        EffectiveTargetSettings eff;
        std::filesystem::path obj_dir;
        std::filesystem::path gen_dir;
        std::filesystem::path state_path;
        std::unordered_map<std::string, std::string> prev_state;
        std::unordered_map<std::string, std::string> current_state;
        std::vector<std::string> generated_sources;
        std::vector<std::filesystem::path> object_paths;
        TargetPchAnalysis pch_analysis;
        UnityPlan unity_plan;
        size_t compiled_task_count = 0;
        bool failed = false;
        std::mutex state_mutex;
    };

    std::vector<TargetBuildPlan> plans(build_order.size());

    auto load_state_map = [](const std::filesystem::path& state_path) {
        std::unordered_map<std::string, std::string> m;
        std::ifstream state_file(fs_util::ToWinLongPath(state_path));
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
            std::ofstream sf(fs_util::ToWinLongPath(state_path));
            if (sf.is_open()) {
                sf << j_state.dump(2) << std::endl;
            }
        } catch (...) {}
    };

    // Build target binary map for {bin} / {bin:target} placeholders
    std::unordered_map<std::string, std::string> target_bin_map;
    for (const auto& t : targets_snapshot) {
        std::filesystem::path bp = t.GetBinaryPath(abs_out, config, is_msvc);
        if (!bp.empty()) {
            std::error_code rel_ec;
            std::filesystem::path rel = std::filesystem::relative(bp, project.GetRootPath(), rel_ec);
            if (!rel_ec) {
                std::string rs = rel.generic_u8string();
                std::replace(rs.begin(), rs.end(), '\\', '/');
                if (rs.rfind("..", 0) != 0) {
                    target_bin_map[t.name] = rs;
                    continue;
                }
            }
            target_bin_map[t.name] = bp.generic_u8string();
        }
    }

    DirectoryCache step_dir_cache;
    std::vector<std::string> expand_warnings;
    if (tc_res.isFallback && !tc_res.bannerMessage.empty()) {
        expand_warnings.push_back(tc_res.bannerMessage);
    }

    // Initialize TargetBuildPlans and collect active step jobs per target
    std::vector<std::vector<ExpandedStepJob>> target_pre_jobs(build_order.size());
    std::vector<std::vector<ExpandedStepJob>> target_post_jobs(build_order.size());

    bool any_pch_used = false;
    for (size_t pi = 0; pi < build_order.size(); ++pi) {
        const Target* t = build_order[pi];
        TargetBuildPlan& plan = plans[pi];
        plan.target = t;

        bool needs_pic = (t->kind == TargetKind::Static) && IsStaticTargetConsumedByShared(targets_snapshot, t->name);
        plan.eff = ResolveEffectiveTargetSettings(project, *t, target_map, config, is_msvc, needs_pic);

        if (t->freestanding && active_tc && ToLower(active_tc->os) == "windows") {
            expand_warnings.push_back(
                "Target '" + t->name + "' is freestanding, but toolchain '" + active_tc->name +
                "' targets Windows PE. Kernels usually need an ELF cross compiler (e.g. x86_64-elf-gcc)."
            );
        }

        if (!t->pch.IsEmpty()) any_pch_used = true;

        plan.obj_dir = t->GetObjectDir(project.GetRootPath(), config, tc_hash8);
        plan.gen_dir = t->GetGenDir(project.GetRootPath(), config);
        fs_util::CreateDirsLong(plan.obj_dir, ec);
        plan.state_path = plan.obj_dir / "state.json";
        plan.prev_state = load_state_map(plan.state_path);
        plan.current_state = plan.prev_state;

        // V2-10 MSVC /Zi warning if compiler cache is active
        if (compiler_cache.active && is_msvc) {
            bool has_zi = false;
            for (const auto& fl : plan.eff.compileFlags) {
                if (fl.find("/Zi") != std::string::npos || fl.find("/ZI") != std::string::npos) {
                    has_zi = true;
                    break;
                }
            }
            if (has_zi) {
                expand_warnings.push_back("Target '" + t->name + "' passes /Zi; ccache/sccache on MSVC requires /Z7 for caching.");
            }
        }

        std::vector<std::string> virtual_outputs;
        for (size_t si = 0; si < t->steps.size(); ++si) {
            const auto& st = t->steps[si];
            if (!EvaluateCondition(st.condition, cond_ctx).active) continue;
            auto jobs = ExpandStepJobs(project, *t, si, st, step_dir_cache, target_bin_map, &virtual_outputs, &expand_warnings, active_tc);
            for (auto& j : jobs) {
                for (const auto& o : j.outputs) {
                    virtual_outputs.push_back(o);
                    if (st.phase == StepPhase::PreBuild && st.generatesSources && IsStepGeneratedSourcePath(o)) {
                        if (std::find(plan.generated_sources.begin(), plan.generated_sources.end(), o) == plan.generated_sources.end()) {
                            plan.generated_sources.push_back(o);
                        }
                    }
                }
                if (st.phase == StepPhase::PreBuild) {
                    target_pre_jobs[pi].push_back(std::move(j));
                } else {
                    target_post_jobs[pi].push_back(std::move(j));
                }
            }
        }
    }

    for (const auto& wmsg : expand_warnings) {
        OutLine warn_ol;
        warn_ol.kind = OutLineKind::Raw;
        warn_ol.text = "> Warning: " + wmsg;
        warn_ol.has_warning = true;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_output_queue.push_back(warn_ol);
        }
        append_log({ warn_ol });
    }

    // Build unified DAG nodes:
    //   StepJob (PreBuild), PrepareCompile, PchBuild (dynamic), Compile (dynamic), TargetLink, StepJob (PostBuild)
    enum class DagKind {
        StepJob,
        PrepareCompile,
        PchBuild,
        Compile,
        TargetLink
    };

    struct DagNode {
        DagKind kind = DagKind::StepJob;
        size_t plan_idx = 0;
        ExpandedStepJob step_job;
        PchArtifactPlan pch_plan;

        // Compile payload
        std::string target_name;
        std::string src_rel;
        std::string progress_label;
        std::filesystem::path obj_path;
        std::string obj_key;
        BuildCommand cmd;
        std::string cmd_hash;
        bool is_unity = false;
        bool is_c_compile = false;
        bool uses_pch = false;
        bool ccache_eligible = false;
        bool uses_depfile = false;
        bool uses_rc_hash = false;
        std::filesystem::path depfile_path;
        std::filesystem::path pch_dep_output;
        std::vector<std::string> unity_members;

        uint32_t priority_ms = 10;
        std::atomic<int> pending_deps{0};
        std::vector<size_t> dependents;
        bool finished = false;
        bool skipped = false;
    };

    std::vector<std::unique_ptr<DagNode>> nodes;
    auto add_node = [&](DagKind k, size_t pi) -> size_t {
        auto n = std::make_unique<DagNode>();
        n->kind = k;
        n->plan_idx = pi;
        nodes.push_back(std::move(n));
        return nodes.size() - 1;
    };
    auto add_edge = [&](size_t from_id, size_t to_id) {
        if (from_id == to_id) return;
        auto& deps = nodes[from_id]->dependents;
        if (std::find(deps.begin(), deps.end(), to_id) == deps.end()) {
            deps.push_back(to_id);
            nodes[to_id]->pending_deps.fetch_add(1);
        }
    };

    std::vector<size_t> prep_compile_nodes(build_order.size(), static_cast<size_t>(-1));
    std::vector<size_t> link_nodes(build_order.size(), static_cast<size_t>(-1));
    std::vector<std::vector<size_t>> pre_step_node_ids(build_order.size());
    std::vector<std::vector<size_t>> post_step_node_ids(build_order.size());

    std::unordered_map<std::string, size_t> output_producer_node;

    for (size_t pi = 0; pi < build_order.size(); ++pi) {
        prep_compile_nodes[pi] = add_node(DagKind::PrepareCompile, pi);
        nodes[prep_compile_nodes[pi]]->priority_ms = 1000000;
        link_nodes[pi] = add_node(DagKind::TargetLink, pi);
        nodes[link_nodes[pi]]->priority_ms = 1000;
        add_edge(prep_compile_nodes[pi], link_nodes[pi]);

        size_t last_step_idx = static_cast<size_t>(-1);
        std::vector<size_t> prev_step_group;
        std::vector<size_t> cur_step_group;
        for (auto& sj : target_pre_jobs[pi]) {
            if (sj.stepIndex != last_step_idx) {
                if (!cur_step_group.empty()) {
                    prev_step_group = std::move(cur_step_group);
                    cur_step_group.clear();
                }
                last_step_idx = sj.stepIndex;
            }
            size_t nid = add_node(DagKind::StepJob, pi);
            nodes[nid]->step_job = std::move(sj);
            nodes[nid]->priority_ms = 500000;
            for (size_t prev_id : prev_step_group) add_edge(prev_id, nid);
            for (const auto& o : nodes[nid]->step_job.outputs) {
                std::string no = o;
                std::replace(no.begin(), no.end(), '\\', '/');
                output_producer_node[no] = nid;
            }
            cur_step_group.push_back(nid);
            pre_step_node_ids[pi].push_back(nid);
        }

        last_step_idx = static_cast<size_t>(-1);
        prev_step_group.clear();
        cur_step_group.clear();
        for (auto& sj : target_post_jobs[pi]) {
            if (sj.stepIndex != last_step_idx) {
                if (!cur_step_group.empty()) {
                    prev_step_group = std::move(cur_step_group);
                    cur_step_group.clear();
                }
                last_step_idx = sj.stepIndex;
            }
            size_t nid = add_node(DagKind::StepJob, pi);
            nodes[nid]->step_job = std::move(sj);
            nodes[nid]->priority_ms = 500;
            add_edge(link_nodes[pi], nid);
            for (size_t prev_id : prev_step_group) add_edge(prev_id, nid);
            for (const auto& o : nodes[nid]->step_job.outputs) {
                std::string no = o;
                std::replace(no.begin(), no.end(), '\\', '/');
                output_producer_node[no] = nid;
            }
            cur_step_group.push_back(nid);
            post_step_node_ids[pi].push_back(nid);
        }
    }

    for (size_t pi = 0; pi < build_order.size(); ++pi) {
        const Target* t = build_order[pi];

        std::unordered_set<size_t> trans_dep_indices;
        std::vector<const Target*> stack = { t };
        std::unordered_set<std::string> vis;
        while (!stack.empty()) {
            const Target* cur = stack.back();
            stack.pop_back();
            if (!vis.insert(cur->name).second) continue;
            auto it_idx = plan_idx_by_name.find(cur->name);
            if (it_idx != plan_idx_by_name.end()) trans_dep_indices.insert(it_idx->second);
            for (const auto& d : cur->deps) {
                if (auto dit = target_map.find(d); dit != target_map.end()) stack.push_back(dit->second);
            }
            for (const auto& pd : cur->publicDeps) {
                if (auto dit = target_map.find(pd); dit != target_map.end()) stack.push_back(dit->second);
            }
        }

        for (size_t dpi : trans_dep_indices) {
            for (size_t pre_id : pre_step_node_ids[dpi]) {
                add_edge(pre_id, prep_compile_nodes[pi]);
            }
        }

        auto wire_direct_dep_links = [&](const std::vector<std::string>& dep_list) {
            for (const auto& dname : dep_list) {
                auto it = plan_idx_by_name.find(dname);
                if (it != plan_idx_by_name.end()) {
                    add_edge(link_nodes[it->second], link_nodes[pi]);
                }
            }
        };
        wire_direct_dep_links(t->deps);
        wire_direct_dep_links(t->publicDeps);
    }

    const size_t initial_node_count = nodes.size();
    for (size_t nid = 0; nid < initial_node_count; ++nid) {
        if (nodes[nid]->kind != DagKind::StepJob) continue;
        const auto& sj = nodes[nid]->step_job;
        for (const auto& inp : sj.inputs) {
            std::string nin = inp;
            std::replace(nin.begin(), nin.end(), '\\', '/');
            auto pit = output_producer_node.find(nin);
            if (pit != output_producer_node.end() && pit->second != nid) {
                add_edge(pit->second, nid);
            }
        }
        for (const auto& ref_t : sj.referencedTargets) {
            auto lit = plan_idx_by_name.find(ref_t);
            if (lit != plan_idx_by_name.end()) {
                add_edge(link_nodes[lit->second], nid);
            }
        }
    }

    // Execution state across worker pool
    std::atomic<bool> abort_all{false};
    std::atomic<int> first_exit_code{0};
    std::atomic<size_t> total_compile_tasks{0};
    std::atomic<size_t> completed_compile_tasks{0};
    std::atomic<size_t> up_to_date_count{0};
    std::atomic<size_t> total_links{0};
    std::atomic<size_t> total_steps_run{0};
    std::atomic<size_t> total_steps_skipped{0};
    std::atomic<size_t> total_pchs_built{0};
    std::atomic<size_t> checked_files_total{0};
    std::atomic<size_t> failed_files_count{0};
    std::atomic<size_t> skipped_links_count{0};
    std::atomic<size_t> built_targets_count{0};
    std::atomic<size_t> remaining_nodes{initial_node_count};
    std::atomic<int> active_jobs{0};
    std::atomic<int> active_link_jobs{0};

    std::mutex dag_mutex;
    std::condition_variable dag_cv;
    auto pq_cmp = [&](size_t a, size_t b) {
        return nodes[a]->priority_ms < nodes[b]->priority_ms;
    };
    std::priority_queue<size_t, std::vector<size_t>, decltype(pq_cmp)> ready_queue(pq_cmp);
    std::deque<size_t> ready_link_queue;

    std::mutex link_track_mutex;
    std::unordered_set<std::string> rebuilt_static_outputs;
    std::vector<std::filesystem::path> runtime_bin_dirs;
    std::vector<std::string> all_copy_dlls;

    std::mutex timing_mutex;
    std::vector<std::string> step_timing_order;
    std::unordered_map<std::string, double> step_timings;
    std::vector<std::pair<std::string, double>> link_timings;
    std::vector<SlowestFileInfo> compile_timings;
    double steps_total_time_s = 0.0;
    double pch_total_time_s = 0.0;
    double compile_total_time_s = 0.0;
    double link_total_time_s = 0.0;
    std::atomic<bool> mingw_long_warned{false};

    {
        std::lock_guard<std::mutex> aplk(m_active_procs_mutex);
        m_worker_abort_flag = &abort_all;
        m_worker_dag_cv = &dag_cv;
        m_worker_dag_mutex = &dag_mutex;
    }

    auto make_slot = [&](const std::string& label) -> uint64_t {
        uint64_t sid = m_next_slot_id.fetch_add(1);
        std::lock_guard<std::mutex> plk(m_progress_mutex);
        m_running_slots[sid] = ActiveJobSlot{ label, std::chrono::steady_clock::now() };
        return sid;
    };
    auto release_slot = [&](uint64_t sid) {
        std::lock_guard<std::mutex> plk(m_progress_mutex);
        m_running_slots.erase(sid);
    };
    struct SlotGuard {
        decltype(release_slot)& fn;
        uint64_t id;
        ~SlotGuard() { fn(id); }
    };

    std::mutex log_exec_mutex;
    bool first_cmd_recorded = false;

    for (size_t i = 0; i < initial_node_count; ++i) {
        if (nodes[i]->pending_deps.load() == 0) {
            if (nodes[i]->kind == DagKind::TargetLink) ready_link_queue.push_back(i);
            else ready_queue.push(i);
        }
    }

    int num_workers = budget.workers;
    if (num_workers <= 0) num_workers = 1;

    auto push_log_lines = [&](const std::vector<OutLine>& lines) {
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            for (const auto& l : lines) {
                m_output_queue.push_back(l);
            }
        }
        append_log(lines);
    };

    auto push_ready_locked = [&](size_t id) {
        if (nodes[id]->kind == DagKind::TargetLink) ready_link_queue.push_back(id);
        else ready_queue.push(id);
    };

    auto finish_node = [&](size_t nid) {
        std::lock_guard<std::mutex> lock(dag_mutex);
        if (!nodes[nid]->finished) {
            nodes[nid]->finished = true;
            remaining_nodes.fetch_sub(1);
            for (size_t dep_id : nodes[nid]->dependents) {
                if (!nodes[dep_id]->finished && nodes[dep_id]->pending_deps.fetch_sub(1) == 1) {
                    push_ready_locked(dep_id);
                }
            }
        }
        dag_cv.notify_all();
    };

    auto fail_node = [&](size_t nid) {
        if (!keep_going) {
            abort_all.store(true);
            std::lock_guard<std::mutex> lock(dag_mutex);
            dag_cv.notify_all();
            return;
        }
        std::lock_guard<std::mutex> lock(dag_mutex);
        std::deque<size_t> skip_q;
        if (!nodes[nid]->finished) {
            nodes[nid]->finished = true;
            nodes[nid]->skipped = true;
            remaining_nodes.fetch_sub(1);
            skip_q.push_back(nid);
        }
        while (!skip_q.empty()) {
            size_t cur = skip_q.front();
            skip_q.pop_front();
            for (size_t dep_id : nodes[cur]->dependents) {
                if (!nodes[dep_id]->finished) {
                    nodes[dep_id]->finished = true;
                    nodes[dep_id]->skipped = true;
                    remaining_nodes.fetch_sub(1);
                    if (nodes[dep_id]->kind == DagKind::TargetLink &&
                        plans[nodes[dep_id]->plan_idx].target->kind != TargetKind::Interface) {
                        skipped_links_count.fetch_add(1);
                    }
                    skip_q.push_back(dep_id);
                }
            }
        }
        dag_cv.notify_all();
    };

    // V2-13 Memory-pressure monitor thread (checks every 2s while active)
    std::atomic<bool> stop_mem_monitor{false};
    std::thread mem_monitor([&]() {
        while (!stop_mem_monitor.load()) {
            for (int i = 0; i < 20 && !stop_mem_monitor.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stop_mem_monitor.load()) break;
            bool low = Scheduler::IsSystemMemoryLow(0.10);
            bool prev = m_throttled_low_memory.exchange(low);
            if (prev && !low) {
                std::lock_guard<std::mutex> lock(dag_mutex);
                dag_cv.notify_all();
            }
        }
    });

    auto check_needs_rebuild_using_dep_log = [&](
        const std::string& obj_path_str,
        const std::string& src_path_str,
        const std::string& cmd_hash_str,
        const std::string& legacy_key,
        const std::unordered_map<std::string, int64_t>& stat_map,
        const std::filesystem::path& depfile_p,
        bool is_msvc_json_dep,
        bool uses_rc_hash,
        std::vector<std::string>* out_cached_deps
    ) -> bool {
        std::string norm_obj = DepLog::NormalizeDepPath(obj_path_str);
        auto obj_it = stat_map.find(norm_obj);
        if (obj_it == stat_map.end()) return true;
        int64_t disk_obj_mtime = obj_it->second;

        if (!src_path_str.empty()) {
            auto src_it = stat_map.find(DepLog::NormalizeDepPath(src_path_str));
            if (src_it == stat_map.end() || src_it->second > disk_obj_mtime) return true;
        }

        uint64_t cur_hash_u64 = DepLog::ParseHashU64(cmd_hash_str);
        DepOutputEntry rec;
        if (dep_log.TryGetOutput(obj_path_str, rec)) {
            if (rec.cmdHash != cur_hash_u64) return true;
            if (rec.mtimeNs != disk_obj_mtime) return true; // externally modified

            for (uint32_t did : rec.depIds) {
                std::string dp = dep_log.GetPath(did);
                if (dp.empty()) continue;
                auto dit = stat_map.find(dp);
                if (dit == stat_map.end()) return true; // dep missing on disk
                if (dit->second > rec.mtimeNs) return true;
            }
            if (out_cached_deps) {
                for (uint32_t did : rec.depIds) {
                    std::string dp = dep_log.GetPath(did);
                    if (!dp.empty()) out_cached_deps->push_back(std::move(dp));
                }
            }
            return false;
        }

        // Fallback to legacy state.json hash + .d/.json file on first deps.bin migration
        uint64_t leg_h = 0;
        if (dep_log.TryGetLegacyHash(legacy_key, leg_h) && leg_h == cur_hash_u64 && !uses_rc_hash) {
            if (fs_util::ExistsLong(depfile_p)) {
                std::vector<std::string> deps = is_msvc_json_dep ? ParseMsvcDeps(depfile_p) : ParseStepDepfile(depfile_p);
                for (const auto& d : deps) {
                    std::filesystem::path dp = project.ResolvePath(d);
                    int64_t dns = DepLog::StatFileNs(dp);
                    if (dns == 0 || dns > disk_obj_mtime) return true;
                }
                return false;
            }
        }
        return true;
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(num_workers));
    for (int w = 0; w < num_workers; ++w) {
        workers.emplace_back([&]() {
            while (true) {
                size_t nid = static_cast<size_t>(-1);
                bool is_link_dispatch = false;
                {
                    std::unique_lock<std::mutex> lock(dag_mutex);
                    dag_cv.wait(lock, [&]() {
                        if (abort_all.load() || remaining_nodes.load() == 0) return true;
                        // Throttle new dispatches if memory is critically low while at least 1 job runs
                        if (m_throttled_low_memory.load() && active_jobs.load() > 0) return false;
                        if (!ready_queue.empty()) return true;
                        if (!ready_link_queue.empty() && active_link_jobs.load() < max_link_jobs) return true;
                        return false;
                    });
                    if (abort_all.load() || (ready_queue.empty() && ready_link_queue.empty() && remaining_nodes.load() == 0)) {
                        break;
                    }
                    if (!ready_queue.empty()) {
                        nid = ready_queue.top();
                        ready_queue.pop();
                    } else if (!ready_link_queue.empty() && active_link_jobs.load() < max_link_jobs) {
                        nid = ready_link_queue.front();
                        ready_link_queue.pop_front();
                        is_link_dispatch = true;
                        active_link_jobs.fetch_add(1);
                    }
                }
                if (nid == static_cast<size_t>(-1)) continue;

                struct ActiveGuard {
                    std::atomic<int>& cnt;
                    bool is_link;
                    std::atomic<int>& link_cnt;
                    std::condition_variable& cv;
                    std::mutex& mtx;
                    ActiveGuard(std::atomic<int>& c, bool il, std::atomic<int>& lc, std::condition_variable& v, std::mutex& m)
                        : cnt(c), is_link(il), link_cnt(lc), cv(v), mtx(m) {
                        cnt.fetch_add(1);
                    }
                    ~ActiveGuard() {
                        cnt.fetch_sub(1);
                        if (is_link) link_cnt.fetch_sub(1);
                        std::lock_guard<std::mutex> lk(mtx);
                        cv.notify_all();
                    }
                } active_guard(active_jobs, is_link_dispatch, active_link_jobs, dag_cv, dag_mutex);

                DagNode* node_ptr = nodes[nid].get();
                {
                    std::lock_guard<std::mutex> lock(dag_mutex);
                    if (node_ptr->finished || node_ptr->skipped) continue;
                }

                size_t pi = node_ptr->plan_idx;
                TargetBuildPlan& plan = plans[pi];
                const Target* t = plan.target;

                if (node_ptr->kind == DagKind::StepJob) {
                    const ExpandedStepJob& job = node_ptr->step_job;
                    std::string reason;
                    bool needs_run = false;
                    {
                        std::lock_guard<std::mutex> slock(plan.state_mutex);
                        needs_run = CheckStepJobNeedsRun(project, job, plan.prev_state, reason);
                    }

                    if (!needs_run) {
                        total_steps_skipped.fetch_add(1);
                        finish_node(nid);
                        continue;
                    }

                    std::error_code dir_ec;
                    fs_util::CreateDirsLong(plan.gen_dir, dir_ec);
                    for (const auto& out_rel : job.outputs) {
                        std::filesystem::path out_p = project.ResolvePath(out_rel);
                        if (out_p.has_parent_path()) fs_util::CreateDirsLong(out_p.parent_path(), dir_ec);
                    }
                    if (!job.depfile.empty()) {
                        std::filesystem::path dp = project.ResolvePath(job.depfile);
                        if (dp.has_parent_path()) fs_util::CreateDirsLong(dp.parent_path(), dir_ec);
                    }

                    {
                        std::lock_guard<std::mutex> lk(log_exec_mutex);
                        if (!first_cmd_recorded) {
                            first_cmd_recorded = true;
                            std::lock_guard<std::mutex> rlock(m_result_mutex);
                            m_executed_command = job.cmd.display_command;
                        }
                    }

                    std::unordered_map<std::string, std::string> step_env = merge_with_tc_env({
                        { "GENIE_CONFIG", config },
                        { "GENIE_TARGET", t->name },
                        { "GENIE_ROOT", project.GetRootPath().generic_u8string() }
                    });

                    auto step_t0 = std::chrono::high_resolution_clock::now();
                    std::vector<OutLine> local_lines;
                    uint64_t slot_id = make_slot("[step] " + job.stepName + (job.label.empty() ? "" : (" " + job.label)));
                    int code = 0;
                    {
                        SlotGuard sg{ release_slot, slot_id };
                        code = ExecuteProcess(job.cmd, project.GetRootPath(), local_lines, &step_env, low_priority,
                                              &m_active_procs_mutex, &m_active_procs, &m_cancel_requested);
                    }
                    double step_dt = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - step_t0).count();

                    {
                        std::lock_guard<std::mutex> tlock(timing_mutex);
                        if (step_timings.find(job.stepName) == step_timings.end()) {
                            step_timing_order.push_back(job.stepName);
                        }
                        step_timings[job.stepName] += step_dt;
                        steps_total_time_s += step_dt;
                    }
                    total_steps_run.fetch_add(1);

                    std::vector<OutLine> emit;
                    OutLine prog;
                    prog.kind = OutLineKind::Progress;
                    prog.text = "[step " + job.stepName + "]" + (job.label.empty() ? "" : (" " + job.label));
                    emit.push_back(std::move(prog));
                    OutLine cmdl;
                    cmdl.kind = OutLineKind::Command;
                    cmdl.text = job.cmd.display_command;
                    emit.push_back(std::move(cmdl));
                    for (auto& l : local_lines) emit.push_back(std::move(l));
                    push_log_lines(emit);

                    if (code != 0 || m_cancel_requested.load()) {
                        if (m_cancel_requested.load()) {
                            std::error_code rec;
                            for (const auto& out_rel : job.outputs) {
                                fs_util::RemoveFileLong(project.ResolvePath(out_rel), rec);
                            }
                            if (!job.depfile.empty()) {
                                fs_util::RemoveFileLong(project.ResolvePath(job.depfile), rec);
                            }
                        }
                        int expected = 0;
                        first_exit_code.compare_exchange_strong(expected, code != 0 ? code : 1);
                        {
                            std::lock_guard<std::mutex> slock(plan.state_mutex);
                            plan.current_state.erase(job.stateKey);
                            save_state_map(plan.state_path, plan.current_state);
                        }
                        fail_node(nid);
                    } else {
                        std::lock_guard<std::mutex> slock(plan.state_mutex);
                        plan.current_state[job.stateKey] = job.cmdHash;
                        save_state_map(plan.state_path, plan.current_state);
                        finish_node(nid);
                    }
                } else if (node_ptr->kind == DagKind::PrepareCompile) {
                    if (t->kind != TargetKind::Interface) {
                        std::vector<std::string> compiled_files = t->GetCompiledFiles();
                        for (const auto& gs : plan.generated_sources) {
                            if (fs_util::ExistsLong(project.ResolvePath(gs))) {
                                if (std::find(compiled_files.begin(), compiled_files.end(), gs) == compiled_files.end()) {
                                    compiled_files.push_back(gs);
                                }
                            }
                        }

                        auto ov_warns = ValidateTargetOverrides(*t);
                        if (!ov_warns.empty()) {
                            std::vector<OutLine> warn_lines;
                            for (const auto& wmsg : ov_warns) {
                                OutLine warn_ol;
                                warn_ol.kind = OutLineKind::Raw;
                                warn_ol.text = "> Warning: " + wmsg;
                                warn_ol.has_warning = true;
                                warn_lines.push_back(std::move(warn_ol));
                            }
                            push_log_lines(warn_lines);
                        }

                        auto grouped_settings = ResolveTargetFilesGrouped(
                            project, *t, plan.eff, compiled_files, config, project.GetCompiler()
                        );

                        bool ccache_with_pch = compiler_cache.active && (compiler_cache.kind == CacheKind::Ccache) && !t->pch.IsEmpty();

                        // V2-8 Analyze PCH compatibility for this target
                        plan.pch_analysis = PchManager::AnalyzeAndPlanTargetPch(
                            project, *t, plan.eff, compiled_files, grouped_settings,
                            *cxx_comp, *c_comp, config, is_msvc, is_clang, ccache_with_pch, true
                        );

                        std::unordered_map<std::string, bool> file_uses_pch;
                        for (const auto& u : plan.pch_analysis.fileUsages) {
                            file_uses_pch[u.file] = u.usesPch;
                        }

                        // V2-9 Plan Unity batches if enabled
                        plan.unity_plan = UnityManager::PlanTargetUnity(
                            project, *t, compiled_files, grouped_settings, file_uses_pch,
                            config, project.GetCompiler(), is_msvc, &dep_log, true
                        );

                        // Collect all paths to batch-stat via V2-11 directory-batched stat
                        std::vector<std::string> paths_to_stat;
                        auto collect_dep_paths = [&](const std::string& out_p_str, const std::string& src_p_str) {
                            paths_to_stat.push_back(out_p_str);
                            if (!src_p_str.empty()) paths_to_stat.push_back(src_p_str);
                            DepOutputEntry rec;
                            if (dep_log.TryGetOutput(out_p_str, rec)) {
                                for (uint32_t did : rec.depIds) {
                                    std::string dp = dep_log.GetPath(did);
                                    if (!dp.empty()) paths_to_stat.push_back(dp);
                                }
                            }
                        };

                        if (plan.pch_analysis.cxxPch.enabled) {
                            collect_dep_paths(plan.pch_analysis.cxxPch.pchOutputFile.generic_u8string(),
                                              project.ResolvePath(plan.pch_analysis.cxxPch.headerRel).generic_u8string());
                            if (is_msvc) {
                                plan.object_paths.push_back(plan.pch_analysis.cxxPch.msvcObjectFile);
                            }
                        }
                        if (plan.pch_analysis.cPch.enabled) {
                            collect_dep_paths(plan.pch_analysis.cPch.pchOutputFile.generic_u8string(),
                                              project.ResolvePath(plan.pch_analysis.cPch.headerRel).generic_u8string());
                            if (is_msvc) {
                                plan.object_paths.push_back(plan.pch_analysis.cPch.msvcObjectFile);
                            }
                        }

                        // Prepare compile descriptors for unity batches + standalone files
                        struct PendingCompileSpec {
                            std::string src_rel;
                            std::string progress_label;
                            std::filesystem::path src_full;
                            std::filesystem::path obj_file;
                            std::string obj_key;
                            BuildCommand cmd;
                            std::string cmd_hash;
                            bool is_unity = false;
                            bool is_c = false;
                            bool uses_pch = false;
                            bool ccache_eligible = false;
                            bool uses_depfile = false;
                            bool uses_rc_hash = false;
                            std::filesystem::path depfile_path;
                            std::filesystem::path pch_out_path;
                            std::vector<std::string> unity_members;
                            uint32_t est_ms = 10;
                        };
                        std::vector<PendingCompileSpec> candidates;
                        double target_factor = Scheduler::ComputeTargetMsPerByte(*t, project, dep_log, plan.obj_dir, is_msvc);

                        // 1) Unity batches
                        for (const auto& ub : plan.unity_plan.batches) {
                            std::string obj_key;
                            std::filesystem::path obj_file = ComputeObjectPath(project, plan.obj_dir, ub.generatedFileRel, is_msvc, obj_key);
                            fs_util::CreateDirsLong(obj_file.parent_path(), ec);
                            plan.object_paths.push_back(obj_file);

                            const CompilerInfo* comp_info = ub.isC ? c_comp : cxx_comp;
                            if (!comp_info) comp_info = cxx_comp;

                            BuildCommand cmd = ub.settings
                                ? CompilerManager::FormulateCompileCommand(*comp_info, ub.generatedFileRel, obj_file, project, *ub.settings)
                                : CompilerManager::FormulateCompileCommand(*comp_info, ub.generatedFileRel, obj_file, project, plan.eff);

                            const PchArtifactPlan& active_pch = ub.isC ? plan.pch_analysis.cPch : plan.pch_analysis.cxxPch;
                            if (ub.usesPch && active_pch.enabled) {
                                PchManager::ApplyPchToCompileCommand(cmd, active_pch, is_msvc, is_clang, ccache_with_pch);
                            }

                            PendingCompileSpec pcs;
                            pcs.src_rel = ub.generatedFileRel;
                            pcs.src_full = ub.generatedFileAbs;
                            pcs.obj_file = obj_file;
                            pcs.obj_key = obj_key;
                            pcs.cmd_hash = CompilerManager::ComputeCommandHash(cmd);
                            pcs.cmd = std::move(cmd);
                            pcs.is_unity = true;
                            pcs.is_c = ub.isC;
                            pcs.uses_pch = ub.usesPch;
                            pcs.ccache_eligible = compiler_cache.active;
                            pcs.uses_depfile = true;
                            pcs.depfile_path = std::filesystem::path(obj_file.generic_u8string() + (is_msvc ? ".json" : ".d"));
                            if (ub.usesPch && active_pch.enabled) pcs.pch_out_path = active_pch.pchOutputFile;
                            pcs.unity_members = ub.memberFiles;

                            std::ostringstream lbl;
                            lbl << "[unity " << ub.batchName << "]";
                            size_t show_n = std::min<size_t>(3, ub.memberFiles.size());
                            for (size_t mi = 0; mi < show_n; ++mi) {
                                lbl << " " << std::filesystem::u8path(ub.memberFiles[mi]).filename().generic_u8string();
                            }
                            if (ub.memberFiles.size() > show_n) lbl << " ...";
                            pcs.progress_label = lbl.str();
                            pcs.est_ms = Scheduler::EstimateCompileDurationMs(obj_file, ub.generatedFileAbs, dep_log, target_factor);

                            collect_dep_paths(obj_file.generic_u8string(), ub.generatedFileAbs.generic_u8string());
                            for (const auto& mf : ub.memberFiles) {
                                paths_to_stat.push_back(project.ResolvePath(mf).generic_u8string());
                            }
                            candidates.push_back(std::move(pcs));
                        }

                        // 2) Standalone files (C/C++ and V2-7 extra source types)
                        for (const auto& file_rel : plan.unity_plan.standaloneFiles) {
                            SourceFileKind fk = ToolRegistry::ClassifySourceFile(file_rel, project.GetCustomTools());
                            std::filesystem::path src_full = project.ResolvePath(file_rel);

                            if (fk != SourceFileKind::C && fk != SourceFileKind::Cxx) {
                                ToolCompileSpec tspec = ToolRegistry::FormulateExtraSourceCommand(
                                    file_rel, *t, plan.eff, project, plan.obj_dir, plan.gen_dir,
                                    project.GetCompiler(), is_msvc,
                                    project.GetToolOverrides(), project.GetCustomTools(), project.GetAsmMode()
                                );
                                if (!tspec.supported) {
                                    OutLine err_ol;
                                    err_ol.kind = OutLineKind::Status;
                                    err_ol.text = "✗ " + tspec.errorMessage;
                                    err_ol.has_error = true;
                                    push_log_lines({ err_ol });
                                    first_exit_code.store(1);
                                    plan.failed = true;
                                    continue;
                                }
                                fs_util::CreateDirsLong(tspec.outputPath.parent_path(), ec);
                                if (!tspec.generatedHeaderPath.empty()) {
                                    fs_util::CreateDirsLong(tspec.generatedHeaderPath.parent_path(), ec);
                                }
                                if (tspec.linkOutput) {
                                    plan.object_paths.push_back(tspec.outputPath);
                                }

                                std::string cmd_h = CompilerManager::ComputeCommandHash(tspec.command);
                                if (tspec.usesRcHash) {
                                    cmd_h += "_" + ToolRegistry::ComputeRcContentHash(src_full, project.GetRootPath(), plan.eff.includeDirs);
                                }
                                PendingCompileSpec pcs;
                                pcs.src_rel = file_rel;
                                pcs.src_full = src_full;
                                pcs.obj_file = tspec.outputPath;
                                pcs.obj_key = tspec.outputPath.generic_u8string();
                                pcs.cmd = std::move(tspec.command);
                                pcs.cmd_hash = std::move(cmd_h);
                                pcs.uses_depfile = tspec.usesDepfile;
                                pcs.uses_rc_hash = tspec.usesRcHash;
                                pcs.depfile_path = tspec.depfilePath;
                                pcs.ccache_eligible = false; // never wrap .rc/.asm/.cu/.ispc/custom tools
                                pcs.est_ms = Scheduler::EstimateCompileDurationMs(tspec.outputPath, src_full, dep_log, target_factor);
                                collect_dep_paths(tspec.outputPath.generic_u8string(), src_full.generic_u8string());
                                candidates.push_back(std::move(pcs));
                                continue;
                            }

                            bool is_c = (fk == SourceFileKind::C);
                            const CompilerInfo* comp_info = is_c ? c_comp : cxx_comp;
                            if (!comp_info) comp_info = cxx_comp;

                            std::string obj_key;
                            std::filesystem::path obj_file = ComputeObjectPath(project, plan.obj_dir, file_rel, is_msvc, obj_key);
                            fs_util::CreateDirsLong(obj_file.parent_path(), ec);
                            plan.object_paths.push_back(obj_file);

                            auto eff_it = grouped_settings.find(file_rel);
                            BuildCommand cmd = (eff_it != grouped_settings.end() && eff_it->second)
                                ? CompilerManager::FormulateCompileCommand(*comp_info, file_rel, obj_file, project, *eff_it->second)
                                : CompilerManager::FormulateCompileCommand(*comp_info, file_rel, obj_file, project, plan.eff);

                            bool uses_pch = false;
                            auto upit = file_uses_pch.find(file_rel);
                            if (upit != file_uses_pch.end()) uses_pch = upit->second;
                            const PchArtifactPlan& active_pch = is_c ? plan.pch_analysis.cPch : plan.pch_analysis.cxxPch;
                            if (uses_pch && active_pch.enabled) {
                                PchManager::ApplyPchToCompileCommand(cmd, active_pch, is_msvc, is_clang, ccache_with_pch);
                            }

                            PendingCompileSpec pcs;
                            pcs.src_rel = file_rel;
                            pcs.src_full = src_full;
                            pcs.obj_file = obj_file;
                            pcs.obj_key = obj_key;
                            pcs.cmd_hash = CompilerManager::ComputeCommandHash(cmd);
                            pcs.cmd = std::move(cmd);
                            pcs.is_c = is_c;
                            pcs.uses_pch = uses_pch;
                            pcs.ccache_eligible = compiler_cache.active;
                            pcs.uses_depfile = true;
                            pcs.depfile_path = std::filesystem::path(obj_file.generic_u8string() + (is_msvc ? ".json" : ".d"));
                            if (uses_pch && active_pch.enabled) pcs.pch_out_path = active_pch.pchOutputFile;
                            pcs.est_ms = Scheduler::EstimateCompileDurationMs(obj_file, src_full, dep_log, target_factor);
                            collect_dep_paths(obj_file.generic_u8string(), src_full.generic_u8string());
                            candidates.push_back(std::move(pcs));
                        }

                        checked_files_total.fetch_add(compiled_files.size());

                        // Batch stat all collected paths in parallel across directories (V2-11)
                        auto stat_map = DepLog::BatchStatFiles(paths_to_stat, num_workers);

                        // Spawn PchBuild nodes if needed
                        size_t cxx_pch_node_id = static_cast<size_t>(-1);
                        size_t c_pch_node_id = static_cast<size_t>(-1);
                        std::vector<std::unique_ptr<DagNode>> new_pch_nodes;

                        auto maybe_plan_pch = [&](const PchArtifactPlan& pp, bool& out_will_rebuild) -> int {
                            out_will_rebuild = false;
                            if (!pp.enabled) return -1;
                            bool needs_pch = check_needs_rebuild_using_dep_log(
                                pp.pchOutputFile.generic_u8string(),
                                project.ResolvePath(pp.headerRel).generic_u8string(),
                                pp.cmdHash,
                                "pch:" + std::string(pp.isC ? "c" : "cxx"),
                                stat_map,
                                pp.depfilePath,
                                is_msvc,
                                false,
                                nullptr
                            );
                            if (is_msvc && !fs_util::ExistsLong(pp.msvcObjectFile)) {
                                needs_pch = true;
                            }
                            if (!needs_pch) return -1;
                            out_will_rebuild = true;
                            auto pn = std::make_unique<DagNode>();
                            pn->kind = DagKind::PchBuild;
                            pn->plan_idx = pi;
                            pn->pch_plan = pp;
                            pn->priority_ms = 200000;
                            if (compiler_cache.active) {
                                CompilerCacheManager::PrependLauncher(pn->pch_plan.compileCmd, compiler_cache);
                            }
                            new_pch_nodes.push_back(std::move(pn));
                            return static_cast<int>(new_pch_nodes.size() - 1);
                        };

                        bool cxx_pch_rebuilt = false, c_pch_rebuilt = false;
                        int cxx_pch_local_idx = maybe_plan_pch(plan.pch_analysis.cxxPch, cxx_pch_rebuilt);
                        int c_pch_local_idx = maybe_plan_pch(plan.pch_analysis.cPch, c_pch_rebuilt);

                        std::vector<std::unique_ptr<DagNode>> new_compiles;
                        std::vector<int> compile_pch_dep_idx;
                        for (auto& pcs : candidates) {
                            bool force_pch_rebuild = pcs.uses_pch && (pcs.is_c ? c_pch_rebuilt : cxx_pch_rebuilt);
                            bool needs_compile = force_pch_rebuild || check_needs_rebuild_using_dep_log(
                                pcs.obj_file.generic_u8string(),
                                pcs.src_full.generic_u8string(),
                                pcs.cmd_hash,
                                pcs.obj_key,
                                stat_map,
                                pcs.depfile_path,
                                is_msvc,
                                pcs.uses_rc_hash,
                                nullptr
                            );
                            if (needs_compile) {
                                plan.compiled_task_count++;
                                auto cn = std::make_unique<DagNode>();
                                cn->kind = DagKind::Compile;
                                cn->plan_idx = pi;
                                cn->target_name = t->name;
                                cn->src_rel = std::move(pcs.src_rel);
                                cn->progress_label = std::move(pcs.progress_label);
                                cn->obj_path = std::move(pcs.obj_file);
                                cn->obj_key = std::move(pcs.obj_key);
                                cn->cmd = std::move(pcs.cmd);
                                cn->cmd_hash = std::move(pcs.cmd_hash);
                                if (pcs.ccache_eligible) {
                                    CompilerCacheManager::PrependLauncher(cn->cmd, compiler_cache);
                                }
                                cn->is_unity = pcs.is_unity;
                                cn->is_c_compile = pcs.is_c;
                                cn->uses_pch = pcs.uses_pch;
                                cn->ccache_eligible = pcs.ccache_eligible;
                                cn->uses_depfile = pcs.uses_depfile;
                                cn->uses_rc_hash = pcs.uses_rc_hash;
                                cn->depfile_path = std::move(pcs.depfile_path);
                                cn->pch_dep_output = std::move(pcs.pch_out_path);
                                cn->unity_members = std::move(pcs.unity_members);
                                cn->priority_ms = pcs.est_ms;

                                int dep_local = -1;
                                if (pcs.uses_pch) {
                                    dep_local = pcs.is_c ? c_pch_local_idx : cxx_pch_local_idx;
                                }
                                compile_pch_dep_idx.push_back(dep_local);
                                m_remaining_est_ms.fetch_add(static_cast<int64_t>(cn->priority_ms));
                                new_compiles.push_back(std::move(cn));
                            } else {
                                up_to_date_count.fetch_add(1);
                                m_prog_uptodate.fetch_add(1);
                            }
                        }

                        if (!new_pch_nodes.empty() || !new_compiles.empty()) {
                            total_compile_tasks.fetch_add(new_compiles.size());
                            m_prog_total.fetch_add(new_compiles.size());
                            {
                                std::lock_guard<std::mutex> lk(log_exec_mutex);
                                if (!first_cmd_recorded && (!new_pch_nodes.empty() || !new_compiles.empty())) {
                                    first_cmd_recorded = true;
                                    const std::string& disp = !new_pch_nodes.empty()
                                        ? new_pch_nodes[0]->pch_plan.compileCmd.display_command
                                        : new_compiles[0]->cmd.display_command;
                                    OutLine first_cmd_line;
                                    first_cmd_line.kind = OutLineKind::Command;
                                    first_cmd_line.text = disp;
                                    push_log_lines({ first_cmd_line });
                                    std::lock_guard<std::mutex> rlock(m_result_mutex);
                                    m_executed_command = disp;
                                }
                            }

                            std::lock_guard<std::mutex> lock(dag_mutex);
                            size_t link_id = link_nodes[pi];
                            std::vector<size_t> created_pch_ids;
                            for (auto& pn : new_pch_nodes) {
                                size_t pid = nodes.size();
                                pn->dependents.push_back(link_id);
                                nodes[link_id]->pending_deps.fetch_add(1);
                                nodes.push_back(std::move(pn));
                                remaining_nodes.fetch_add(1);
                                created_pch_ids.push_back(pid);
                                ready_queue.push(pid);
                            }
                            if (cxx_pch_local_idx >= 0) cxx_pch_node_id = created_pch_ids[static_cast<size_t>(cxx_pch_local_idx)];
                            if (c_pch_local_idx >= 0) c_pch_node_id = created_pch_ids[static_cast<size_t>(c_pch_local_idx)];

                            for (size_t ci = 0; ci < new_compiles.size(); ++ci) {
                                auto& cn = new_compiles[ci];
                                size_t cid = nodes.size();
                                cn->dependents.push_back(link_id);
                                nodes[link_id]->pending_deps.fetch_add(1);
                                int p_local = compile_pch_dep_idx[ci];
                                if (p_local >= 0) {
                                    size_t p_nid = created_pch_ids[static_cast<size_t>(p_local)];
                                    nodes[p_nid]->dependents.push_back(cid);
                                    cn->pending_deps.fetch_add(1);
                                }
                                nodes.push_back(std::move(cn));
                                remaining_nodes.fetch_add(1);
                                if (nodes[cid]->pending_deps.load() == 0) {
                                    ready_queue.push(cid);
                                }
                            }
                            dag_cv.notify_all();
                        }
                    }
                    if (plan.failed) fail_node(nid);
                    else finish_node(nid);
                } else if (node_ptr->kind == DagKind::PchBuild) {
                    const PchArtifactPlan& pp = node_ptr->pch_plan;
                    std::unordered_map<std::string, std::string> pch_env = tc_base_env;
                    if (compiler_cache.active) {
                        CompilerCacheManager::PopulateEnv(pch_env, compiler_cache, cache_prefs, true);
                    }
                    auto t0 = std::chrono::high_resolution_clock::now();
                    std::vector<OutLine> local_lines;
                    uint64_t slot_id = make_slot("[pch] " + t->name);
                    int code = 0;
                    {
                        SlotGuard sg{ release_slot, slot_id };
                        code = ExecuteProcess(pp.compileCmd, project.GetRootPath(), local_lines,
                                              pch_env.empty() ? nullptr : &pch_env, low_priority,
                                              &m_active_procs_mutex, &m_active_procs, &m_cancel_requested);
                    }
                    double dt = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();

                    {
                        std::lock_guard<std::mutex> tlock(timing_mutex);
                        pch_total_time_s += dt;
                    }
                    total_pchs_built.fetch_add(1);

                    std::vector<OutLine> emit;
                    OutLine prog;
                    prog.kind = OutLineKind::Progress;
                    prog.text = "[pch] " + t->name + (pp.isC ? " (C)" : " (C++)");
                    emit.push_back(std::move(prog));
                    OutLine cmdl;
                    cmdl.kind = OutLineKind::Command;
                    cmdl.text = pp.compileCmd.display_command;
                    emit.push_back(std::move(cmdl));
                    for (auto& l : local_lines) emit.push_back(std::move(l));
                    push_log_lines(emit);

                    if (code != 0 || m_cancel_requested.load()) {
                        if (m_cancel_requested.load()) {
                            std::error_code rec;
                            fs_util::RemoveFileLong(pp.pchOutputFile, rec);
                            if (!pp.msvcObjectFile.empty()) fs_util::RemoveFileLong(pp.msvcObjectFile, rec);
                            fs_util::RemoveFileLong(pp.depfilePath, rec);
                        }
                        int expected = 0;
                        first_exit_code.compare_exchange_strong(expected, code != 0 ? code : 1);
                        plan.failed = true;
                        fail_node(nid);
                    } else {
                        std::error_code tec;
                        int64_t mtime_ns = DepLog::StatFileNs(pp.pchOutputFile);
                        std::vector<std::string> raw_deps = is_msvc ? ParseMsvcDeps(pp.depfilePath) : ParseStepDepfile(pp.depfilePath);
                        std::vector<std::string> stored_deps;
                        stored_deps.push_back(project.ResolvePath(pp.headerRel).generic_u8string());
                        for (const auto& d : raw_deps) {
                            std::string abs_d = project.ResolvePath(d).generic_u8string();
                            if (ignore_sys_headers && DepLog::IsSystemHeader(abs_d)) continue;
                            stored_deps.push_back(std::move(abs_d));
                        }
                        dep_log.RecordOutput(
                            pp.pchOutputFile.generic_u8string(),
                            mtime_ns,
                            DepLog::ParseHashU64(pp.cmdHash),
                            static_cast<uint32_t>(dt * 1000.0),
                            stored_deps
                        );
                        if (!keep_dep_files) std::filesystem::remove(pp.depfilePath, tec);
                        finish_node(nid);
                    }
                } else if (node_ptr->kind == DagKind::Compile) {
                    std::unordered_map<std::string, std::string> comp_env = tc_base_env;
                    if (node_ptr->ccache_eligible && compiler_cache.active) {
                        CompilerCacheManager::PopulateEnv(comp_env, compiler_cache, cache_prefs, any_pch_used);
                    }

                    if (!is_msvc && !is_clang) {
                        std::string long_arg;
                        if (fs_util::HasMinGwLongArgRisk(node_ptr->cmd, long_arg) && !mingw_long_warned.exchange(true)) {
                            OutLine lwarn;
                            lwarn.kind = OutLineKind::Raw;
                            lwarn.has_warning = true;
                            lwarn.text = "> Warning: MinGW tools may fail on long paths (>= 250 chars) — consider moving the project to a shorter folder.";
                            push_log_lines({ lwarn });
                        }
                    }

                    auto t0 = std::chrono::high_resolution_clock::now();
                    std::vector<OutLine> local_lines;
                    std::string job_lbl = !node_ptr->progress_label.empty() ? node_ptr->progress_label : node_ptr->src_rel;
                    uint64_t slot_id = make_slot(job_lbl);
                    int code = 0;
                    {
                        SlotGuard sg{ release_slot, slot_id };
                        code = ExecuteProcess(node_ptr->cmd, project.GetRootPath(), local_lines,
                                              comp_env.empty() ? nullptr : &comp_env, low_priority,
                                              &m_active_procs_mutex, &m_active_procs, &m_cancel_requested);
                    }
                    double dt = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
                    m_remaining_est_ms.fetch_sub(static_cast<int64_t>(node_ptr->priority_ms));
                    m_prog_completed.fetch_add(1);

                    {
                        std::lock_guard<std::mutex> tlock(timing_mutex);
                        compile_total_time_s += dt;
                        compile_timings.push_back(SlowestFileInfo{
                            node_ptr->src_rel,
                            static_cast<uint32_t>(dt * 1000.0)
                        });
                    }

                    size_t done_now = completed_compile_tasks.fetch_add(1) + 1;
                    size_t total_now = total_compile_tasks.load();

                    std::vector<OutLine> emit;
                    OutLine prog;
                    prog.kind = OutLineKind::Progress;
                    if (!node_ptr->progress_label.empty()) {
                        prog.text = "[" + std::to_string(done_now) + "/" + std::to_string(total_now) + "] " + node_ptr->progress_label;
                    } else if (multi_target_output) {
                        prog.text = "[" + std::to_string(done_now) + "/" + std::to_string(total_now) + "] [" + node_ptr->target_name + "] " + node_ptr->src_rel;
                    } else {
                        prog.text = "[" + std::to_string(done_now) + "/" + std::to_string(total_now) + "] " + node_ptr->src_rel;
                    }
                    emit.push_back(std::move(prog));
                    for (auto& l : local_lines) emit.push_back(std::move(l));

                    if (code != 0 || m_cancel_requested.load()) {
                        if (m_cancel_requested.load()) {
                            std::error_code rec;
                            fs_util::RemoveFileLong(node_ptr->obj_path, rec);
                            if (!node_ptr->depfile_path.empty()) fs_util::RemoveFileLong(node_ptr->depfile_path, rec);
                        } else {
                            failed_files_count.fetch_add(1);
                            m_prog_failed.fetch_add(1);
                        }
                        plan.failed = true;
                        int expected = 0;
                        first_exit_code.compare_exchange_strong(expected, code != 0 ? code : 1);
                        if (!m_cancel_requested.load() && node_ptr->is_unity) {
                            OutLine uhint;
                            uhint.kind = OutLineKind::Raw;
                            uhint.text = "> Hint: Unity batches can cause name clashes (static functions, anonymous namespaces). Exclude a file from unity: right-click -> Exclude from unity.";
                            uhint.has_warning = true;
                            emit.push_back(std::move(uhint));
                        }
                        push_log_lines(emit);
                        fail_node(nid);
                    } else {
                        m_prog_compiled.fetch_add(1);
                        push_log_lines(emit);
                        std::error_code tec;
                        int64_t mtime_ns = DepLog::StatFileNs(node_ptr->obj_path);
                        std::vector<std::string> stored_deps;
                        if (node_ptr->uses_rc_hash) {
                            stored_deps = ToolRegistry::ScanRcDependencies(
                                project.ResolvePath(node_ptr->src_rel), project.GetRootPath(), plan.eff.includeDirs
                            );
                        } else if (node_ptr->uses_depfile) {
                            std::vector<std::string> raw_deps = is_msvc
                                ? ParseMsvcDeps(node_ptr->depfile_path)
                                : ParseStepDepfile(node_ptr->depfile_path);
                            for (const auto& d : raw_deps) {
                                std::string abs_d = project.ResolvePath(d).generic_u8string();
                                if (ignore_sys_headers && DepLog::IsSystemHeader(abs_d)) continue;
                                stored_deps.push_back(std::move(abs_d));
                            }
                            if (!keep_dep_files) std::filesystem::remove(node_ptr->depfile_path, tec);
                        }
                        if (!node_ptr->pch_dep_output.empty()) {
                            stored_deps.push_back(node_ptr->pch_dep_output.generic_u8string());
                        }
                        dep_log.RecordOutput(
                            node_ptr->obj_path.generic_u8string(),
                            mtime_ns,
                            DepLog::ParseHashU64(node_ptr->cmd_hash),
                            static_cast<uint32_t>(dt * 1000.0),
                            stored_deps
                        );
                        finish_node(nid);
                    }
                } else if (node_ptr->kind == DagKind::TargetLink) {
                    bool link_failed = false;
                    if (t->kind != TargetKind::Interface && !plan.object_paths.empty()) {
                        std::filesystem::path binary_path = t->GetBinaryPath(abs_out, config, is_msvc);
                        fs_util::CreateDirsLong(binary_path.parent_path(), ec);
                        {
                            std::lock_guard<std::mutex> llock(link_track_mutex);
                            if (t->kind == TargetKind::Executable || t->kind == TargetKind::Shared) {
                                runtime_bin_dirs.push_back(binary_path.parent_path());
                            }
                            if (!t->freestanding) {
                                for (const auto& d : plan.eff.copyDlls) {
                                    if (std::find(all_copy_dlls.begin(), all_copy_dlls.end(), d) == all_copy_dlls.end()) {
                                        all_copy_dlls.push_back(d);
                                    }
                                }
                            }
                        }

                        BuildCommand link_cmd_preview = CompilerManager::FormulateLinkCommand(
                            *cxx_comp, *t, plan.eff, plan.object_paths, binary_path, project, false
                        );
                        std::vector<std::filesystem::path> hash_items = plan.object_paths;
                        for (const auto& [dep_name, implib_p] : plan.eff.sharedDepImportLibs) {
                            std::string imp_hash = ComputeFileContentHash(implib_p);
                            hash_items.push_back(std::filesystem::path("__implib__:" + dep_name + ":" + imp_hash));
                        }
                        if (!t->linkerScript.empty()) {
                            std::filesystem::path lds_p = project.ResolvePath(t->linkerScript);
                            hash_items.push_back(std::filesystem::path("__lds__:" + t->linkerScript + ":" + ComputeFileContentHash(lds_p)));
                        }
                        std::string link_hash = CompilerManager::ComputeLinkHash(link_cmd_preview, hash_items);
                        uint64_t link_hash_u64 = DepLog::ParseHashU64(link_hash);

                        bool needs_link = false;
                        std::error_code bin_ec;
                        bool binary_exists = fs_util::ExistsLong(binary_path);
                        if (t->kind == TargetKind::Shared) {
                            std::filesystem::path implib_p = t->GetImportLibPath(abs_out, config, is_msvc);
                            if (!implib_p.empty() && !fs_util::ExistsLong(implib_p)) {
                                binary_exists = false;
                            }
                        }

                        DepOutputEntry prev_link_rec;
                        bool has_link_rec = dep_log.TryGetOutput(binary_path.generic_u8string(), prev_link_rec);
                        if (!binary_exists || plan.compiled_task_count > 0 ||
                            !has_link_rec || prev_link_rec.cmdHash != link_hash_u64) {
                            needs_link = true;
                        } else {
                            auto bin_time = std::filesystem::last_write_time(fs_util::ToWinLongPath(binary_path), bin_ec);
                            if (bin_ec) {
                                needs_link = true;
                            } else {
                                for (const auto& obj_p : plan.object_paths) {
                                    std::error_code obj_ec;
                                    auto obj_t = std::filesystem::last_write_time(fs_util::ToWinLongPath(obj_p), obj_ec);
                                    if (obj_ec || obj_t > bin_time) {
                                        needs_link = true;
                                        break;
                                    }
                                }
                                if (!needs_link) {
                                    std::lock_guard<std::mutex> llock(link_track_mutex);
                                    for (const auto& static_lib : plan.eff.staticDepLibraries) {
                                        if (rebuilt_static_outputs.find(static_lib.generic_u8string()) != rebuilt_static_outputs.end()) {
                                            needs_link = true;
                                            break;
                                        }
                                        std::error_code sl_ec;
                                        auto sl_t = std::filesystem::last_write_time(fs_util::ToWinLongPath(static_lib), sl_ec);
                                        if (sl_ec || sl_t > bin_time) {
                                            needs_link = true;
                                            break;
                                        }
                                    }
                                }
                                if (!needs_link && !t->linkerScript.empty()) {
                                    int64_t lds_ns = DepLog::StatFileNs(project.ResolvePath(t->linkerScript));
                                    int64_t bin_ns = DepLog::StatFileNs(binary_path);
                                    if (lds_ns == 0 || bin_ns == 0 || lds_ns > bin_ns) {
                                        needs_link = true;
                                    }
                                }
                                if (!needs_link && IsAnyResolvedLibraryNewer(project, plan.eff, is_msvc, bin_time)) {
                                    needs_link = true;
                                }
                            }
                        }

                        if (needs_link) {
                            total_links.fetch_add(1);
                            if (t->kind == TargetKind::Static && fs_util::ExistsLong(binary_path)) {
                                fs_util::RemoveFileLong(binary_path, bin_ec);
                            }

                            BuildCommand link_cmd = CompilerManager::FormulateLinkCommand(
                                *cxx_comp, *t, plan.eff, plan.object_paths, binary_path, project, true
                            );

                            {
                                std::lock_guard<std::mutex> lk(log_exec_mutex);
                                if (!first_cmd_recorded) {
                                    first_cmd_recorded = true;
                                    std::lock_guard<std::mutex> rlock(m_result_mutex);
                                    m_executed_command = link_cmd.display_command;
                                }
                            }

                            OutLine link_line;
                            link_line.kind = OutLineKind::Command;
                            link_line.text = link_cmd.display_command;
                            push_log_lines({ link_line });

                            auto lt0 = std::chrono::high_resolution_clock::now();
                            std::vector<OutLine> link_output;
                            uint64_t lslot = make_slot("[link] " + t->name);
                            int link_code = 0;
                            {
                                SlotGuard lsg{ release_slot, lslot };
                                link_code = ExecuteProcess(link_cmd, project.GetRootPath(), link_output,
                                                           tc_base_env.empty() ? nullptr : &tc_base_env, low_priority,
                                                           &m_active_procs_mutex, &m_active_procs, &m_cancel_requested);
                            }
                            double ldt = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - lt0).count();
                            {
                                std::lock_guard<std::mutex> tlock(timing_mutex);
                                link_total_time_s += ldt;
                            }
                            push_log_lines(link_output);

                            if (link_code == 0 && !m_cancel_requested.load()) {
                                built_targets_count.fetch_add(1);
                                {
                                    std::lock_guard<std::mutex> tlock(timing_mutex);
                                    link_timings.emplace_back(t->name, ldt);
                                }
                                int64_t b_ns = DepLog::StatFileNs(binary_path);
                                dep_log.RecordOutput(
                                    binary_path.generic_u8string(),
                                    b_ns,
                                    link_hash_u64,
                                    static_cast<uint32_t>(ldt * 1000.0),
                                    {}
                                );
                                if (t->kind == TargetKind::Static) {
                                    std::lock_guard<std::mutex> llock(link_track_mutex);
                                    rebuilt_static_outputs.insert(binary_path.generic_u8string());
                                }
                            } else {
                                if (m_cancel_requested.load()) {
                                    std::error_code rec;
                                    fs_util::RemoveFileLong(binary_path, rec);
                                    if (t->kind == TargetKind::Shared) {
                                        std::filesystem::path implib_p = t->GetImportLibPath(abs_out, config, is_msvc);
                                        if (!implib_p.empty()) fs_util::RemoveFileLong(implib_p, rec);
                                    }
                                }
                                link_failed = true;
                                skipped_links_count.fetch_add(1);
                                int expected = 0;
                                first_exit_code.compare_exchange_strong(expected, link_code);
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
                                    push_log_lines({ hint });
                                }
                            }
                        } else {
                            built_targets_count.fetch_add(1);
                        }
                    }

                    if (!link_failed && !plan.failed) {
                        UnityManager::SaveAdaptiveHotState(project, *t, config, plan.unity_plan);
                        finish_node(nid);
                    } else {
                        fail_node(nid);
                    }
                }
            }
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    {
        std::lock_guard<std::mutex> aplk(m_active_procs_mutex);
        m_worker_abort_flag = nullptr;
        m_worker_dag_cv = nullptr;
        m_worker_dag_mutex = nullptr;
    }
    {
        std::lock_guard<std::mutex> plk(m_progress_mutex);
        m_running_slots.clear();
    }
    stop_mem_monitor.store(true);
    if (mem_monitor.joinable()) mem_monitor.join();
    m_throttled_low_memory.store(false);

    dep_log.Close();

    const bool was_cancelled = m_cancel_requested.load();
    bool build_ok = !was_cancelled && !abort_all.load() && (failed_files_count.load() == 0) && (skipped_links_count.load() == 0) && (first_exit_code.load() == 0);
    const size_t total_tasks = total_compile_tasks.load();

    // V2-10 post-build cache stats
    BuildCacheSummary cache_summary;
    if (compiler_cache.active && total_tasks > 0 && !was_cancelled) {
        CacheStatsSnapshot cache_after = CompilerCacheManager::QueryStats(compiler_cache);
        cache_summary = CompilerCacheManager::ComputeDelta(compiler_cache, cache_before, cache_after);
    }

    // V2-19 Export compile_commands.json after build plan (even if build failed, unless disabled)
    CompDbExportResult compdb_res;
    if (project.GetCompileCommandsConfig().enabled && !was_cancelled) {
        compdb_res = CompDbManager::ExportCompileCommands(project, compiler_manager, active_tc);
    }

    // 4. Post-link DLL copying (§5.1.2)
    if (build_ok && !all_copy_dlls.empty()) {
        std::filesystem::path bin_dir = runtime_bin_dirs.empty()
            ? (abs_out / config / "bin")
            : runtime_bin_dirs[0];
        for (const auto& dll_rel : all_copy_dlls) {
            std::filesystem::path src_dll = project.ResolvePath(dll_rel);
            if (!fs_util::ExistsLong(src_dll)) {
                OutLine warn;
                warn.kind = OutLineKind::Raw;
                warn.text = "> Warning: DLL to copy not found: " + dll_rel + " (resolved: " + src_dll.generic_u8string() + ")";
                warn.has_warning = true;
                {
                    std::lock_guard<std::mutex> lock(m_queue_mutex);
                    m_output_queue.push_back(warn);
                }
                append_log({ warn });
            } else {
                std::filesystem::path dst_dll = bin_dir / src_dll.filename();
                bool should_copy = true;
                if (fs_util::ExistsLong(dst_dll)) {
                    auto src_t = std::filesystem::last_write_time(fs_util::ToWinLongPath(src_dll), ec);
                    auto dst_t = std::filesystem::last_write_time(fs_util::ToWinLongPath(dst_dll), ec);
                    if (!ec && src_t <= dst_t) {
                        should_copy = false;
                    }
                }
                if (should_copy) {
                    fs_util::CreateDirsLong(bin_dir, ec);
                    fs_util::CopyFileLong(src_dll, dst_dll, ec);
                    OutLine copy_msg;
                    copy_msg.kind = OutLineKind::Raw;
                    copy_msg.text = "> Copied " + src_dll.filename().generic_u8string();
                    {
                        std::lock_guard<std::mutex> lock(m_queue_mutex);
                        m_output_queue.push_back(copy_msg);
                    }
                    append_log({ copy_msg });
                }
            }
        }
    }

    const auto end_time = std::chrono::high_resolution_clock::now();
    const double duration = std::chrono::duration<double>(end_time - start_time).count();

    const int final_exit_code = first_exit_code.load();
    const size_t steps_ran = total_steps_run.load();
    const size_t pchs_ran = total_pchs_built.load();
    const size_t checked_cnt = checked_files_total.load();
    const size_t compiled_ok_cnt = m_prog_compiled.load();

    // V2-20 Top 10 slowest files
    std::vector<SlowestFileInfo> slowest_top10;
    {
        std::lock_guard<std::mutex> tlock(timing_mutex);
        slowest_top10 = compile_timings;
    }
    std::sort(slowest_top10.begin(), slowest_top10.end(), [](const SlowestFileInfo& a, const SlowestFileInfo& b) {
        return a.durationMs > b.durationMs;
    });
    if (slowest_top10.size() > 10) slowest_top10.resize(10);

    // Summary line formatted with counts, PCH/step/link timings, cache rate, cancel & keepGoing counts
    std::ostringstream ss_summary;
    if (was_cancelled) {
        ss_summary << "■ Build cancelled · " << compiled_ok_cnt << " compiled · "
                   << std::fixed << std::setprecision(2) << duration << "s";
    } else if (build_ok) {
        if (total_tasks == 0 && total_links.load() == 0 && steps_ran == 0 && pchs_ran == 0) {
            ss_summary << "✓ Up to date · checked " << checked_cnt << " file(s) in "
                       << std::fixed << std::setprecision(2) << duration << "s";
        } else {
            ss_summary << "✓ Build succeeded · ";
            if (steps_ran > 0) {
                ss_summary << steps_ran << (steps_ran == 1 ? " step" : " steps");
                if (!step_timing_order.empty()) {
                    ss_summary << " (";
                    for (size_t i = 0; i < step_timing_order.size(); ++i) {
                        if (i > 0) ss_summary << ", ";
                        const std::string& sn = step_timing_order[i];
                        ss_summary << sn << " " << std::fixed << std::setprecision(2) << step_timings[sn] << "s";
                    }
                    ss_summary << ")";
                }
                ss_summary << ", ";
            }
            if (pchs_ran > 0) {
                ss_summary << pchs_ran << " pch (" << std::fixed << std::setprecision(2) << pch_total_time_s << "s), ";
            }
            ss_summary << total_tasks << " compiled, "
                       << up_to_date_count.load() << " up to date";
            if (!link_timings.empty()) {
                ss_summary << " · link ";
                for (size_t li = 0; li < link_timings.size(); ++li) {
                    if (li > 0) ss_summary << ", ";
                    ss_summary << link_timings[li].first << " " << std::fixed << std::setprecision(2) << link_timings[li].second << "s";
                }
            }
            if (cache_summary.valid) {
                ss_summary << " · cache " << std::fixed << std::setprecision(0) << cache_summary.hitRatePercent << "% hit";
            }
            ss_summary << " · " << std::fixed << std::setprecision(2) << duration << "s";
        }
    } else if (keep_going) {
        ss_summary << "✗ " << failed_files_count.load() << " file(s) failed · "
                   << skipped_links_count.load() << " target(s) not linked · "
                   << built_targets_count.load() << " target(s) built · "
                   << std::fixed << std::setprecision(2) << duration << "s";
    } else {
        ss_summary << "✗ Build failed · exit " << final_exit_code << " · "
                   << std::fixed << std::setprecision(2) << duration << "s";
    }

    std::string summary_line = ss_summary.str();
    std::vector<OutLine> end_lines;
    OutLine blank;
    blank.kind = OutLineKind::Raw;
    blank.text = "";
    end_lines.push_back(blank);

    if (was_cancelled) {
        OutLine cnote;
        cnote.kind = OutLineKind::Raw;
        cnote.text = "■ cancelled by user";
        end_lines.push_back(cnote);
    }

    OutLine sum;
    sum.kind = OutLineKind::Status;
    sum.text = summary_line;
    sum.has_error = !build_ok;
    end_lines.push_back(sum);

    // Log top 10 slowest files into disk build log text as well (V2-20)
    if (!slowest_top10.empty()) {
        std::vector<OutLine> slow_log_lines;
        OutLine hdr;
        hdr.kind = OutLineKind::Raw;
        hdr.text = "Slowest files:";
        slow_log_lines.push_back(hdr);
        for (const auto& sf : slowest_top10) {
            OutLine sl;
            sl.kind = OutLineKind::Raw;
            std::ostringstream ssf;
            ssf << "  " << std::fixed << std::setprecision(2) << (sf.durationMs / 1000.0) << "s  " << sf.path;
            sl.text = ssf.str();
            slow_log_lines.push_back(std::move(sl));
        }
        append_log(slow_log_lines);
    }

    push_log_lines(end_lines);
    if (m_build_log) {
        m_build_log->EndSession();
    }

    bool defender_hint = false;
    if (build_ok && total_tasks > 500) {
        UserSettings us = HintsManager::LoadUserSettings();
        defender_hint = HintsManager::ShouldSuggestDefenderExclusion(project.GetRootPath(), total_tasks, us);
    }

    (void)log_path;
    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_last_result.exit_code = build_ok ? 0 : (final_exit_code != 0 ? final_exit_code : 1);
        m_last_result.duration_seconds = duration;
        m_last_result.success = build_ok;
        m_last_result.cancelled = was_cancelled;
        m_last_result.checked_files_count = checked_cnt;
        m_last_result.compiled_count = compiled_ok_cnt;
        m_last_result.up_to_date_count = up_to_date_count.load();
        m_last_result.failed_count = failed_files_count.load();
        m_last_result.skipped_count = skipped_links_count.load();
        m_last_result.active_worker_count = budget.workers;
        m_last_result.ram_capped = budget.ramCapped;
        m_last_result.cache_summary = cache_summary;
        m_last_result.steps_seconds = steps_total_time_s;
        m_last_result.pch_seconds = pch_total_time_s;
        m_last_result.compile_seconds = compile_total_time_s;
        m_last_result.link_seconds = link_total_time_s;
        m_last_result.slowest_files = std::move(slowest_top10);
        m_last_result.compile_commands_ok = compdb_res.success;
        m_last_result.compile_commands_modified = compdb_res.fileModified;
        m_last_result.compile_commands_count = compdb_res.entryCount;
        m_last_result.defender_hint_recommended = defender_hint;
    }

    m_status.store(BuildStatus::Finished);
}

} // namespace core
