#pragma once

#include "source_rules.h"
#include "build_steps.h"
#include "condition.h"
#include "pch.h"
#include "unity.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>

namespace core {

class Project;

enum class TargetKind {
    Executable,
    Static,
    Shared,
    Interface
};

std::string TargetKindToString(TargetKind kind);
TargetKind TargetKindFromString(const std::string& str);

// Reusable settings block for target.public, target.private, and target.configs[cfg]
struct SettingsBlock {
    std::vector<std::string> includeDirs;
    std::vector<std::string> systemIncludeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
    std::vector<std::string> libDirs;
    std::vector<std::string> libs;
    std::map<std::string, std::string> libConditions; // optional V2-5 "if" per library entry
    std::vector<std::string> copyDlls;
    std::string cxxStd;
    std::string cStd;
    std::string warnings;
    std::string runtime; // "", "dynamic", "static"

    bool IsEmpty() const {
        return includeDirs.empty() && systemIncludeDirs.empty() && defines.empty() &&
               compileFlags.empty() && linkFlags.empty() && libDirs.empty() &&
               libs.empty() && libConditions.empty() && copyDlls.empty() && cxxStd.empty() &&
               cStd.empty() && warnings.empty() && runtime.empty();
    }
};

// Configuration-specific overrides inside a FlagOverride (V2-3)
struct OverrideConfigSettings {
    std::vector<std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
    std::string optimize;  // "", "O0", "O1", "O2", "O3", "Os"
    std::string debugInfo; // "", "none", "line", "full"
    std::string warnings;
    std::string cxxStd;
    std::string cStd;

    bool IsEmpty() const {
        return defines.empty() && compileFlags.empty() && linkFlags.empty() &&
               optimize.empty() && debugInfo.empty() && warnings.empty() &&
               cxxStd.empty() && cStd.empty();
    }
};

// V2-3 Per-folder / per-file flag override rule
struct FlagOverride {
    std::string match;     // glob pattern, e.g. "src/simd/**" or "src/hot/mathlib.cpp"
    std::string condition; // optional "if" expression
    SettingsBlock settings;
    std::string optimize;  // "", "O0", "O1", "O2", "O3", "Os"
    std::string debugInfo; // "", "none", "line", "full"
    bool excludeFromPch = false;
    bool excludeFromUnity = false;
    std::map<std::string, OverrideConfigSettings> configs;
};

// Sanitized folder segment for a configuration name ([A-Za-z0-9_-])
std::string SanitizeConfigDirName(const std::string& config_name);

// V2-16 Run / Debug command configuration per target
struct RunConfig {
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    std::map<std::string, std::string> env;
    bool buildBeforeRun = true;

    bool IsEmpty() const {
        return command.empty() && args.empty() && cwd.empty() && env.empty() && buildBeforeRun;
    }
};

enum class KernelBootProtocol {
    Multiboot2,
    Limine,
    Minimal
};

struct Target {
    std::string name;
    TargetKind kind = TargetKind::Executable;
    std::string outputName;
    std::string condition; // optional V2-5 "if" expression
    bool exportAll = false; // MinGW/GCC: -Wl,--export-all-symbols for shared targets

    // V2-15 Freestanding / kernel preset fields
    bool freestanding = false;
    std::string linkerScript;
    std::string entry;
    std::vector<std::string> cxxFlags;
    bool linkLibgcc = false;

    // V2-16 Run and external debug configurations
    RunConfig run;
    RunConfig debug;

    std::vector<SourceRule> sources;
    std::vector<std::string> files;
    std::vector<std::string> exclude;
    std::unordered_set<std::string> disabledFiles;

    std::vector<std::string> deps;
    std::vector<std::string> publicDeps;

    SettingsBlock pub;
    SettingsBlock priv;
    std::map<std::string, SettingsBlock> configs;
    std::vector<FlagOverride> overrides;
    std::vector<BuildStep> steps; // V2-6 pre-build and post-build steps
    PchConfig pch;                // V2-8 precompiled headers
    UnityConfig unity;            // V2-9 unity/jumbo builds

    // Transient expanded state (populated by Project::ExpandAllSourceRules)
    std::vector<std::string> expandedFiles;
    std::unordered_set<std::string> expandedFilesSet;
    std::vector<std::string> headerFiles;
    std::vector<size_t> ruleMatchCounts;
    std::unordered_set<std::string> missingFilesSet;
    std::vector<std::string> missingFiles;

    std::string GetEffectiveOutputName() const;
    std::string GetExportsMacro() const;
    bool HasV2Fields() const;

    std::filesystem::path GetObjectDir(const std::filesystem::path& project_root, const std::string& config, const std::string& tc_hash8 = "") const;
    std::filesystem::path GetGenDir(const std::filesystem::path& project_root, const std::string& config) const;
    std::filesystem::path GetBinaryPath(const std::filesystem::path& abs_output_dir, const std::string& config, bool is_msvc) const;
    std::filesystem::path GetImportLibPath(const std::filesystem::path& abs_output_dir, const std::string& config, bool is_msvc) const;

    std::vector<std::string> GetCompiledFiles() const;
    size_t GetEnabledFilesCount() const;
    bool IsFileEnabled(const std::string& rel_path) const;
};

struct EffectiveTargetSettings {
    std::vector<std::string> includeDirs;
    std::vector<std::string> systemIncludeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> cxxFlags;     // V2-15 C++-only flags (e.g. -fno-exceptions -fno-rtti)
    std::vector<std::string> linkFlags;
    std::vector<std::string> libDirs;
    std::vector<std::string> libs;
    std::vector<std::string> copyDlls;
    std::string cxxStd;
    std::string cStd;
    std::string warnings;
    bool freestanding = false;
    std::string linkerScript;
    std::string entry;
    bool linkLibgcc = false;

    // Dependency outputs in link order (dependents before dependencies for GNU ld)
    std::vector<std::filesystem::path> depLinkLibraries;
    std::vector<std::filesystem::path> staticDepLibraries;
    std::vector<std::pair<std::string, std::filesystem::path>> sharedDepImportLibs;
};

// Validates targets (names, duplicate outputs, missing deps, dependency cycles, V2-5 conditions) and
// returns build order (dependencies before dependents). If root_target is non-empty,
// includes only root_target and its transitive dependencies.
bool ResolveTargetBuildOrder(
    const std::vector<Target>& targets,
    const std::string& root_target,
    std::vector<const Target*>& out_order,
    std::string& out_error,
    const ConditionContext* cond_ctx = nullptr
);

// Validates all condition expressions across targets, rules, overrides, steps, and libs.
bool ValidateAllTargetConditions(
    const std::vector<Target>& targets,
    std::string& out_error
);

// Resolves effective compile + link settings for target (with transitive usage propagation)
EffectiveTargetSettings ResolveEffectiveTargetSettings(
    const Project& project,
    const Target& target,
    const std::unordered_map<std::string, const Target*>& target_map,
    const std::string& config,
    bool is_msvc,
    bool static_needs_pic = false
);

// Returns true if any shared target transitively depends on static_target_name
bool IsStaticTargetConsumedByShared(
    const std::vector<Target>& targets,
    const std::string& static_target_name
);

// V2-15 Generates starter kernel files (once, never overwriting existing files) and populates Target
void CreateKernelStarterTemplate(
    const std::filesystem::path& project_root,
    const std::string& target_name,
    KernelBootProtocol protocol,
    Target& out_target,
    std::vector<std::string>* out_created_files = nullptr
);

} // namespace core
