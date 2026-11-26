#pragma once

#include "tool_registry.h"
#include "msvc_env.h"
#include <nlohmann/json_fwd.hpp>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace core {

class Project;

enum class ToolchainFamily {
    Gcc,
    Clang,
    Msvc,
    ClangCl
};

std::string ToolchainFamilyToString(ToolchainFamily fam);
ToolchainFamily ToolchainFamilyFromString(const std::string& str);

// V2-14 Named Toolchain Profile (cross-compilers, sysroot, ld mode, custom env)
struct ToolchainProfile {
    std::string name;
    ToolchainFamily family = ToolchainFamily::Gcc;
    std::string cc;
    std::string cxx;
    std::string ar;
    std::string ld;
    std::string objcopy;
    std::string targetTriple;
    std::string sysroot;
    std::string os;             // "windows", "linux", "macos", "none"
    std::string arch;           // "x64", "x86", "arm64", "arm"
    std::string linkWith = "driver"; // "driver" | "ld"
    std::map<std::string, std::string> env;
    std::vector<std::string> extraCompileFlags;
    std::vector<std::string> extraLinkFlags;
    std::string launcher = "auto";
    std::vector<CustomToolSpec> customTools;

    // Transient runtime metadata
    std::string version;
    std::string idHash8;
    bool detected = false;
    bool isProjectLocal = false;

    void ComputeIdHash();
    bool IsMsvcSyntax() const {
        return family == ToolchainFamily::Msvc || family == ToolchainFamily::ClangCl;
    }
    bool IsClangDriver() const {
        return family == ToolchainFamily::Clang || family == ToolchainFamily::ClangCl;
    }
};

struct ToolchainResolution {
    ToolchainProfile profile;
    bool found = false;
    bool isFallback = false;
    std::string requestedName;
    std::string bannerMessage;
};

struct ToolchainTestResult {
    bool success = false;
    double durationSeconds = 0.0;
    std::string output;
};

// V2-14 Toolchain profile detection, persistence, merging, fallback resolution & testing
class ToolchainManager {
public:
    static std::filesystem::path GetUserToolchainsPath();

    // Reads/writes user-level custom/manual profiles in %APPDATA%/GenieBuild/toolchains.json
    static std::vector<ToolchainProfile> LoadUserProfiles();
    static void SaveUserProfiles(const std::vector<ToolchainProfile>& profiles);

    // Parses / serializes ToolchainProfile JSON (used by both toolchains.json and project.json)
    static ToolchainProfile ProfileFromJson(const nlohmann::json& j, bool is_project_local = false);
    static nlohmann::json ProfileToJson(const ToolchainProfile& p);

    // Scans PATH, well-known install folders, prefixed cross-compilers, and MSVC captures (V2-17)
    static std::vector<ToolchainProfile> DetectAllToolchains(
        bool force_vs_refresh = false,
        MsvcEnvCacheState* out_msvc_state = nullptr
    );

    // Resolves ${root} placeholders in a profile for a specific project root
    static ToolchainProfile ResolveProfileVariables(
        const ToolchainProfile& prof,
        const std::filesystem::path& project_root
    );

    // Merges auto-detected, user-saved, and project-local profiles (project-local overrides same name)
    static std::vector<ToolchainProfile> MergeProfiles(
        const std::vector<ToolchainProfile>& detected,
        const std::vector<ToolchainProfile>& user_profiles,
        const std::vector<ToolchainProfile>& project_profiles,
        const std::filesystem::path& project_root = {}
    );

    // Resolves the requested toolchain profile for a Project (falling back cleanly to same family/arch if missing)
    static ToolchainResolution ResolveForProject(
        const Project& project,
        const std::vector<ToolchainProfile>& available_profiles
    );

    // Runs a quick compile+link (+ run when hosted) sanity check for a toolchain profile
    static ToolchainTestResult TestProfile(const ToolchainProfile& profile);

    // Queries "<cc> -print-libgcc-file-name" using profile env (V2-15 linkLibgcc with linkWith=ld)
    static std::string QueryLibgccPath(const ToolchainProfile& profile);
};

} // namespace core
