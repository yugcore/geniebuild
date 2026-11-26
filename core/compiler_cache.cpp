#include "compiler_cache.h"

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core {

namespace {

std::string ToLowerCache(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string FindCacheExe(const std::string& name_or_path) {
    if (name_or_path.empty()) return "";
    std::error_code ec;
    std::filesystem::path p(name_or_path);
    if (p.is_absolute() || name_or_path.find('/') != std::string::npos || name_or_path.find('\\') != std::string::npos) {
        if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
            return p.generic_u8string();
        }
#if defined(_WIN32)
        if (!p.has_extension()) {
            std::filesystem::path pe(p.generic_u8string() + ".exe");
            if (std::filesystem::exists(pe, ec) && std::filesystem::is_regular_file(pe, ec)) {
                return pe.generic_u8string();
            }
        }
#endif
        return "";
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";
#if defined(_WIN32)
    const char delim = ';';
#else
    const char delim = ':';
#endif
    std::stringstream ss(path_env);
    std::string dir_s;
    while (std::getline(ss, dir_s, delim)) {
        if (dir_s.empty()) continue;
        std::filesystem::path d(dir_s);
#if defined(_WIN32)
        for (const char* ext : { ".exe", ".cmd", "" }) {
            std::filesystem::path c = d / (name_or_path + ext);
            if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec)) {
                return c.generic_u8string();
            }
        }
#else
        std::filesystem::path c = d / name_or_path;
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec)) {
            return c.generic_u8string();
        }
#endif
    }
    return "";
}

bool RunAndCapture(const std::vector<std::string>& args, std::string& out_stdout, int timeout_ms = 2000) {
    out_stdout.clear();
    if (args.empty()) return false;
    reproc::process proc;
    reproc::options opts;
    opts.redirect.out.type = reproc::redirect::pipe;
    opts.redirect.err.type = reproc::redirect::discard;
    opts.deadline = reproc::milliseconds(timeout_ms);

    if (proc.start(args, opts)) return false;
    uint8_t buf[2048];
    while (true) {
        auto [n, ec] = proc.read(reproc::stream::out, buf, sizeof(buf));
        if (ec || n == 0) break;
        out_stdout.append(reinterpret_cast<const char*>(buf), n);
    }
    auto [status, wec] = proc.wait(reproc::milliseconds(timeout_ms));
    return !wec && status == 0;
}

} // namespace

std::filesystem::path CompilerCacheManager::GetPrefsFilePath() {
    std::filesystem::path p;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        p = std::filesystem::path(appdata) / "GenieBuild" / "cache.json";
    }
#endif
    if (p.empty()) {
        const char* home = std::getenv("USERPROFILE");
        if (!home) home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            p = std::filesystem::path(home) / ".geniebuild" / "cache.json";
        } else {
            p = std::filesystem::temp_directory_path() / "GenieBuild" / "cache.json";
        }
    }
    return p;
}

CompilerCachePrefs CompilerCacheManager::LoadPrefs() {
    CompilerCachePrefs prefs;
    std::filesystem::path fp = GetPrefsFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(fp, ec) || ec) return prefs;
    try {
        std::ifstream f(fp);
        if (f.is_open()) {
            nlohmann::json j;
            f >> j;
            if (j.is_object()) {
                if (j.contains("launcher") && j["launcher"].is_string()) {
                    prefs.launcher = j["launcher"].get<std::string>();
                }
                if (j.contains("cacheMaxSize") && j["cacheMaxSize"].is_string()) {
                    prefs.maxSize = j["cacheMaxSize"].get<std::string>();
                }
            }
        }
    } catch (...) {}
    return prefs;
}

void CompilerCacheManager::SavePrefs(const CompilerCachePrefs& prefs) {
    std::filesystem::path fp = GetPrefsFilePath();
    std::error_code ec;
    std::filesystem::create_directories(fp.parent_path(), ec);
    try {
        nlohmann::json j = nlohmann::json::object();
        j["launcher"] = prefs.launcher;
        j["cacheMaxSize"] = prefs.maxSize;
        std::ofstream f(fp, std::ios::trunc);
        if (f.is_open()) {
            f << j.dump(2) << std::endl;
        }
    } catch (...) {}
}

DetectedCompilerCache CompilerCacheManager::DetectCache(const CompilerCachePrefs& prefs, bool is_msvc, bool query_version) {
    DetectedCompilerCache res;
    std::string mode = ToLowerCache(prefs.launcher);
    if (mode.empty()) mode = "auto";
    if (mode == "none" || mode == "off") {
        return res;
    }

    auto try_pick = [&](const std::string& exe_spec, CacheKind k, const std::string& label) -> bool {
        std::string found = FindCacheExe(exe_spec);
        if (found.empty()) return false;
        res.kind = k;
        res.name = label;
        res.executablePath = found;
        res.active = true;
        if (query_version) {
            std::string out;
            if (RunAndCapture({ found, "--version" }, out, 1500)) {
                std::istringstream iss(out);
                std::getline(iss, res.version);
                while (!res.version.empty() && (res.version.back() == '\r' || res.version.back() == ' ')) {
                    res.version.pop_back();
                }
            }
        }
        return true;
    };

    if (mode == "auto") {
        if (is_msvc) {
            if (!try_pick("sccache", CacheKind::Sccache, "sccache")) {
                try_pick("ccache", CacheKind::Ccache, "ccache");
            }
        } else {
            if (!try_pick("ccache", CacheKind::Ccache, "ccache")) {
                try_pick("sccache", CacheKind::Sccache, "sccache");
            }
        }
    } else if (mode == "ccache") {
        try_pick("ccache", CacheKind::Ccache, "ccache");
    } else if (mode == "sccache") {
        try_pick("sccache", CacheKind::Sccache, "sccache");
    } else {
        std::string low_name = ToLowerCache(std::filesystem::u8path(prefs.launcher).filename().generic_u8string());
        CacheKind k = (low_name.find("sccache") != std::string::npos) ? CacheKind::Sccache : CacheKind::Ccache;
        try_pick(prefs.launcher, k, k == CacheKind::Sccache ? "sccache" : "ccache");
    }

    return res;
}

void CompilerCacheManager::PrependLauncher(BuildCommand& cmd, const DetectedCompilerCache& cache) {
    if (!cache.active || cache.executablePath.empty() || cmd.program.empty()) return;
    std::string orig_prog = cmd.program;
    cmd.program = cache.executablePath;
    cmd.arguments.insert(cmd.arguments.begin(), orig_prog);
    std::string prefix = cache.executablePath;
    if (prefix.find(' ') != std::string::npos) prefix = "\"" + prefix + "\"";
    cmd.display_command = prefix + " " + cmd.display_command;
}

void CompilerCacheManager::PopulateEnv(
    std::unordered_map<std::string, std::string>& env,
    const DetectedCompilerCache& cache,
    const CompilerCachePrefs& prefs,
    bool uses_pch
) {
    if (!cache.active) return;
    if (cache.kind == CacheKind::Ccache) {
        if (!prefs.maxSize.empty()) {
            env["CCACHE_MAXSIZE"] = prefs.maxSize;
        }
        if (uses_pch) {
            env["CCACHE_SLOPPINESS"] = "pch_defines,time_macros,include_file_mtime";
        }
    } else if (cache.kind == CacheKind::Sccache) {
        if (!prefs.maxSize.empty()) {
            env["SCCACHE_CACHE_SIZE"] = prefs.maxSize;
        }
    }
}

CacheStatsSnapshot CompilerCacheManager::QueryStats(const DetectedCompilerCache& cache) {
    CacheStatsSnapshot snap;
    if (!cache.active || cache.executablePath.empty()) return snap;

    std::string out;
    if (cache.kind == CacheKind::Sccache) {
        if (!RunAndCapture({ cache.executablePath, "--show-stats", "--stats-format=json" }, out)) {
            return snap;
        }
        try {
            nlohmann::json j = nlohmann::json::parse(out);
            uint64_t hits = 0, misses = 0;
            bool got_any = false;
            if (j.contains("stats") && j["stats"].is_object()) {
                const auto& st = j["stats"];
                if (st.contains("cache_hits") && st["cache_hits"].is_object() && st["cache_hits"].contains("counts")) {
                    for (auto it = st["cache_hits"]["counts"].begin(); it != st["cache_hits"]["counts"].end(); ++it) {
                        if (it.value().is_number_unsigned()) hits += it.value().get<uint64_t>();
                    }
                    got_any = true;
                } else if (st.contains("cache_hits") && st["cache_hits"].is_number_unsigned()) {
                    hits = st["cache_hits"].get<uint64_t>();
                    got_any = true;
                }
                if (st.contains("cache_misses") && st["cache_misses"].is_object() && st["cache_misses"].contains("counts")) {
                    for (auto it = st["cache_misses"]["counts"].begin(); it != st["cache_misses"]["counts"].end(); ++it) {
                        if (it.value().is_number_unsigned()) misses += it.value().get<uint64_t>();
                    }
                    got_any = true;
                } else if (st.contains("cache_misses") && st["cache_misses"].is_number_unsigned()) {
                    misses = st["cache_misses"].get<uint64_t>();
                    got_any = true;
                }
            }
            if (got_any) {
                snap.valid = true;
                snap.hits = hits;
                snap.misses = misses;
            }
        } catch (...) {}
    } else if (cache.kind == CacheKind::Ccache) {
        if (!RunAndCapture({ cache.executablePath, "--print-stats" }, out)) {
            return snap;
        }
        std::istringstream iss(out);
        std::string line;
        uint64_t hits = 0, misses = 0;
        bool got_any = false;
        while (std::getline(iss, line)) {
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string key = line.substr(0, tab);
            std::string val = line.substr(tab + 1);
            try {
                uint64_t num = std::stoull(val);
                if (key == "direct_cache_hit" || key == "preprocessed_cache_hit" || key == "cache_hit_direct" || key == "cache_hit_preprocessed") {
                    hits += num;
                    got_any = true;
                } else if (key == "cache_miss") {
                    misses += num;
                    got_any = true;
                }
            } catch (...) {}
        }
        if (got_any) {
            snap.valid = true;
            snap.hits = hits;
            snap.misses = misses;
        }
    }
    return snap;
}

BuildCacheSummary CompilerCacheManager::ComputeDelta(
    const DetectedCompilerCache& cache,
    const CacheStatsSnapshot& before,
    const CacheStatsSnapshot& after
) {
    BuildCacheSummary sum;
    if (!cache.active || !before.valid || !after.valid) return sum;
    if (after.hits < before.hits || after.misses < before.misses) return sum;

    uint64_t dh = after.hits - before.hits;
    uint64_t dm = after.misses - before.misses;
    uint64_t total = dh + dm;
    if (total == 0) return sum;

    sum.valid = true;
    sum.toolName = cache.name;
    sum.hits = dh;
    sum.misses = dm;
    sum.hitRatePercent = (static_cast<double>(dh) * 100.0) / static_cast<double>(total);
    return sum;
}

bool CompilerCacheManager::ClearCache(const DetectedCompilerCache& cache, std::string& out_message) {
    if (!cache.active || cache.executablePath.empty()) {
        out_message = "No compiler cache is currently active.";
        return false;
    }
    std::string out;
    bool ok = false;
    if (cache.kind == CacheKind::Ccache) {
        ok = RunAndCapture({ cache.executablePath, "-C" }, out, 5000);
        out_message = ok ? "ccache cleared." : "Failed to clear ccache.";
    } else if (cache.kind == CacheKind::Sccache) {
        ok = RunAndCapture({ cache.executablePath, "--zero-stats" }, out, 5000);
        out_message = ok ? "sccache stats reset." : "Failed to reset sccache stats.";
    }
    return ok;
}

} // namespace core
