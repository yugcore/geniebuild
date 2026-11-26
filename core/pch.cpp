#include "pch.h"
#include "project.h"
#include "target.h"
#include "settings_resolver.h"
#include "source_rules.h"
#include "tool_registry.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace core {

namespace {

std::string ToLowerPch(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void HashPchStr(uint64_t& h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= 0xFF;
    h *= 1099511628211ULL;
}

bool IsPchRelevantFlag(const std::string& flag) {
    if (flag.rfind("-m", 0) == 0 || flag.rfind("/arch:", 0) == 0 ||
        flag.rfind("-f", 0) == 0 || flag.rfind("-std=", 0) == 0 ||
        flag.rfind("/std:", 0) == 0 || flag.rfind("-D", 0) == 0 ||
        flag.rfind("/D", 0) == 0 || flag.rfind("-U", 0) == 0 ||
        flag.rfind("/U", 0) == 0) {
        return true;
    }
    return false;
}

std::vector<std::string> ExtractRelevantFlags(const EffectiveFileSettings& eff) {
    std::vector<std::string> out;
    for (const auto& f : eff.base.compileFlags) {
        for (const auto& tok : CompilerManager::SplitFlags(f)) {
            if (IsPchRelevantFlag(tok)) out.push_back(tok);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void AppendArgPch(BuildCommand& cmd, std::ostringstream& ss, const std::string& arg) {
    cmd.arguments.push_back(arg);
    if (arg.find(' ') != std::string::npos && !(arg.front() == '"' && arg.back() == '"')) {
        ss << " \"" << arg << "\"";
    } else {
        ss << " " << arg;
    }
}

PchArtifactPlan BuildSinglePchPlan(
    const Project& project,
    const Target& target,
    const EffectiveFileSettings& base_eff,
    const std::string& header_rel,
    const std::string& msvc_source_rel,
    const CompilerInfo& comp,
    const std::string& config,
    bool is_c,
    bool is_msvc,
    bool is_clang,
    bool ccache_active,
    bool write_generated_files
) {
    PchArtifactPlan plan;
    if (header_rel.empty()) return plan;

    plan.enabled = true;
    plan.isC = is_c;
    plan.headerRel = header_rel;
    plan.pchDir = project.GetRootPath() / ".geniebuild" / "pch" / SanitizeConfigDirName(config) / target.name;
    plan.baseSignature = PchManager::ComputePchSignature(base_eff, is_c, is_msvc);

    std::error_code ec;
    if (write_generated_files) {
        std::filesystem::create_directories(plan.pchDir, ec);
    }

    std::filesystem::path header_abs = project.ResolvePath(header_rel);
    std::string header_abs_s = header_abs.generic_u8string();
    std::string stem_name = is_c ? "pch_c" : "pch";

    // Resolve compiler command flags (same logic as CompilerManager::FormulateCompileCommand)
    std::string comp_prog = is_c ? project.GetCCompiler() : project.GetCompiler();
    if (comp_prog.empty()) comp_prog = comp.name;
    plan.compileCmd.program = comp_prog;

    ResolvedConfigParams cfg_params = ResolveConfigParams(project, config);
    std::vector<std::string> cfg_flags = GetConfigCompileFlags(
        cfg_params, base_eff.optimize, base_eff.debugInfo, is_msvc, is_clang
    );

    std::ostringstream ss;
    ss << comp_prog;

    if (is_msvc) {
        std::filesystem::path cpp_file;
        if (!is_c && !msvc_source_rel.empty()) {
            cpp_file = project.ResolvePath(msvc_source_rel);
        } else {
            cpp_file = plan.pchDir / (stem_name + (is_c ? ".c" : ".cpp"));
            if (write_generated_files) {
                std::string body = "// GenieBuild generated PCH source\n#include \"" + header_abs_s + "\"\n";
                PchManager::WriteFileIfDifferent(cpp_file, body);
            }
        }
        plan.forwardingOrSourceFile = cpp_file;
        plan.pchOutputFile = plan.pchDir / (stem_name + ".pch");
        plan.msvcObjectFile = plan.pchDir / (stem_name + ".obj");
        plan.depfilePath = std::filesystem::path(plan.msvcObjectFile.generic_u8string() + ".json");

        std::string hdr_name = header_abs.filename().generic_u8string();
        std::string hdr_dir = header_abs.parent_path().generic_u8string();

        AppendArgPch(plan.compileCmd, ss, "/c");
        AppendArgPch(plan.compileCmd, ss, cpp_file.generic_u8string());
        AppendArgPch(plan.compileCmd, ss, "/Yc" + hdr_name);
        AppendArgPch(plan.compileCmd, ss, "/Fp" + plan.pchOutputFile.generic_u8string());
        AppendArgPch(plan.compileCmd, ss, "/FI" + hdr_name);
        AppendArgPch(plan.compileCmd, ss, "/Fo" + plan.msvcObjectFile.generic_u8string());
        AppendArgPch(plan.compileCmd, ss, "/sourceDependencies");
        AppendArgPch(plan.compileCmd, ss, plan.depfilePath.generic_u8string());

        if (is_c) {
            std::string c_std = base_eff.base.cStd;
            if (c_std == "c17") AppendArgPch(plan.compileCmd, ss, "/std:c17");
            else AppendArgPch(plan.compileCmd, ss, "/std:c11");
        } else {
            std::string cxx_std = base_eff.base.cxxStd;
            if (cxx_std == "c++23") AppendArgPch(plan.compileCmd, ss, "/std:c++latest");
            else if (cxx_std == "c++20") AppendArgPch(plan.compileCmd, ss, "/std:c++20");
            else if (cxx_std == "c++14") AppendArgPch(plan.compileCmd, ss, "/std:c++14");
            else AppendArgPch(plan.compileCmd, ss, "/std:c++17");
        }

        std::string warn = base_eff.base.warnings;
        if (warn == "Off") AppendArgPch(plan.compileCmd, ss, "/W0");
        else if (warn == "-Wall -Wextra") AppendArgPch(plan.compileCmd, ss, "/W4");
        else AppendArgPch(plan.compileCmd, ss, "/W3");

        for (const auto& f : cfg_flags) AppendArgPch(plan.compileCmd, ss, f);
        AppendArgPch(plan.compileCmd, ss, "/I.");
        AppendArgPch(plan.compileCmd, ss, "/Iinclude");
        if (!hdr_dir.empty()) AppendArgPch(plan.compileCmd, ss, "/I" + hdr_dir);
        for (const auto& inc : base_eff.base.includeDirs) AppendArgPch(plan.compileCmd, ss, "/I" + inc);
        for (const auto& sinc : base_eff.base.systemIncludeDirs) AppendArgPch(plan.compileCmd, ss, "/I" + sinc);
        for (const auto& def : base_eff.base.defines) AppendArgPch(plan.compileCmd, ss, "/D" + def);
        for (const auto& fl : base_eff.base.compileFlags) {
            for (const auto& tok : CompilerManager::SplitFlags(fl)) AppendArgPch(plan.compileCmd, ss, tok);
        }
    } else if (is_clang) {
        plan.forwardingOrSourceFile = header_abs;
        plan.pchOutputFile = plan.pchDir / (stem_name + ".pch");
        plan.depfilePath = std::filesystem::path(plan.pchOutputFile.generic_u8string() + ".d");

        AppendArgPch(plan.compileCmd, ss, "-x");
        AppendArgPch(plan.compileCmd, ss, is_c ? "c-header" : "c++-header");
        AppendArgPch(plan.compileCmd, ss, header_abs_s);
        AppendArgPch(plan.compileCmd, ss, "-Xclang");
        AppendArgPch(plan.compileCmd, ss, "-emit-pch");
        AppendArgPch(plan.compileCmd, ss, "-o");
        AppendArgPch(plan.compileCmd, ss, plan.pchOutputFile.generic_u8string());
        AppendArgPch(plan.compileCmd, ss, "-MMD");
        AppendArgPch(plan.compileCmd, ss, "-MF");
        AppendArgPch(plan.compileCmd, ss, plan.depfilePath.generic_u8string());

        if (is_c) {
            std::string c_std = base_eff.base.cStd.empty() ? "c11" : base_eff.base.cStd;
            AppendArgPch(plan.compileCmd, ss, "-std=" + c_std);
        } else {
            std::string cxx_std = base_eff.base.cxxStd.empty() ? "c++17" : base_eff.base.cxxStd;
            AppendArgPch(plan.compileCmd, ss, "-std=" + cxx_std);
        }

        std::string warn = base_eff.base.warnings;
        if (warn == "Off") AppendArgPch(plan.compileCmd, ss, "-w");
        else if (warn == "-Wall -Wextra") {
            AppendArgPch(plan.compileCmd, ss, "-Wall");
            AppendArgPch(plan.compileCmd, ss, "-Wextra");
        } else AppendArgPch(plan.compileCmd, ss, "-Wall");

        for (const auto& f : cfg_flags) AppendArgPch(plan.compileCmd, ss, f);
        AppendArgPch(plan.compileCmd, ss, "-I.");
        AppendArgPch(plan.compileCmd, ss, "-Iinclude");
        for (const auto& inc : base_eff.base.includeDirs) AppendArgPch(plan.compileCmd, ss, "-I" + inc);
        for (const auto& sinc : base_eff.base.systemIncludeDirs) {
            AppendArgPch(plan.compileCmd, ss, "-isystem");
            AppendArgPch(plan.compileCmd, ss, sinc);
        }
        for (const auto& def : base_eff.base.defines) AppendArgPch(plan.compileCmd, ss, "-D" + def);
        for (const auto& fl : base_eff.base.compileFlags) {
            for (const auto& tok : CompilerManager::SplitFlags(fl)) AppendArgPch(plan.compileCmd, ss, tok);
        }
    } else {
        // GCC: forwarding header + .gch
        std::filesystem::path fwd_h = plan.pchDir / (stem_name + ".h");
        if (write_generated_files) {
            std::string body = "// GenieBuild forwarding PCH header\n#include \"" + header_abs_s + "\"\n";
            PchManager::WriteFileIfDifferent(fwd_h, body);
        }
        plan.forwardingOrSourceFile = fwd_h;
        plan.pchOutputFile = std::filesystem::path(fwd_h.generic_u8string() + ".gch");
        plan.depfilePath = std::filesystem::path(plan.pchOutputFile.generic_u8string() + ".d");

        AppendArgPch(plan.compileCmd, ss, "-x");
        AppendArgPch(plan.compileCmd, ss, is_c ? "c-header" : "c++-header");
        AppendArgPch(plan.compileCmd, ss, fwd_h.generic_u8string());
        AppendArgPch(plan.compileCmd, ss, "-o");
        AppendArgPch(plan.compileCmd, ss, plan.pchOutputFile.generic_u8string());
        AppendArgPch(plan.compileCmd, ss, "-MMD");
        AppendArgPch(plan.compileCmd, ss, "-MF");
        AppendArgPch(plan.compileCmd, ss, plan.depfilePath.generic_u8string());
        if (ccache_active) {
            AppendArgPch(plan.compileCmd, ss, "-fpch-preprocess");
        }

        if (is_c) {
            std::string c_std = base_eff.base.cStd.empty() ? "c11" : base_eff.base.cStd;
            AppendArgPch(plan.compileCmd, ss, "-std=" + c_std);
        } else {
            std::string cxx_std = base_eff.base.cxxStd.empty() ? "c++17" : base_eff.base.cxxStd;
            AppendArgPch(plan.compileCmd, ss, "-std=" + cxx_std);
        }

        std::string warn = base_eff.base.warnings;
        if (warn == "Off") AppendArgPch(plan.compileCmd, ss, "-w");
        else if (warn == "-Wall -Wextra") {
            AppendArgPch(plan.compileCmd, ss, "-Wall");
            AppendArgPch(plan.compileCmd, ss, "-Wextra");
        } else AppendArgPch(plan.compileCmd, ss, "-Wall");

        for (const auto& f : cfg_flags) AppendArgPch(plan.compileCmd, ss, f);
        AppendArgPch(plan.compileCmd, ss, "-I.");
        AppendArgPch(plan.compileCmd, ss, "-Iinclude");
        std::string hdr_dir = header_abs.parent_path().generic_u8string();
        if (!hdr_dir.empty()) AppendArgPch(plan.compileCmd, ss, "-I" + hdr_dir);
        for (const auto& inc : base_eff.base.includeDirs) AppendArgPch(plan.compileCmd, ss, "-I" + inc);
        for (const auto& sinc : base_eff.base.systemIncludeDirs) {
            AppendArgPch(plan.compileCmd, ss, "-isystem");
            AppendArgPch(plan.compileCmd, ss, sinc);
        }
        for (const auto& def : base_eff.base.defines) AppendArgPch(plan.compileCmd, ss, "-D" + def);
        for (const auto& fl : base_eff.base.compileFlags) {
            for (const auto& tok : CompilerManager::SplitFlags(fl)) AppendArgPch(plan.compileCmd, ss, tok);
        }
    }

    plan.compileCmd.display_command = ss.str();
    plan.cmdHash = CompilerManager::ComputeCommandHash(plan.compileCmd);
    return plan;
}

} // namespace

bool PchManager::WriteFileIfDifferent(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing.is_open()) {
            std::string cur((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (cur == content) return false;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

std::string PchManager::ComputePchSignature(
    const EffectiveFileSettings& eff,
    bool is_c_source,
    bool is_msvc,
    std::string* out_diag_summary
) {
    uint64_t h = 14695981039346656037ULL;
    std::vector<std::string> sorted_defs = eff.base.defines;
    std::sort(sorted_defs.begin(), sorted_defs.end());
    for (const auto& d : sorted_defs) HashPchStr(h, "D:" + d);

    HashPchStr(h, is_c_source ? ("cStd:" + eff.base.cStd) : ("cxxStd:" + eff.base.cxxStd));
    HashPchStr(h, "opt:" + eff.optimize);
    if (is_msvc) {
        HashPchStr(h, "rt:" + eff.runtime);
    }
    for (const auto& rf : ExtractRelevantFlags(eff)) {
        HashPchStr(h, "F:" + rf);
    }

    if (out_diag_summary) {
        *out_diag_summary = (is_c_source ? eff.base.cStd : eff.base.cxxStd) + " " + eff.optimize;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string PchManager::DescribeSignatureDiff(
    const EffectiveFileSettings& base,
    const EffectiveFileSettings& file,
    bool is_c_source,
    bool is_msvc
) {
    std::vector<std::string> bd = base.base.defines;
    std::vector<std::string> fd = file.base.defines;
    std::sort(bd.begin(), bd.end());
    std::sort(fd.begin(), fd.end());
    if (bd != fd) return "defines differ";

    if (is_c_source ? (base.base.cStd != file.base.cStd) : (base.base.cxxStd != file.base.cxxStd)) {
        return "standard differs";
    }
    if (base.optimize != file.optimize) return "optimize differs";
    if (is_msvc && base.runtime != file.runtime) return "MSVC runtime differs";
    if (ExtractRelevantFlags(base) != ExtractRelevantFlags(file)) return "PCH-relevant flags differ";
    return "settings differ";
}

TargetPchAnalysis PchManager::AnalyzeAndPlanTargetPch(
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
    bool ccache_active,
    bool write_generated_files
) {
    TargetPchAnalysis analysis;
    if (target.pch.IsEmpty() || (target.pch.header.empty() && target.pch.cHeader.empty())) {
        return analysis;
    }

    // Build baseline target EffectiveFileSettings (with no per-file overrides)
    EffectiveFileSettings base_eff;
    base_eff.base = target_eff;
    ResolvedConfigParams cfg_p = ResolveConfigParams(project, config);
    base_eff.configName = config;
    base_eff.basePreset = BaseConfigPresetToString(cfg_p.base);
    base_eff.optimize = cfg_p.optimize;
    base_eff.debugInfo = cfg_p.debugInfo;
    base_eff.runtime = cfg_p.runtime;
    base_eff.lto = cfg_p.lto;
    if (cfg_p.defineDebug && std::find(base_eff.base.defines.begin(), base_eff.base.defines.end(), "_DEBUG") == base_eff.base.defines.end()) {
        base_eff.base.defines.push_back("_DEBUG");
    }
    if (cfg_p.defineNdebug && std::find(base_eff.base.defines.begin(), base_eff.base.defines.end(), "NDEBUG") == base_eff.base.defines.end()) {
        base_eff.base.defines.push_back("NDEBUG");
    }

    if (!target.pch.header.empty()) {
        analysis.cxxPch = BuildSinglePchPlan(
            project, target, base_eff, target.pch.header, target.pch.source,
            cxx_comp, config, false, is_msvc, is_clang, ccache_active, write_generated_files
        );
    }
    if (!target.pch.cHeader.empty()) {
        analysis.cPch = BuildSinglePchPlan(
            project, target, base_eff, target.pch.cHeader, "",
            c_comp, config, true, is_msvc, is_clang, ccache_active, write_generated_files
        );
    }

    std::vector<CompiledGlob> pch_excludes;
    for (const auto& pat : target.pch.exclude) {
        CompiledGlob cg = CompiledGlob::Compile(pat);
        if (cg.IsValid()) pch_excludes.push_back(std::move(cg));
    }

    for (const auto& f : source_files) {
        PchFileUsage usage;
        usage.file = f;
        SourceFileKind kind = ToolRegistry::ClassifySourceFile(f);
        if (kind != SourceFileKind::C && kind != SourceFileKind::Cxx) {
            usage.usesPch = false;
            usage.excludeReason = "non-C/C++ source";
            analysis.fileUsages.push_back(std::move(usage));
            continue;
        }

        usage.isC = (kind == SourceFileKind::C);
        const PchArtifactPlan& active_plan = usage.isC ? analysis.cPch : analysis.cxxPch;
        if (!active_plan.enabled) {
            usage.usesPch = false;
            usage.excludeReason = usage.isC ? "C file (no C PCH configured)" : "C++ file (no C++ PCH configured)";
            analysis.fileUsages.push_back(std::move(usage));
            continue;
        }

        // Skip explicit MSVC pch.source if it matches
        if (is_msvc && !target.pch.source.empty()) {
            std::string n1 = f;
            std::string n2 = target.pch.source;
            std::replace(n1.begin(), n1.end(), '\\', '/');
            std::replace(n2.begin(), n2.end(), '\\', '/');
            if (ToLowerPch(n1) == ToLowerPch(n2)) {
                usage.usesPch = false;
                usage.excludeReason = "PCH source file";
                analysis.fileUsages.push_back(std::move(usage));
                continue;
            }
        }

        bool excluded_glob = false;
        for (const auto& cg : pch_excludes) {
            if (cg.Matches(f)) {
                excluded_glob = true;
                break;
            }
        }
        if (excluded_glob) {
            usage.usesPch = false;
            usage.excludeReason = "excluded by pch.exclude";
            analysis.fileUsages.push_back(std::move(usage));
            continue;
        }

        auto fit = file_settings.find(f);
        if (fit != file_settings.end() && fit->second) {
            const EffectiveFileSettings& fe = *fit->second;
            if (fe.excludeFromPch) {
                usage.usesPch = false;
                usage.excludeReason = "excludeFromPch override";
                analysis.fileUsages.push_back(std::move(usage));
                continue;
            }
            std::string fsig = ComputePchSignature(fe, usage.isC, is_msvc);
            if (fsig != active_plan.baseSignature) {
                usage.usesPch = false;
                usage.excludeReason = DescribeSignatureDiff(base_eff, fe, usage.isC, is_msvc);
                analysis.fileUsages.push_back(std::move(usage));
                continue;
            }
        }

        usage.usesPch = true;
        analysis.includedCount++;
        analysis.fileUsages.push_back(std::move(usage));
    }

    analysis.totalCount = source_files.size();
    return analysis;
}

void PchManager::ApplyPchToCompileCommand(
    BuildCommand& cmd,
    const PchArtifactPlan& pch_plan,
    bool is_msvc,
    bool is_clang,
    bool ccache_active
) {
    if (!pch_plan.enabled) return;

    std::vector<std::string> extra;
    if (is_msvc) {
        std::string hdr_name = std::filesystem::u8path(pch_plan.headerRel).filename().generic_u8string();
        std::string hdr_dir = std::filesystem::u8path(pch_plan.headerRel).parent_path().generic_u8string();
        extra.push_back("/Yu" + hdr_name);
        extra.push_back("/Fp" + pch_plan.pchOutputFile.generic_u8string());
        extra.push_back("/FI" + hdr_name);
        if (!hdr_dir.empty()) {
            extra.push_back("/I" + hdr_dir);
        }
    } else if (is_clang) {
        extra.push_back("-include-pch");
        extra.push_back(pch_plan.pchOutputFile.generic_u8string());
        if (ccache_active) {
            extra.push_back("-fpch-preprocess");
        }
    } else {
        extra.push_back("-include");
        extra.push_back(pch_plan.forwardingOrSourceFile.generic_u8string());
        if (ccache_active) {
            extra.push_back("-fpch-preprocess");
        }
    }

    std::ostringstream ss;
    ss << cmd.display_command;
    for (const auto& a : extra) {
        cmd.arguments.push_back(a);
        if (a.find(' ') != std::string::npos) ss << " \"" << a << "\"";
        else ss << " " << a;
    }
    cmd.display_command = ss.str();
}

} // namespace core
