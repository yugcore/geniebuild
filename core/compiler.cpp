#include "compiler.h"
#include "project.h"
#include "target.h"
#include "settings_resolver.h"

#include <cstdlib>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

namespace core {

static std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

CompilerManager::CompilerManager() {
    DetectCompilers();
}

std::vector<std::string> CompilerManager::GetSearchedCompilerNames() {
    return { "g++", "clang++", "cl", "gcc", "clang" };
}

bool CompilerManager::CheckMsvcEnvironment(std::string& out_error_message) {
#if defined(_WIN32)
    const char* inc = std::getenv("INCLUDE");
    const char* lib = std::getenv("LIB");
    if (!inc || inc[0] == '\0' || !lib || lib[0] == '\0') {
        out_error_message = "MSVC (cl.exe) requires the Visual Studio Developer Command Prompt environment.\n"
                            "The INCLUDE or LIB environment variable is missing.\n"
                            "Please run GenieBuild from 'x64 Native Tools Command Prompt for VS' or configure INCLUDE/LIB.";
        return false;
    }
#endif
    return true;
}

std::vector<std::string> CompilerManager::SplitFlags(const std::string& raw_flags) {
    std::vector<std::string> result;
    std::string current;
    bool in_quotes = false;
    char quote_char = '\0';

    for (size_t i = 0; i < raw_flags.size(); ++i) {
        char c = raw_flags[i];
        if ((c == '"' || c == '\'') && !in_quotes) {
            in_quotes = true;
            quote_char = c;
        } else if (in_quotes && c == quote_char) {
            in_quotes = false;
            quote_char = '\0';
        } else if (std::isspace(static_cast<unsigned char>(c)) && !in_quotes) {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

static void HashStringFnv1a(uint64_t& hash, const std::string& str) {
    for (unsigned char c : str) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    hash ^= 0xFF;
    hash *= 1099511628211ULL;
}

static std::string FormatHash64(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

std::string CompilerManager::ComputeCommandHash(const BuildCommand& cmd) {
    uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit
    HashStringFnv1a(hash, cmd.program);
    for (const auto& arg : cmd.arguments) {
        HashStringFnv1a(hash, arg);
    }
    return FormatHash64(hash);
}

std::string CompilerManager::ComputeLinkHash(
    const BuildCommand& link_cmd,
    const std::vector<std::filesystem::path>& object_files
) {
    uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit
    HashStringFnv1a(hash, link_cmd.program);
    for (const auto& arg : link_cmd.arguments) {
        HashStringFnv1a(hash, arg);
    }
    std::vector<std::string> sorted_objs;
    sorted_objs.reserve(object_files.size());
    for (const auto& obj : object_files) {
        std::string obj_s = obj.generic_u8string();
        std::replace(obj_s.begin(), obj_s.end(), '\\', '/');
        sorted_objs.push_back(std::move(obj_s));
    }
    std::sort(sorted_objs.begin(), sorted_objs.end());
    for (const auto& obj_s : sorted_objs) {
        HashStringFnv1a(hash, obj_s);
    }
    return FormatHash64(hash);
}

static void AppendArg(BuildCommand& cmd, std::ostringstream& ss, const std::string& arg) {
    cmd.arguments.push_back(arg);
    if (arg.find(' ') != std::string::npos && arg.front() != '"' && arg.back() != '"') {
        ss << " \"" << arg << "\"";
    } else {
        ss << " " << arg;
    }
}

static std::string SanitizeTargetFileToken(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

static std::string ResolveCompanionTool(const CompilerInfo& linker, const std::string& tool_base) {
    std::error_code ec;
    if (!linker.executable_path.empty()) {
        std::filesystem::path dir = linker.executable_path.parent_path();
#if defined(_WIN32)
        std::filesystem::path exe_cand = dir / (tool_base + ".exe");
        if (std::filesystem::exists(exe_cand, ec) && !ec) {
            return exe_cand.generic_u8string();
        }
#endif
        std::filesystem::path cand = dir / tool_base;
        if (std::filesystem::exists(cand, ec) && !ec) {
            return cand.generic_u8string();
        }
    }
    return tool_base;
}

BuildCommand CompilerManager::FormulateCompileCommand(
    const CompilerInfo& compiler,
    const std::string& source_rel_path,
    const std::filesystem::path& object_path,
    const Project& project,
    const EffectiveFileSettings& file_eff
) {
    const EffectiveTargetSettings& eff = file_eff.base;
    BuildCommand cmd;
    std::filesystem::path src_p(source_rel_path);
    std::string ext = ToLower(src_p.extension().generic_u8string());
    bool is_c_source = (ext == ".c");

    // Select paired compiler executable
    std::string comp_prog = is_c_source ? project.GetCCompiler() : project.GetCompiler();
    if (comp_prog.empty()) {
        comp_prog = compiler.name;
    }
    cmd.program = comp_prog;

    const std::string comp_lower = ToLower(comp_prog);
    const bool is_msvc = (comp_lower == "cl" || comp_lower == "cl.exe");
    const bool is_clang = (comp_lower.find("clang") != std::string::npos);

    std::ostringstream ss;
    ss << comp_prog;

    const std::string config = project.GetConfiguration();
    ResolvedConfigParams cfg_params = ResolveConfigParams(project, config);

    std::string obj_str = object_path.generic_u8string();
    std::replace(obj_str.begin(), obj_str.end(), '\\', '/');

    std::string src_str = source_rel_path;
    std::replace(src_str.begin(), src_str.end(), '\\', '/');

    std::vector<std::string> cfg_flags = GetConfigCompileFlags(
        cfg_params, file_eff.optimize, file_eff.debugInfo, is_msvc, is_clang
    );

    if (is_msvc) {
        AppendArg(cmd, ss, "/c");
        AppendArg(cmd, ss, src_str);
        AppendArg(cmd, ss, "/Fo" + obj_str);
        AppendArg(cmd, ss, "/sourceDependencies");
        AppendArg(cmd, ss, obj_str + ".json");

        // Language standard
        if (is_c_source) {
            std::string c_std = eff.cStd;
            if (c_std == "c17") AppendArg(cmd, ss, "/std:c17");
            else AppendArg(cmd, ss, "/std:c11");
        } else {
            std::string cxx_std = eff.cxxStd;
            if (cxx_std == "c++23") AppendArg(cmd, ss, "/std:c++latest");
            else if (cxx_std == "c++20") AppendArg(cmd, ss, "/std:c++20");
            else if (cxx_std == "c++14") AppendArg(cmd, ss, "/std:c++14");
            else AppendArg(cmd, ss, "/std:c++17");
        }

        // Warnings
        std::string warn = eff.warnings;
        if (warn == "Off") AppendArg(cmd, ss, "/W0");
        else if (warn == "-Wall -Wextra") AppendArg(cmd, ss, "/W4");
        else AppendArg(cmd, ss, "/W3");

        // Configuration & override optimization/debug/runtime/LTO flags
        for (const auto& f : cfg_flags) {
            AppendArg(cmd, ss, f);
        }

        // Automatic -I. and -Iinclude (Spec §5.1.3)
        AppendArg(cmd, ss, "/I.");
        AppendArg(cmd, ss, "/Iinclude");

        // User & propagated include dirs
        for (const auto& inc : eff.includeDirs) {
            AppendArg(cmd, ss, "/I" + inc);
        }
        for (const auto& sys_inc : eff.systemIncludeDirs) {
            AppendArg(cmd, ss, "/I" + sys_inc);
        }

        // Defines
        for (const auto& def : eff.defines) {
            AppendArg(cmd, ss, "/D" + def);
        }

        // Compile flags
        for (const auto& flag : eff.compileFlags) {
            for (const auto& token : SplitFlags(flag)) {
                AppendArg(cmd, ss, token);
            }
        }
    } else {
        // gcc / clang
        AppendArg(cmd, ss, "-c");
        AppendArg(cmd, ss, src_str);
        AppendArg(cmd, ss, "-o");
        AppendArg(cmd, ss, obj_str);
        AppendArg(cmd, ss, "-MMD");
        AppendArg(cmd, ss, "-MF");
        AppendArg(cmd, ss, obj_str + ".d");

        // Language standard
        if (is_c_source) {
            std::string c_std = eff.cStd;
            if (c_std.empty()) c_std = "c11";
            AppendArg(cmd, ss, "-std=" + c_std);
        } else {
            std::string cxx_std = eff.cxxStd;
            if (cxx_std.empty()) cxx_std = "c++17";
            AppendArg(cmd, ss, "-std=" + cxx_std);
        }

        // Warnings
        std::string warn = eff.warnings;
        if (warn == "Off") {
            AppendArg(cmd, ss, "-w");
        } else if (warn == "-Wall -Wextra") {
            AppendArg(cmd, ss, "-Wall");
            AppendArg(cmd, ss, "-Wextra");
        } else {
            AppendArg(cmd, ss, "-Wall");
        }

        // Configuration & override optimization/debug/LTO flags
        for (const auto& f : cfg_flags) {
            AppendArg(cmd, ss, f);
        }

        // Automatic -I. and -Iinclude (Spec §5.1.3)
        AppendArg(cmd, ss, "-I.");
        AppendArg(cmd, ss, "-Iinclude");

        // User & propagated include dirs
        for (const auto& inc : eff.includeDirs) {
            AppendArg(cmd, ss, "-I" + inc);
        }
        for (const auto& sys_inc : eff.systemIncludeDirs) {
            AppendArg(cmd, ss, "-isystem");
            AppendArg(cmd, ss, sys_inc);
        }

        // Defines
        for (const auto& def : eff.defines) {
            AppendArg(cmd, ss, "-D" + def);
        }

        // Compile flags
        for (const auto& flag : eff.compileFlags) {
            for (const auto& token : SplitFlags(flag)) {
                AppendArg(cmd, ss, token);
            }
        }
    }

    cmd.display_command = ss.str();
    return cmd;
}

BuildCommand CompilerManager::FormulateCompileCommand(
    const CompilerInfo& compiler,
    const std::string& source_rel_path,
    const std::filesystem::path& object_path,
    const Project& project,
    const EffectiveTargetSettings& eff
) {
    EffectiveFileSettings file_eff;
    file_eff.base = eff;
    ResolvedConfigParams cfg = ResolveConfigParams(project, project.GetConfiguration());
    if (cfg.defineDebug && std::find(file_eff.base.defines.begin(), file_eff.base.defines.end(), "_DEBUG") == file_eff.base.defines.end()) {
        file_eff.base.defines.push_back("_DEBUG");
    }
    if (cfg.defineNdebug && std::find(file_eff.base.defines.begin(), file_eff.base.defines.end(), "NDEBUG") == file_eff.base.defines.end()) {
        file_eff.base.defines.push_back("NDEBUG");
    }
    return FormulateCompileCommand(compiler, source_rel_path, object_path, project, file_eff);
}

BuildCommand CompilerManager::FormulateCompileCommand(
    const CompilerInfo& compiler,
    const std::string& source_rel_path,
    const std::filesystem::path& object_path,
    const Project& project
) {
    const auto& targets = project.GetTargets();
    if (!targets.empty()) {
        const Target* primary = project.GetPrimaryTarget();
        if (primary) {
            std::unordered_map<std::string, const Target*> map;
            for (const auto& t : targets) map[t.name] = &t;
            std::string comp_low = ToLower(project.GetCompiler());
            bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
            EffectiveTargetSettings eff = ResolveEffectiveTargetSettings(project, *primary, map, project.GetConfiguration(), is_msvc);
            return FormulateCompileCommand(compiler, source_rel_path, object_path, project, eff);
        }
    }

    EffectiveTargetSettings eff;
    eff.includeDirs = project.GetIncludeDirs();
    eff.defines = project.GetDefines();
    eff.compileFlags = project.GetCompileFlags();
    eff.cxxStd = project.GetCxxStd();
    eff.cStd = project.GetCStd();
    eff.warnings = project.GetWarnings();
    ConfigOverrides ov = project.GetConfigOverrides(project.GetConfiguration());
    eff.defines.insert(eff.defines.end(), ov.defines.begin(), ov.defines.end());
    eff.compileFlags.insert(eff.compileFlags.end(), ov.compileFlags.begin(), ov.compileFlags.end());
    return FormulateCompileCommand(compiler, source_rel_path, object_path, project, eff);
}

BuildCommand CompilerManager::FormulateLinkCommand(
    const CompilerInfo& linker,
    const Target& target,
    const EffectiveTargetSettings& eff,
    const std::vector<std::filesystem::path>& object_files,
    const std::filesystem::path& output_binary_path,
    const Project& project,
    bool write_rsp
) {
    BuildCommand cmd;

    const std::string config = project.GetConfiguration();
    const bool is_debug = (config != "Release");

    // Driver selection: link with C++ driver if any C++ file exists, else C driver (§5.1.6)
    const auto& files_for_lang = target.expandedFiles.empty() ? target.files : target.expandedFiles;
    SourceLanguageType lang = DetermineLanguage(files_for_lang);
    std::string link_prog = (lang == SourceLanguageType::C) ? project.GetCCompiler() : project.GetCompiler();
    if (link_prog.empty()) {
        link_prog = linker.name;
    }

    const std::string link_lower = ToLower(link_prog);
    const bool is_msvc = (link_lower == "cl" || link_lower == "cl.exe");
    const bool is_clang = (link_lower.find("clang") != std::string::npos);
    ResolvedConfigParams cfg_params = ResolveConfigParams(project, config);
    std::vector<std::string> cfg_link_flags = GetConfigLinkFlags(cfg_params, target.kind, is_msvc, is_clang);

    std::string rsp_filename = target.name.empty()
        ? "objs.rsp"
        : ("objs_" + SanitizeTargetFileToken(target.name) + ".rsp");
    std::string rsp_rel = ".geniebuild/" + rsp_filename;

    if (write_rsp) {
        std::error_code ec;
        std::filesystem::path gb_dir = project.GetRootPath() / ".geniebuild";
        std::filesystem::create_directories(gb_dir, ec);
        std::filesystem::path rsp_path = gb_dir / rsp_filename;

        std::ofstream rsp_file(rsp_path);
        if (rsp_file.is_open()) {
            for (const auto& obj : object_files) {
                std::string obj_s = obj.generic_u8string();
                std::replace(obj_s.begin(), obj_s.end(), '\\', '/');
                rsp_file << "\"" << obj_s << "\"\n";
            }
        }
    }

    std::string bin_str = output_binary_path.generic_u8string();
    std::replace(bin_str.begin(), bin_str.end(), '\\', '/');

    // Static library archiving: ar rcs / lib.exe /OUT:
    if (target.kind == TargetKind::Static) {
        if (is_msvc) {
            std::string lib_prog = ResolveCompanionTool(linker, "lib");
            cmd.program = lib_prog;
            std::ostringstream ss;
            ss << lib_prog;
            AppendArg(cmd, ss, "/NOLOGO");
            AppendArg(cmd, ss, "/OUT:" + bin_str);
            AppendArg(cmd, ss, "@" + rsp_rel);
            cmd.display_command = ss.str();
        } else {
            std::string ar_prog = ResolveCompanionTool(linker, "ar");
            cmd.program = ar_prog;
            std::ostringstream ss;
            ss << ar_prog;
            AppendArg(cmd, ss, "rcs");
            AppendArg(cmd, ss, bin_str);
            AppendArg(cmd, ss, "@" + rsp_rel);
            cmd.display_command = ss.str();
        }
        return cmd;
    }

    cmd.program = link_prog;
    std::ostringstream ss;
    ss << link_prog;

    if (is_msvc) {
        AppendArg(cmd, ss, "@" + rsp_rel);
        AppendArg(cmd, ss, "/Fe:" + bin_str);
        AppendArg(cmd, ss, "/link");

        if (target.kind == TargetKind::Shared) {
            AppendArg(cmd, ss, "/DLL");
            std::filesystem::path implib_p = target.GetImportLibPath(project.GetAbsoluteOutputDir(), config, true);
            if (!implib_p.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(implib_p.parent_path(), ec);
                std::string imp_s = implib_p.generic_u8string();
                std::replace(imp_s.begin(), imp_s.end(), '\\', '/');
                AppendArg(cmd, ss, "/IMPLIB:" + imp_s);
            }
        }

        for (const auto& lf : cfg_link_flags) {
            AppendArg(cmd, ss, lf);
        }

        // Dependency target archives & import libraries
        for (const auto& dep_lib : eff.depLinkLibraries) {
            std::string dep_s = dep_lib.generic_u8string();
            std::replace(dep_s.begin(), dep_s.end(), '\\', '/');
            AppendArg(cmd, ss, dep_s);
        }

        // Library search paths
        for (const auto& lib_dir : eff.libDirs) {
            AppendArg(cmd, ss, "/LIBPATH:" + lib_dir);
        }

        // Libraries (e.g. SDL2.lib, opengl32.lib)
        for (const auto& lib_name : eff.libs) {
            std::string lib_arg = lib_name;
            if (lib_arg.size() < 4 || lib_arg.substr(lib_arg.size() - 4) != ".lib") {
                lib_arg += ".lib";
            }
            AppendArg(cmd, ss, lib_arg);
        }

        // Raw link flags
        for (const auto& flag : eff.linkFlags) {
            for (const auto& token : SplitFlags(flag)) {
                AppendArg(cmd, ss, token);
            }
        }
    } else {
        // gcc / clang
        if (target.kind == TargetKind::Shared) {
            AppendArg(cmd, ss, "-shared");
        }
        AppendArg(cmd, ss, "@" + rsp_rel);
        AppendArg(cmd, ss, "-o");
        AppendArg(cmd, ss, bin_str);

        if (target.kind == TargetKind::Shared) {
            std::filesystem::path implib_p = target.GetImportLibPath(project.GetAbsoluteOutputDir(), config, false);
            if (!implib_p.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(implib_p.parent_path(), ec);
                std::string imp_s = implib_p.generic_u8string();
                std::replace(imp_s.begin(), imp_s.end(), '\\', '/');
                AppendArg(cmd, ss, "-Wl,--out-implib," + imp_s);
            }
            if (target.exportAll) {
                AppendArg(cmd, ss, "-Wl,--export-all-symbols");
            }
        }

        for (const auto& lf : cfg_link_flags) {
            AppendArg(cmd, ss, lf);
        }

        // Dependency target archives & import libraries (dependents before dependencies)
        for (const auto& dep_lib : eff.depLinkLibraries) {
            std::string dep_s = dep_lib.generic_u8string();
            std::replace(dep_s.begin(), dep_s.end(), '\\', '/');
            AppendArg(cmd, ss, dep_s);
        }

        // Library search paths
        for (const auto& lib_dir : eff.libDirs) {
            AppendArg(cmd, ss, "-L" + lib_dir);
        }

        // Libraries (e.g. -lopengl32, -lSDL2)
        for (const auto& lib_name : eff.libs) {
            std::string lib_arg = lib_name;
            if (lib_arg.rfind("-l", 0) != 0 && lib_arg.find('/') == std::string::npos && lib_arg.find('\\') == std::string::npos) {
                lib_arg = "-l" + lib_arg;
            }
            AppendArg(cmd, ss, lib_arg);
        }

        // Raw link flags
        for (const auto& flag : eff.linkFlags) {
            for (const auto& token : SplitFlags(flag)) {
                AppendArg(cmd, ss, token);
            }
        }
    }

    cmd.display_command = ss.str();
    return cmd;
}

BuildCommand CompilerManager::FormulateLinkCommand(
    const CompilerInfo& linker,
    const std::vector<std::filesystem::path>& object_files,
    const std::filesystem::path& output_binary_path,
    const Project& project,
    bool write_rsp
) {
    const auto& targets = project.GetTargets();
    if (!targets.empty()) {
        const Target* primary = project.GetPrimaryTarget();
        if (primary) {
            std::unordered_map<std::string, const Target*> map;
            for (const auto& t : targets) map[t.name] = &t;
            std::string comp_low = ToLower(project.GetCompiler());
            bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
            EffectiveTargetSettings eff = ResolveEffectiveTargetSettings(project, *primary, map, project.GetConfiguration(), is_msvc);
            return FormulateLinkCommand(linker, *primary, eff, object_files, output_binary_path, project, write_rsp);
        }
    }

    Target dummy;
    dummy.name = "";
    dummy.kind = TargetKind::Executable;
    dummy.files = project.GetFiles();
    EffectiveTargetSettings eff;
    eff.libDirs = project.GetLibDirs();
    eff.libs = project.GetLibs();
    eff.linkFlags = project.GetLinkFlags();
    ConfigOverrides ov = project.GetConfigOverrides(project.GetConfiguration());
    eff.linkFlags.insert(eff.linkFlags.end(), ov.linkFlags.begin(), ov.linkFlags.end());
    return FormulateLinkCommand(linker, dummy, eff, object_files, output_binary_path, project, write_rsp);
}

void CompilerManager::DetectCompilers() {
    m_detected_compilers.clear();

    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return;
    }

    // Split PATH environment variable by delimiter (';' on Windows, ':' on POSIX)
#if defined(_WIN32)
    const char delimiter = ';';
#else
    const char delimiter = ':';
#endif

    std::vector<std::filesystem::path> search_dirs;
    std::string path_str(path_env);
    std::stringstream ss(path_str);
    std::string dir_token;

    while (std::getline(ss, dir_token, delimiter)) {
        if (!dir_token.empty()) {
            std::error_code ec;
            std::filesystem::path dir_path(dir_token);
            if (std::filesystem::exists(dir_path, ec) && std::filesystem::is_directory(dir_path, ec)) {
                search_dirs.push_back(dir_path);
            }
        }
    }

    // Spec 4.5: Standard compilers to search for on PATH
    struct CandidateSpec {
        const char* name;
        CompilerLanguage lang;
    };

    const CandidateSpec candidates[] = {
        { "g++",     CompilerLanguage::Cxx },
        { "clang++", CompilerLanguage::Cxx },
        { "cl",      CompilerLanguage::Both }, // MSVC cl compiles both C and C++
        { "gcc",     CompilerLanguage::C },
        { "clang",   CompilerLanguage::C }
    };

    // Helper to find if candidate exists in search directories
    for (const auto& candidate : candidates) {
        bool found = false;

        for (const auto& dir : search_dirs) {
            std::error_code ec;

#if defined(_WIN32)
            // On Windows, executables commonly end with .exe
            std::filesystem::path exe_candidate = dir / (std::string(candidate.name) + ".exe");
            if (std::filesystem::exists(exe_candidate, ec) && std::filesystem::is_regular_file(exe_candidate, ec)) {
                CompilerInfo info;
                info.name = candidate.name;
                info.executable_path = exe_candidate;
                info.language = candidate.lang;
                m_detected_compilers.push_back(info);
                found = true;
                break;
            }
#endif
            std::filesystem::path direct_candidate = dir / candidate.name;
            if (std::filesystem::exists(direct_candidate, ec) && std::filesystem::is_regular_file(direct_candidate, ec)) {
                CompilerInfo info;
                info.name = candidate.name;
                info.executable_path = direct_candidate;
                info.language = candidate.lang;
                m_detected_compilers.push_back(info);
                found = true;
                break;
            }
        }

        (void)found;
    }
}

const CompilerInfo* CompilerManager::FindCompiler(const std::string& name) const {
    const std::string lower = ToLower(name);
    for (const auto& comp : m_detected_compilers) {
        if (ToLower(comp.name) == lower) {
            return &comp;
        }
    }
    return nullptr;
}

SourceLanguageType CompilerManager::DetermineLanguage(const std::vector<std::string>& files) {
    if (files.empty()) {
        return SourceLanguageType::Empty;
    }

    bool has_c = false;
    bool has_cpp = false;

    for (const auto& file : files) {
        std::filesystem::path p(file);
        std::string ext = ToLower(p.extension().generic_u8string());

        if (ext == ".c") {
            has_c = true;
        } else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
            has_cpp = true;
        }
    }

    if (has_c && has_cpp) {
        return SourceLanguageType::Mixed;
    }
    if (has_cpp) {
        return SourceLanguageType::Cxx;
    }
    if (has_c) {
        return SourceLanguageType::C;
    }
    return SourceLanguageType::Empty;
}

BuildCommand CompilerManager::FormulateCommand(
    const CompilerInfo& compiler,
    const std::vector<std::string>& files,
    const std::filesystem::path& project_root,
    const std::filesystem::path& output_binary_path
) {
    BuildCommand cmd;
    cmd.program = compiler.name;

    const std::string comp_name = ToLower(compiler.name);
    const bool is_msvc = (comp_name == "cl" || comp_name == "cl.exe");
    const SourceLanguageType lang = DetermineLanguage(files);

    // Part 2: Write response file .geniebuild/files.rsp
    std::error_code ec;
    std::filesystem::path gb_dir = project_root / ".geniebuild";
    std::filesystem::create_directories(gb_dir, ec);
    std::filesystem::path rsp_path = gb_dir / "files.rsp";

    {
        std::ofstream rsp_file(rsp_path);
        if (rsp_file.is_open()) {
            for (const auto& f : files) {
                std::string fwd = f;
                std::replace(fwd.begin(), fwd.end(), '\\', '/');
                rsp_file << "\"" << fwd << "\"\n";
            }
        }
    }

    std::ostringstream ss;
    ss << compiler.name;

    if (is_msvc) {
        cmd.arguments.push_back("@.geniebuild/files.rsp");
        ss << " @.geniebuild/files.rsp";

        if (lang == SourceLanguageType::Cxx) {
            cmd.arguments.push_back("/std:c++17");
            ss << " /std:c++17";
        }

        std::string fe_arg = "/Fe:" + output_binary_path.generic_u8string();
        cmd.arguments.push_back(fe_arg);
        ss << " " << fe_arg;
    } else {
        cmd.arguments.push_back("@.geniebuild/files.rsp");
        ss << " @.geniebuild/files.rsp";

        if (lang == SourceLanguageType::Cxx) {
            cmd.arguments.push_back("-std=c++17");
            ss << " -std=c++17";
        }

        cmd.arguments.push_back("-o");
        cmd.arguments.push_back(output_binary_path.generic_u8string());
        ss << " -o " << output_binary_path.generic_u8string();
    }

    cmd.display_command = ss.str();
    return cmd;
}

} // namespace core
