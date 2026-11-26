#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <memory>

namespace core {

class Project; // Forward declaration
struct Target;
struct EffectiveTargetSettings;
struct EffectiveFileSettings;
struct ToolchainProfile;
struct ToolchainResolution;
struct MsvcEnvCacheState;

// Identifies which language(s) a compiler toolchain handles
enum class CompilerLanguage {
    C,
    Cxx,
    Both // e.g. MSVC 'cl' can compile both C and C++
};

// Characterizes the language mix of a project's build list
enum class SourceLanguageType {
    Empty,
    C,
    Cxx,
    Mixed // Mixed builds supported in §5.1.6
};

// Information about a compiler detected on the host system
struct CompilerInfo {
    std::string name;                     // e.g. "g++", "clang++", "cl", "gcc", "clang"
    std::filesystem::path executable_path; // Full resolved path from PATH
    CompilerLanguage language = CompilerLanguage::Cxx;
    std::shared_ptr<const ToolchainProfile> profile; // V2-14 optional backing profile

    bool IsCxxCompatible() const {
        return language == CompilerLanguage::Cxx || language == CompilerLanguage::Both;
    }

    bool IsCCompatible() const {
        return language == CompilerLanguage::C || language == CompilerLanguage::Both;
    }
};

// Represents a fully assembled compiler command ready for execution
struct BuildCommand {
    std::string program;                // Path or executable name to launch
    std::vector<std::string> arguments; // Arguments vector (for reproc process launch)
    std::string display_command;        // Formatted command line string for display in Output pane
    std::string hash_salt;              // Extra version/sysroot/triple salt hashed in ComputeCommandHash
};

// Manages compiler detection from PATH and command-line formulation.
// Core component: strictly independent of UI and windowing libraries.
class CompilerManager {
public:
    CompilerManager();
    ~CompilerManager();

    // Searches system PATH + VS/cross installs for compilers and toolchain profiles
    void DetectCompilers(bool force_vs_refresh = false);

    // Returns all detected compilers
    const std::vector<CompilerInfo>& GetDetectedCompilers() const { return m_detected_compilers; }

    // V2-14 / V2-17 Toolchain profile queries & resolution
    std::vector<ToolchainProfile> GetDetectedToolchains() const;
    std::vector<ToolchainProfile> GetMergedToolchains(const Project& project) const;
    ToolchainResolution ResolveProjectToolchain(const Project& project) const;
    CompilerInfo MakeCompilerInfoFromProfile(const ToolchainProfile& profile, bool for_c = false) const;
    MsvcEnvCacheState GetMsvcEnvState() const;
    void DismissMsvcBanner();

    // Returns true if at least one compiler is available
    bool HasCompilers() const { return !m_detected_compilers.empty(); }

    // Finds a detected compiler by name (case-insensitive)
    const CompilerInfo* FindCompiler(const std::string& name) const;

    // Returns the standard list of compiler names searched for (for UI error explanations)
    static std::vector<std::string> GetSearchedCompilerNames();

    // Inspects a list of source files to determine whether it is C, C++, Mixed, or Empty
    static SourceLanguageType DetermineLanguage(const std::vector<std::string>& files);

    // Checks MSVC Developer Command Prompt environment (Spec §5.1 guards)
    static bool CheckMsvcEnvironment(std::string& out_error_message);

    // Validates V2-12 linker options and tool availability (lld, mold, gold, fastLink, splitDwarf)
    static bool ValidateLinkerOptions(
        const Project& project,
        bool is_msvc,
        bool is_clang,
        std::string& out_error_message
    );

    // Checks if a specific linker executable (lld / lld-link / mold / ld.gold) is available on PATH
    static bool IsLinkerAvailable(const std::string& linker_mode, bool is_msvc);

    // Splits raw flag strings shell-style keeping quotes intact (Spec §5.1.3)
    static std::vector<std::string> SplitFlags(const std::string& raw_flags);

    // Computes a deterministic 64-bit hex hash of a command's program + arguments (Spec §5.1.5)
    static std::string ComputeCommandHash(const BuildCommand& cmd);

    // Computes a deterministic 64-bit hex hash of the link command + sorted object list (E1)
    static std::string ComputeLinkHash(
        const BuildCommand& link_cmd,
        const std::vector<std::filesystem::path>& object_files
    );

    // Formulates the incremental compile command for a single source file (Spec §5.1.5)
    static BuildCommand FormulateCompileCommand(
        const CompilerInfo& compiler,
        const std::string& source_rel_path,
        const std::filesystem::path& object_path,
        const Project& project
    );

    // Target-aware compile command using resolved EffectiveTargetSettings (V2-2)
    static BuildCommand FormulateCompileCommand(
        const CompilerInfo& compiler,
        const std::string& source_rel_path,
        const std::filesystem::path& object_path,
        const Project& project,
        const EffectiveTargetSettings& eff
    );

    // File-override-aware compile command using EffectiveFileSettings (V2-3 + V2-4)
    static BuildCommand FormulateCompileCommand(
        const CompilerInfo& compiler,
        const std::string& source_rel_path,
        const std::filesystem::path& object_path,
        const Project& project,
        const EffectiveFileSettings& file_eff
    );

    // Formulates the final link command combining all compiled objects (Spec §5.1.5)
    static BuildCommand FormulateLinkCommand(
        const CompilerInfo& linker,
        const std::vector<std::filesystem::path>& object_files,
        const std::filesystem::path& output_binary_path,
        const Project& project,
        bool write_rsp = true
    );

    // Target-aware link / archive command for executable, static, or shared targets (V2-2)
    static BuildCommand FormulateLinkCommand(
        const CompilerInfo& linker,
        const Target& target,
        const EffectiveTargetSettings& eff,
        const std::vector<std::filesystem::path>& object_files,
        const std::filesystem::path& output_binary_path,
        const Project& project,
        bool write_rsp = true
    );

    // Formulates the single-command invocation for the given compiler and source files (legacy/fallback)
    static BuildCommand FormulateCommand(
        const CompilerInfo& compiler,
        const std::vector<std::string>& files,
        const std::filesystem::path& project_root,
        const std::filesystem::path& output_binary_path
    );

private:
    struct ToolchainCacheState;
    std::vector<CompilerInfo> m_detected_compilers;
    std::shared_ptr<ToolchainCacheState> m_tc_state;
};

} // namespace core
