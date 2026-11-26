#pragma once

#include "compiler.h"
#include <filesystem>
#include <string>
#include <unordered_map>

namespace core {

enum class CacheKind {
    None,
    Ccache,
    Sccache
};

struct CompilerCachePrefs {
    std::string launcher = "auto";  // "auto", "ccache", "sccache", "none", or custom path
    std::string maxSize = "20G";
};

struct DetectedCompilerCache {
    CacheKind kind = CacheKind::None;
    std::string name;           // "ccache" or "sccache"
    std::string executablePath;
    std::string version;
    bool active = false;
};

struct CacheStatsSnapshot {
    bool valid = false;
    uint64_t hits = 0;
    uint64_t misses = 0;
};

struct BuildCacheSummary {
    bool valid = false;
    std::string toolName;
    uint64_t hits = 0;
    uint64_t misses = 0;
    double hitRatePercent = 0.0;
};

// V2-10 Compiler cache (ccache / sccache)
class CompilerCacheManager {
public:
    static std::filesystem::path GetPrefsFilePath();
    static CompilerCachePrefs LoadPrefs();
    static void SavePrefs(const CompilerCachePrefs& prefs);

    // Resolves active compiler cache launcher based on prefs and compiler family
    static DetectedCompilerCache DetectCache(const CompilerCachePrefs& prefs, bool is_msvc, bool query_version = false);

    // Prepends launcher to a compile BuildCommand (only call for compiles, never links/steps, and AFTER hashing!)
    static void PrependLauncher(BuildCommand& cmd, const DetectedCompilerCache& cache);

    // Populates CCACHE_* / SCCACHE_* env vars
    static void PopulateEnv(
        std::unordered_map<std::string, std::string>& env,
        const DetectedCompilerCache& cache,
        const CompilerCachePrefs& prefs,
        bool uses_pch
    );

    // Queries current stats from ccache / sccache (returns valid=false if unavailable)
    static CacheStatsSnapshot QueryStats(const DetectedCompilerCache& cache);

    // Computes delta between before/after snapshots
    static BuildCacheSummary ComputeDelta(
        const DetectedCompilerCache& cache,
        const CacheStatsSnapshot& before,
        const CacheStatsSnapshot& after
    );

    // Clears cache stats / contents (ccache -C or sccache --zero-stats)
    static bool ClearCache(const DetectedCompilerCache& cache, std::string& out_message);
};

} // namespace core
