#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

class Project;
class DepLog;
struct Target;
struct EffectiveTargetSettings;
struct EffectiveFileSettings;

struct UnityConfig {
    bool enabled = false;
    size_t batchSize = 16;
    std::vector<std::string> exclude;
    bool adaptive = true;
};

struct UnityBatch {
    std::string batchName;                   // e.g. "core_0"
    std::string generatedFileRel;            // ".geniebuild/unity/<Config>/<target>/<hash>_<k>.cpp"
    std::filesystem::path generatedFileAbs;
    bool isC = false;
    bool usesPch = false;
    std::vector<std::string> memberFiles;    // original source paths included in this batch
    std::shared_ptr<const EffectiveFileSettings> settings;
};

struct UnityPlan {
    std::vector<UnityBatch> batches;
    std::vector<std::string> standaloneFiles; // hot files + excluded + non-C/C++ + singletons
    std::vector<std::string> hotFiles;        // adaptive hot files compiled standalone this build
    size_t totalBatchCount = 0;
};

// V2-9 Unity / Jumbo build batch planner
class UnityManager {
public:
    // Plans unity batches for a target (folder-first, same lang/settings/PCH, adaptive hot tracking)
    static UnityPlan PlanTargetUnity(
        const Project& project,
        const Target& target,
        const std::vector<std::string>& compiled_files,
        const std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>>& file_settings,
        const std::unordered_map<std::string, bool>& file_uses_pch,
        const std::string& config,
        const std::string& compiler_name,
        bool is_msvc,
        const DepLog* dep_log = nullptr,
        bool write_files = true
    );

    // Records hot-file graduation state after a successful build
    static void SaveAdaptiveHotState(
        const Project& project,
        const Target& target,
        const std::string& config,
        const UnityPlan& plan
    );
};

} // namespace core
