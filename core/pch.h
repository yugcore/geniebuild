#pragma once

#include "compiler.h"
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

class Project;
struct Target;
struct EffectiveTargetSettings;
struct EffectiveFileSettings;

struct PchConfig {
    std::string header;             // C++ precompiled header path (e.g. "src/core/pch.h")
    std::string cHeader;            // Optional C precompiled header path
    std::string source;             // Optional explicit MSVC PCH source (.cpp)
    std::vector<std::string> exclude; // Glob patterns excluded from PCH

    bool IsEmpty() const {
        return header.empty() && cHeader.empty() && source.empty() && exclude.empty();
    }
};

struct PchFileUsage {
    std::string file;
    bool isC = false;
    bool usesPch = false;
    std::string excludeReason; // empty if usesPch == true
};

struct PchArtifactPlan {
    bool enabled = false;
    bool isC = false;
    std::string headerRel;
    std::filesystem::path pchDir;
    std::filesystem::path forwardingOrSourceFile; // gcc: pch.h, msvc: pch.cpp, clang: source header
    std::filesystem::path pchOutputFile;          // gcc: pch.h.gch, clang/msvc: pch.pch
    std::filesystem::path msvcObjectFile;         // msvc: pch.obj (linked into target)
    std::filesystem::path depfilePath;
    BuildCommand compileCmd;
    std::string cmdHash;
    std::string baseSignature;
};

struct TargetPchAnalysis {
    PchArtifactPlan cxxPch;
    PchArtifactPlan cPch;
    std::vector<PchFileUsage> fileUsages;
    size_t includedCount = 0;
    size_t totalCount = 0;
};

// V2-8 Precompiled Header support
class PchManager {
public:
    // Computes PCH-relevant compatibility hash (defines, std, optimize, -m*/arch, -f* codegen, runtime)
    static std::string ComputePchSignature(
        const EffectiveFileSettings& eff,
        bool is_c_source,
        bool is_msvc,
        std::string* out_diag_summary = nullptr
    );

    // Explains first differing PCH-relevant category between base and file settings
    static std::string DescribeSignatureDiff(
        const EffectiveFileSettings& base,
        const EffectiveFileSettings& file,
        bool is_c_source,
        bool is_msvc
    );

    // Analyzes target sources and builds PCH compile plan + per-file compatibility list
    static TargetPchAnalysis AnalyzeAndPlanTargetPch(
        const Project& project,
        const Target& target,
        const EffectiveTargetSettings& target_eff,
        const std::vector<std::string>& source_files,
        const std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>>& file_settings,
        const CompilerInfo& cxx_comp,
        const CompilerInfo& c_comp,
        const std::string& config,
        bool is_msvc,
        bool is_clang,
        bool ccache_active = false,
        bool write_generated_files = true
    );

    // Appends PCH-consumption flags to a TU's compile command
    static void ApplyPchToCompileCommand(
        BuildCommand& cmd,
        const PchArtifactPlan& pch_plan,
        bool is_msvc,
        bool is_clang,
        bool ccache_active = false
    );

    // Writes file only if content differs (preserves mtime on unchanged PCH wrappers)
    static bool WriteFileIfDifferent(const std::filesystem::path& path, const std::string& content);
};

} // namespace core
