#pragma once

#include "compiler.h"
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>

namespace core {

class Project;
struct Target;
struct ToolchainProfile;
struct RunConfig;
class DirectoryCache;

enum class StepPhase {
    PreBuild,
    PostBuild
};

std::string StepPhaseToString(StepPhase phase);
StepPhase StepPhaseFromString(const std::string& str);

// V2-6 Pre-build / Post-build step definition
struct BuildStep {
    std::string name;
    StepPhase phase = StepPhase::PreBuild;
    std::string foreachPattern;      // e.g. "shaders/**/*.{vert,frag,comp}"
    std::vector<std::string> inputs; // e.g. ["assets/**"]
    std::string command;             // e.g. "glslc -MD -MF {out}.d {in} -o {out}"
    std::vector<std::string> outputs;// e.g. ["{bindir}/shaders/{name}.spv"]
    std::string depfile;             // e.g. "{out}.d"
    std::string condition;           // optional V2-5 "if" expression
    bool generatesSources = false;
    bool shell = false;
    bool always = false;
};

struct StepExpandContext {
    std::string root;
    std::string config;
    std::string target;
    std::string toolchain;
    std::string toolchainHash8;
    std::string objcopyTool; // V2-16 {objcopy}
    std::string ldTool;      // V2-16 {ld}
    std::string genDir;
    std::string binDir;
    std::string libDir;
    std::string objDir;
    std::string binFile;
    std::unordered_map<std::string, std::string> targetBinFiles;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

struct ExpandedStepJob {
    std::string targetName;
    std::string stepName;
    size_t stepIndex = 0;
    size_t jobIndex = 0;
    StepPhase phase = StepPhase::PreBuild;
    std::string label;                  // e.g. "basic.frag" or ""
    std::vector<std::string> inputs;    // normalized relative or absolute paths
    std::vector<std::string> outputs;   // normalized relative or absolute paths
    std::string depfile;                // normalized path if non-empty
    BuildCommand cmd;
    std::string cmdHash;
    std::string stateKey;
    bool shell = false;
    bool always = false;
    bool generatesSources = false;
    std::vector<std::string> referencedTargets; // from {bin:<target>}
};

struct StepDryRunEntry {
    std::string targetName;
    std::string stepName;
    std::string label;
    StepPhase phase = StepPhase::PreBuild;
    bool active = true;
    bool wouldRun = false;
    std::string reason;
    std::string expandedCommand;
    std::vector<std::string> outputs;
};

// Returns target's generated-files directory relative to project root: ".geniebuild/gen/<Config>/<target>"
std::string GetTargetGenDirRel(const std::string& config, const std::string& target_name);

// Returns true if file extension is C/C++ compilable (.c, .cpp, .cc, .cxx)
bool IsStepGeneratedSourcePath(const std::string& path_str);

// Expands §2.3 project variables (${root}, ${config}, ${target}, ${toolchain}, ${env:NAME})
std::string ExpandProjectVariables(
    const std::string& text,
    const Project& project,
    const std::string& target_name,
    std::vector<std::string>* out_warnings = nullptr
);

// Expands V2-6 step placeholders ({in}, {out}, {stem}, {name}, {dir}, {gen}, {bindir}, {libdir}, {objdir}, {bin}, {bin:T}, {root}, {config}, {target})
std::string ExpandStepPlaceholders(
    const std::string& text,
    const StepExpandContext& ctx,
    bool quote_paths_with_spaces = false,
    std::vector<std::string>* out_warnings = nullptr
);

// Extracts any target names referenced via {bin:<target>} in a step
std::vector<std::string> ExtractStepBinTargetRefs(const BuildStep& step);

// Builds a BuildCommand from an expanded command string (handling shell vs direct execution)
BuildCommand BuildStepCommand(const std::string& expanded_cmd, bool shell);

// Builds a StepExpandContext for a given project and target
StepExpandContext MakeStepExpandContext(
    const Project& project,
    const Target& target,
    const std::unordered_map<std::string, std::string>& target_bin_map,
    const ToolchainProfile* toolchain = nullptr
);

// V2-16 Prebuilt step presets (ELF->bin, GRUB ISO, Limine ISO, Copy folder)
struct StepPreset {
    std::string id;
    std::string label;
    std::string requiredTool;
    std::string missingHint;
    BuildStep step;
};

std::vector<StepPreset> GetBuiltInStepPresets();

// Expands a Target RunConfig / Debug RunConfig into executable BuildCommand, cwd, and env
BuildCommand FormulateRunCommand(
    const Project& project,
    const Target& target,
    const RunConfig& run_cfg,
    const ToolchainProfile* toolchain,
    std::filesystem::path& out_cwd,
    std::unordered_map<std::string, std::string>& out_env
);

// Expands a BuildStep into concrete jobs (fanned out if foreach is set)
std::vector<ExpandedStepJob> ExpandStepJobs(
    const Project& project,
    const Target& target,
    size_t step_index,
    const BuildStep& step,
    DirectoryCache& cache,
    const std::unordered_map<std::string, std::string>& target_bin_map,
    const std::vector<std::string>* virtual_outputs = nullptr,
    std::vector<std::string>* out_warnings = nullptr,
    const ToolchainProfile* toolchain = nullptr
);

// Evaluates whether a step job needs to run, returning a human-readable reason
bool CheckStepJobNeedsRun(
    const Project& project,
    const ExpandedStepJob& job,
    const std::unordered_map<std::string, std::string>& prev_state,
    std::string& out_reason
);

// Parses a Makefile-style .d depfile generated by a custom step
std::vector<std::string> ParseStepDepfile(const std::filesystem::path& dep_path);

// Checks if a step's program is available on PATH or in project/planned outputs
bool IsStepProgramAvailable(
    const std::string& program,
    const std::filesystem::path& project_root,
    const std::vector<std::string>& planned_outputs,
    std::string& out_resolved
);

// Preview string for the first job of a step (used by Settings UI)
std::string PreviewStepCommand(
    const Project& project,
    const Target& target,
    const BuildStep& step,
    DirectoryCache& cache,
    std::vector<std::string>* out_outputs = nullptr
);

// Performs a non-mutating dry run of all steps in a target and returns status/reasons
std::vector<StepDryRunEntry> DryRunTargetSteps(
    const Project& project,
    const Target& target,
    const std::vector<Target>& all_targets,
    DirectoryCache& cache
);

// Manages persistent folder trust for projects containing custom build steps (V2-6 security)
class TrustStore {
public:
    static std::filesystem::path GetTrustFilePath();
    static bool IsTrusted(const std::filesystem::path& project_root);
    static void SetTrusted(const std::filesystem::path& project_root, bool trusted);
    static size_t CountCustomCommands(const Project& project);
};

} // namespace core
