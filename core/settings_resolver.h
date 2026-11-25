#pragma once

#include "target.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>

namespace core {

class Project;

// V2-4 Base Configuration Preset
enum class BaseConfigPreset {
    Debug,
    Release,
    RelWithDebInfo,
    MinSize
};

std::string BaseConfigPresetToString(BaseConfigPreset p);
BaseConfigPreset BaseConfigPresetFromString(const std::string& str, const std::string& fallback_cfg_name = "");

// Tracked flag entry for Effective Flags panel (V2-3)
struct FlagWithOrigin {
    std::string flag;
    std::string origin;     // e.g. "project", "config: Development", "target: renderer (private)", "inherited: core (public)", "override: src/simd/**"
    std::string reason;     // e.g. "preset: relWithDebInfo", "overridden by override: src/hot.cpp"
    bool overridden = false;
};

// Fully resolved settings for a single translation unit (or override signature group)
struct EffectiveFileSettings {
    EffectiveTargetSettings base;
    std::string configName;
    std::string basePreset; // "debug" | "release" | "relWithDebInfo" | "minSize"
    std::string optimize;   // "O0" | "O1" | "O2" | "O3" | "Os"
    std::string debugInfo;  // "none" | "line" | "full"
    std::string runtime;    // "dynamic" | "static"
    std::string lto;        // "off" | "thin" | "full"
    bool defineNdebug = false;
    bool defineDebug = false;
    bool excludeFromPch = false;
    bool excludeFromUnity = false;

    std::vector<std::string> matchedOverridePatterns;
    std::vector<std::string> matchedOverrideSummaries;
    std::vector<FlagWithOrigin> effectiveFlags;
};

// Resolved configuration parameters (merged with base preset)
struct ResolvedConfigParams {
    std::string name;
    BaseConfigPreset base = BaseConfigPreset::Debug;
    std::string optimize;   // "O0".."Os"
    std::string debugInfo;  // "none" | "line" | "full"
    std::string runtime;    // "dynamic" | "static"
    std::string lto;        // "off" | "thin" | "full"
    bool defineNdebug = false;
    bool defineDebug = false;
};

ResolvedConfigParams ResolveConfigParams(const Project& project, const std::string& config_name);

// Generates compiler/linker flags for a ResolvedConfigParams (for preview & command formulation)
std::vector<std::string> GetConfigCompileFlags(
    const ResolvedConfigParams& cfg,
    const std::string& optimize_override,
    const std::string& debug_info_override,
    bool is_msvc,
    bool is_clang
);

std::vector<std::string> GetConfigLinkFlags(
    const ResolvedConfigParams& cfg,
    TargetKind kind,
    bool is_msvc,
    bool is_clang
);

// Resolves a single file's EffectiveFileSettings (layered resolution order + origins)
EffectiveFileSettings ResolveFileSettings(
    const Project& project,
    const Target& target,
    const EffectiveTargetSettings& target_eff,
    const std::string& file_rel_path,
    const std::string& config_name,
    const std::string& compiler_name,
    bool is_c_source
);

// Batch resolution using override signature grouping (avoids resolving 50k times independently)
std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>> ResolveTargetFilesGrouped(
    const Project& project,
    const Target& target,
    const EffectiveTargetSettings& target_eff,
    const std::vector<std::string>& files,
    const std::string& config_name,
    const std::string& compiler_name
);

// Validates overrides against target's files (warns on unmatched overrides)
std::vector<std::string> ValidateTargetOverrides(const Target& target);

// Suggests closest known key for unknown JSON field in an override ("did you mean ...?")
std::string SuggestOverrideFieldName(const std::string& unknown_key);

// Checks MSVC runtime CRT consistency across target and its static dependencies
bool CheckRuntimeConsistency(
    const Project& project,
    const std::vector<const Target*>& build_order,
    const std::unordered_map<std::string, const Target*>& target_map,
    const std::string& config_name,
    std::string& out_error
);

} // namespace core
