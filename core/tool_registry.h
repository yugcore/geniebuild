#pragma once

#include "compiler.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace core {

class Project;
struct Target;
struct EffectiveTargetSettings;

enum class SourceFileKind {
    Unknown,
    C,
    Cxx,
    Rc,       // .rc
    AsmPre,   // .S (preprocessed assembly)
    AsmRaw,   // .s (raw assembly)
    AsmNasm,  // .asm (nasm or ml64)
    Cuda,     // .cu
    Ispc,     // .ispc
    Custom    // user customTools
};

struct CustomToolSpec {
    std::string ext;     // e.g. ".glsl"
    std::string program; // e.g. "glslc"
    std::string args;    // e.g. "-c {in} -o {out}"
    std::string output;  // e.g. "{objdir}/{name}.spv"
    bool link = false;   // whether output is passed to the linker
};

struct DetectedToolInfo {
    std::string id;       // "windres", "rc", "nasm", "ml64", "nvcc", "ispc"
    std::string path;     // resolved executable path (or empty if not found)
    std::string version;  // first line of --version / /? (optional, cached)
    bool found = false;
};

struct ToolCompileSpec {
    bool supported = true;
    bool linkOutput = true;
    bool usesDepfile = false;
    bool usesRcHash = false;
    std::string errorMessage; // set if unsupported or missing tool
    std::string requiredTool;
    std::filesystem::path outputPath;
    std::filesystem::path depfilePath;
    std::filesystem::path generatedHeaderPath; // for .ispc
    BuildCommand command;
};

// V2-7 Built-in + Custom Tool Registry
class ToolRegistry {
public:
    // Classifies source file extension (preserving case for .S vs .s)
    static SourceFileKind ClassifySourceFile(
        const std::string& path_str,
        const std::vector<CustomToolSpec>& custom_tools = {}
    );

    // Returns true if extension is any built-in or custom compilable source type
    static bool IsAnyCompilableSource(
        const std::string& path_str,
        const std::vector<CustomToolSpec>& custom_tools = {}
    );

    // Detects windres, rc, nasm, ml64, nvcc, ispc on PATH (and optional overrides)
    static std::vector<DetectedToolInfo> DetectTools(
        const std::map<std::string, std::string>& tool_overrides = {},
        bool query_versions = false
    );

    // Resolves a single tool executable (checking overrides first, then PATH)
    static std::string ResolveToolExecutable(
        const std::string& tool_id,
        const std::map<std::string, std::string>& tool_overrides = {}
    );

    // Formulates compile command, output path, and dep mode for non-C/C++ or custom source files
    static ToolCompileSpec FormulateExtraSourceCommand(
        const std::string& source_rel_path,
        const Target& target,
        const EffectiveTargetSettings& eff,
        const Project& project,
        const std::filesystem::path& obj_dir,
        const std::filesystem::path& gen_dir,
        const std::string& compiler_name,
        bool is_msvc,
        const std::map<std::string, std::string>& tool_overrides,
        const std::vector<CustomToolSpec>& custom_tools,
        const std::string& asm_mode = "auto" // "auto", "nasm", "ml64"
    );

    // Scans a .rc file for ICON, BITMAP, RCDATA, CURSOR, FONT, MANIFEST, and #include paths
    static std::vector<std::string> ScanRcDependencies(
        const std::filesystem::path& rc_full_path,
        const std::filesystem::path& project_root,
        const std::vector<std::string>& include_dirs
    );

    // Computes a deterministic content hash of an .rc file and all referenced asset/include files
    static std::string ComputeRcContentHash(
        const std::filesystem::path& rc_full_path,
        const std::filesystem::path& project_root,
        const std::vector<std::string>& include_dirs
    );

    // Returns type breakdown summary string e.g. "412 C++ · 3 asm · 1 rc"
    static std::string FormatTypeBreakdown(
        const std::vector<std::string>& files,
        const std::vector<CustomToolSpec>& custom_tools = {}
    );
};

} // namespace core
