#include "settings_resolver.h"
#include "project.h"
#include "compiler.h"
#include "condition.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace core {

static std::string ToLowerRes(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static void AppendUniqueRes(std::vector<std::string>& dst, const std::vector<std::string>& src) {
    for (const auto& item : src) {
        if (item.empty()) continue;
        if (std::find(dst.begin(), dst.end(), item) == dst.end()) {
            dst.push_back(item);
        }
    }
}

std::string BaseConfigPresetToString(BaseConfigPreset p) {
    switch (p) {
        case BaseConfigPreset::Debug:          return "debug";
        case BaseConfigPreset::Release:        return "release";
        case BaseConfigPreset::RelWithDebInfo: return "relWithDebInfo";
        case BaseConfigPreset::MinSize:        return "minSize";
    }
    return "debug";
}

BaseConfigPreset BaseConfigPresetFromString(const std::string& str, const std::string& fallback_cfg_name) {
    std::string low = ToLowerRes(str);
    if (low == "debug") return BaseConfigPreset::Debug;
    if (low == "release") return BaseConfigPreset::Release;
    if (low == "relwithdebinfo") return BaseConfigPreset::RelWithDebInfo;
    if (low == "minsize" || low == "minsizerel") return BaseConfigPreset::MinSize;

    std::string cfg_low = ToLowerRes(fallback_cfg_name);
    if (cfg_low == "release" || cfg_low == "shipping" || cfg_low == "profile") return BaseConfigPreset::Release;
    if (cfg_low == "relwithdebinfo" || cfg_low == "development") return BaseConfigPreset::RelWithDebInfo;
    if (cfg_low == "minsize" || cfg_low == "minsizerel") return BaseConfigPreset::MinSize;
    return BaseConfigPreset::Debug;
}

std::string SanitizeConfigDirName(const std::string& config_name) {
    if (config_name.empty()) return "Default";
    std::string out;
    out.reserve(config_name.size());
    for (unsigned char c : config_name) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "Default" : out;
}

ResolvedConfigParams ResolveConfigParams(const Project& project, const std::string& config_name) {
    ResolvedConfigParams res;
    res.name = config_name;
    ConfigOverrides ov = project.GetConfigOverrides(config_name);

    res.base = BaseConfigPresetFromString(ov.base, config_name);

    // Base defaults from table (§V2-4)
    switch (res.base) {
        case BaseConfigPreset::Debug:
            res.optimize = "O0";
            res.debugInfo = "full";
            res.defineNdebug = false;
            res.defineDebug = true;
            break;
        case BaseConfigPreset::Release:
            res.optimize = "O2";
            res.debugInfo = "none";
            res.defineNdebug = true;
            res.defineDebug = false;
            break;
        case BaseConfigPreset::RelWithDebInfo:
            res.optimize = "O2";
            res.debugInfo = "full";
            res.defineNdebug = true;
            res.defineDebug = false;
            break;
        case BaseConfigPreset::MinSize:
            res.optimize = "Os";
            res.debugInfo = "none";
            res.defineNdebug = true;
            res.defineDebug = false;
            break;
    }
    res.runtime = "dynamic";
    res.lto = "off";

    if (!ov.optimize.empty()) res.optimize = ov.optimize;
    if (!ov.debugInfo.empty()) res.debugInfo = ov.debugInfo;
    if (!ov.runtime.empty()) res.runtime = ov.runtime;
    if (!ov.lto.empty()) res.lto = ov.lto;

    return res;
}

std::vector<std::string> GetConfigCompileFlags(
    const ResolvedConfigParams& cfg,
    const std::string& optimize_override,
    const std::string& debug_info_override,
    bool is_msvc,
    bool is_clang
) {
    std::vector<std::string> out;
    std::string opt = optimize_override.empty() ? cfg.optimize : optimize_override;
    std::string dbg = debug_info_override.empty() ? cfg.debugInfo : debug_info_override;
    std::string opt_low = ToLowerRes(opt);
    std::string dbg_low = ToLowerRes(dbg);
    std::string rt_low = ToLowerRes(cfg.runtime);
    std::string lto_low = ToLowerRes(cfg.lto);

    if (is_msvc) {
        if (opt_low == "o0") out.push_back("/Od");
        else if (opt_low == "o1") out.push_back("/O1");
        else if (opt_low == "o3") { out.push_back("/O2"); out.push_back("/Ob3"); }
        else if (opt_low == "os") out.push_back("/O1");
        else out.push_back("/O2");

        if (dbg_low == "line" || dbg_low == "full") {
            out.push_back("/Z7");
        }

        const bool is_dbg_crt = (cfg.base == BaseConfigPreset::Debug);
        if (rt_low == "static") {
            out.push_back(is_dbg_crt ? "/MTd" : "/MT");
        } else {
            out.push_back(is_dbg_crt ? "/MDd" : "/MD");
        }

        if (lto_low == "thin" || lto_low == "full") {
            out.push_back("/GL");
        }
    } else {
        if (opt_low == "o0") out.push_back("-O0");
        else if (opt_low == "o1") out.push_back("-O1");
        else if (opt_low == "o3") out.push_back("-O3");
        else if (opt_low == "os") out.push_back("-Os");
        else out.push_back("-O2");

        if (dbg_low == "line") out.push_back("-g1");
        else if (dbg_low == "full") out.push_back("-g");

        if (lto_low == "thin") {
            out.push_back(is_clang ? "-flto=thin" : "-flto");
        } else if (lto_low == "full") {
            out.push_back("-flto");
        }
    }
    return out;
}

std::vector<std::string> GetConfigLinkFlags(
    const ResolvedConfigParams& cfg,
    TargetKind kind,
    bool is_msvc,
    bool is_clang
) {
    std::vector<std::string> out;
    std::string dbg_low = ToLowerRes(cfg.debugInfo);
    std::string rt_low = ToLowerRes(cfg.runtime);
    std::string lto_low = ToLowerRes(cfg.lto);

    if (is_msvc) {
        if (dbg_low == "line" || dbg_low == "full") {
            out.push_back("/DEBUG");
        }
        if (lto_low == "thin" || lto_low == "full") {
            out.push_back("/LTCG");
        }
    } else {
        if (dbg_low == "line" || dbg_low == "full") {
            out.push_back("-g");
        }
        if (lto_low == "thin") {
            out.push_back(is_clang ? "-flto=thin" : "-flto");
        } else if (lto_low == "full") {
            out.push_back("-flto");
        }
        if (rt_low == "static" && kind == TargetKind::Executable) {
            out.push_back("-static");
        }
    }
    return out;
}

static bool IsSameOriginFlagCategory(const std::string& a, const std::string& b) {
    // Detect mutually overriding single-value flag categories (-O*, /O*, -w / -W*, -std=, /std:)
    auto cat = [](const std::string& f) -> int {
        if (f.rfind("-O", 0) == 0 || f.rfind("/O", 0) == 0) return 1;
        if (f == "-w" || f.rfind("-Wall", 0) == 0 || f.rfind("-Wextra", 0) == 0 || f.rfind("/W", 0) == 0) return 2;
        if (f.rfind("-std=", 0) == 0 || f.rfind("/std:", 0) == 0) return 3;
        return 0;
    };
    int ca = cat(a);
    return ca != 0 && ca == cat(b);
}

static void RecordOriginFlag(
    std::vector<FlagWithOrigin>& list,
    const std::string& flag,
    const std::string& origin,
    const std::string& reason = ""
) {
    for (auto& existing : list) {
        if (!existing.overridden && IsSameOriginFlagCategory(existing.flag, flag)) {
            existing.overridden = true;
            existing.reason = "overridden by " + origin + " (" + flag + ")";
        }
    }
    list.push_back({ flag, origin, reason, false });
}

static void ResolveFromMatchedOverrides(
    const Project& project,
    const Target& target,
    const EffectiveTargetSettings& target_eff,
    const std::vector<const FlagOverride*>& matched_overrides,
    const std::string& config_name,
    const std::string& compiler_name,
    EffectiveFileSettings& out
) {
    out.base = target_eff;
    out.configName = config_name;

    std::string comp_low = ToLowerRes(compiler_name);
    const bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
    const bool is_clang = (comp_low.find("clang") != std::string::npos);

    ResolvedConfigParams cfg = ResolveConfigParams(project, config_name);
    if (!target.pub.runtime.empty()) cfg.runtime = target.pub.runtime;
    if (!target.priv.runtime.empty()) cfg.runtime = target.priv.runtime;
    if (auto tcfg_rt = target.configs.find(config_name); tcfg_rt != target.configs.end() && !tcfg_rt->second.runtime.empty()) {
        cfg.runtime = tcfg_rt->second.runtime;
    }
    out.basePreset = BaseConfigPresetToString(cfg.base);
    out.optimize = cfg.optimize;
    out.debugInfo = cfg.debugInfo;
    out.runtime = cfg.runtime;
    out.lto = cfg.lto;
    out.defineNdebug = cfg.defineNdebug;
    out.defineDebug = cfg.defineDebug;

    // Ensure base preset _DEBUG / NDEBUG define is present unless explicitly overridden
    if (cfg.defineDebug) {
        AppendUniqueRes(out.base.defines, { "_DEBUG" });
    }
    if (cfg.defineNdebug) {
        AppendUniqueRes(out.base.defines, { "NDEBUG" });
    }

    // Build origin list in exact resolution order:
    // root -> configs[active] -> target.private/public + inherited public -> target.configs[active] -> overrides[]
    auto add_inc_origin = [&](const std::vector<std::string>& incs, const std::string& origin) {
        for (const auto& i : incs) {
            RecordOriginFlag(out.effectiveFlags, (is_msvc ? "/I" : "-I") + i, origin);
        }
    };
    auto add_def_origin = [&](const std::vector<std::string>& defs, const std::string& origin) {
        for (const auto& d : defs) {
            RecordOriginFlag(out.effectiveFlags, (is_msvc ? "/D" : "-D") + d, origin);
        }
    };
    auto add_raw_origin = [&](const std::vector<std::string>& flags, const std::string& origin) {
        for (const auto& f : flags) {
            for (const auto& tok : CompilerManager::SplitFlags(f)) {
                RecordOriginFlag(out.effectiveFlags, tok, origin);
            }
        }
    };

    add_inc_origin(project.GetIncludeDirs(), "project");
    add_def_origin(project.GetDefines(), "project");
    add_raw_origin(project.GetCompileFlags(), "project");

    ConfigOverrides root_cfg = project.GetConfigOverrides(config_name);
    if (cfg.defineDebug) RecordOriginFlag(out.effectiveFlags, std::string(is_msvc ? "/D" : "-D") + "_DEBUG", "preset: " + out.basePreset);
    if (cfg.defineNdebug) RecordOriginFlag(out.effectiveFlags, std::string(is_msvc ? "/D" : "-D") + "NDEBUG", "preset: " + out.basePreset);
    add_def_origin(root_cfg.defines, "config: " + config_name);
    add_raw_origin(root_cfg.compileFlags, "config: " + config_name);

    add_inc_origin(target.priv.includeDirs, "target: " + target.name + " (private)");
    add_def_origin(target.priv.defines, "target: " + target.name + " (private)");
    add_raw_origin(target.priv.compileFlags, "target: " + target.name + " (private)");

    add_inc_origin(target.pub.includeDirs, "target: " + target.name + " (public)");
    add_def_origin(target.pub.defines, "target: " + target.name + " (public)");
    add_raw_origin(target.pub.compileFlags, "target: " + target.name + " (public)");

    // Inherited includes/defines (anything in target_eff not from project or target itself)
    for (const auto& inc : target_eff.includeDirs) {
        bool own = (std::find(project.GetIncludeDirs().begin(), project.GetIncludeDirs().end(), inc) != project.GetIncludeDirs().end()) ||
                   (std::find(target.priv.includeDirs.begin(), target.priv.includeDirs.end(), inc) != target.priv.includeDirs.end()) ||
                   (std::find(target.pub.includeDirs.begin(), target.pub.includeDirs.end(), inc) != target.pub.includeDirs.end());
        if (!own) RecordOriginFlag(out.effectiveFlags, (is_msvc ? "/I" : "-I") + inc, "inherited dependency (public)");
    }
    for (const auto& def : target_eff.defines) {
        bool own = (std::find(project.GetDefines().begin(), project.GetDefines().end(), def) != project.GetDefines().end()) ||
                   (std::find(root_cfg.defines.begin(), root_cfg.defines.end(), def) != root_cfg.defines.end()) ||
                   (std::find(target.priv.defines.begin(), target.priv.defines.end(), def) != target.priv.defines.end()) ||
                   (std::find(target.pub.defines.begin(), target.pub.defines.end(), def) != target.pub.defines.end());
        if (!own) RecordOriginFlag(out.effectiveFlags, (is_msvc ? "/D" : "-D") + def, "inherited / target");
    }

    auto tcfg_it = target.configs.find(config_name);
    if (tcfg_it != target.configs.end()) {
        std::string org = "target config: " + target.name + " [" + config_name + "]";
        add_inc_origin(tcfg_it->second.includeDirs, org);
        add_def_origin(tcfg_it->second.defines, org);
        add_raw_origin(tcfg_it->second.compileFlags, org);
    }

    // Record base optimize/debug flags before overrides
    std::string opt_origin = "preset: " + out.basePreset;
    if (!root_cfg.optimize.empty()) opt_origin = "config: " + config_name;
    for (const auto& f : GetConfigCompileFlags(cfg, "", "", is_msvc, is_clang)) {
        RecordOriginFlag(out.effectiveFlags, f, opt_origin);
    }

    if (!out.base.warnings.empty()) {
        RecordOriginFlag(out.effectiveFlags, out.base.warnings, "project/target");
    }

    // Apply matched overrides in listed order
    for (const FlagOverride* ov : matched_overrides) {
        std::string org = "override: " + ov->match;
        out.matchedOverridePatterns.push_back(ov->match);

        AppendUniqueRes(out.base.includeDirs, ov->settings.includeDirs);
        AppendUniqueRes(out.base.systemIncludeDirs, ov->settings.systemIncludeDirs);
        AppendUniqueRes(out.base.defines, ov->settings.defines);
        for (const auto& cf : ov->settings.compileFlags) {
            out.base.compileFlags.push_back(cf);
        }
        for (const auto& lf : ov->settings.linkFlags) {
            out.base.linkFlags.push_back(lf);
        }
        if (!ov->settings.cxxStd.empty()) out.base.cxxStd = ov->settings.cxxStd;
        if (!ov->settings.cStd.empty()) out.base.cStd = ov->settings.cStd;
        if (!ov->settings.warnings.empty()) {
            out.base.warnings = ov->settings.warnings;
            RecordOriginFlag(out.effectiveFlags, ov->settings.warnings, org);
        }
        if (!ov->optimize.empty()) {
            out.optimize = ov->optimize;
            for (const auto& f : GetConfigCompileFlags(cfg, ov->optimize, "none", is_msvc, is_clang)) {
                if (f.rfind("-O", 0) == 0 || f.rfind("/O", 0) == 0) {
                    RecordOriginFlag(out.effectiveFlags, f, org);
                }
            }
        }
        if (!ov->debugInfo.empty()) {
            out.debugInfo = ov->debugInfo;
        }
        if (ov->excludeFromPch) out.excludeFromPch = true;
        if (ov->excludeFromUnity) out.excludeFromUnity = true;

        add_inc_origin(ov->settings.includeDirs, org);
        add_def_origin(ov->settings.defines, org);
        add_raw_origin(ov->settings.compileFlags, org);

        // Per-config override inside FlagOverride
        auto ocfg_it = ov->configs.find(config_name);
        if (ocfg_it != ov->configs.end()) {
            std::string cfg_org = org + " [" + config_name + "]";
            const auto& oc = ocfg_it->second;
            AppendUniqueRes(out.base.defines, oc.defines);
            for (const auto& cf : oc.compileFlags) out.base.compileFlags.push_back(cf);
            for (const auto& lf : oc.linkFlags) out.base.linkFlags.push_back(lf);
            if (!oc.warnings.empty()) {
                out.base.warnings = oc.warnings;
                RecordOriginFlag(out.effectiveFlags, oc.warnings, cfg_org);
            }
            if (!oc.cxxStd.empty()) out.base.cxxStd = oc.cxxStd;
            if (!oc.cStd.empty()) out.base.cStd = oc.cStd;
            if (!oc.optimize.empty()) {
                out.optimize = oc.optimize;
                for (const auto& f : GetConfigCompileFlags(cfg, oc.optimize, "none", is_msvc, is_clang)) {
                    if (f.rfind("-O", 0) == 0 || f.rfind("/O", 0) == 0) {
                        RecordOriginFlag(out.effectiveFlags, f, cfg_org);
                    }
                }
            }
            if (!oc.debugInfo.empty()) out.debugInfo = oc.debugInfo;
            add_def_origin(oc.defines, cfg_org);
            add_raw_origin(oc.compileFlags, cfg_org);
        }

        // Build concise tooltip summary for this override
        std::ostringstream sum;
        bool has_part = false;
        for (const auto& f : ov->settings.compileFlags) {
            if (has_part) sum << " ";
            sum << f;
            has_part = true;
        }
        for (const auto& d : ov->settings.defines) {
            if (has_part) sum << " ";
            sum << "-D" << d;
            has_part = true;
        }
        if (!ov->settings.warnings.empty()) {
            if (has_part) sum << ", ";
            sum << "warnings " << ov->settings.warnings;
            has_part = true;
        }
        if (!ov->optimize.empty()) {
            if (has_part) sum << ", ";
            sum << ov->optimize;
            has_part = true;
        }
        if (ocfg_it != ov->configs.end() && !ocfg_it->second.optimize.empty()) {
            if (has_part) sum << ", ";
            sum << config_name << ":" << ocfg_it->second.optimize;
            has_part = true;
        }
        if (!has_part) sum << "custom settings";
        out.matchedOverrideSummaries.push_back(sum.str() + " (" + ov->match + ")");
    }
}

EffectiveFileSettings ResolveFileSettings(
    const Project& project,
    const Target& target,
    const EffectiveTargetSettings& target_eff,
    const std::string& file_rel_path,
    const std::string& config_name,
    const std::string& compiler_name,
    bool is_c_source
) {
    (void)is_c_source;
    ConditionContext cond_ctx = BuildConditionContext(project);
    if (!config_name.empty()) cond_ctx.config = config_name;
    if (!compiler_name.empty()) {
        std::string comp_low = ToLowerRes(compiler_name);
        if (comp_low == "cl" || comp_low == "cl.exe") cond_ctx.compilerFamily = "msvc";
        else if (comp_low == "clang-cl" || comp_low == "clangcl") cond_ctx.compilerFamily = "clangcl";
        else if (comp_low.find("clang") != std::string::npos) cond_ctx.compilerFamily = "clang";
        else cond_ctx.compilerFamily = "gcc";
        cond_ctx.toolchain = compiler_name;
    }

    std::vector<const FlagOverride*> matched;
    for (const auto& ov : target.overrides) {
        if (!ov.condition.empty() && !EvaluateCondition(ov.condition, cond_ctx).active) {
            continue;
        }
        CompiledGlob cg = CompiledGlob::Compile(ov.match);
        if (cg.Matches(file_rel_path)) {
            matched.push_back(&ov);
        }
    }

    EffectiveFileSettings out;
    ResolveFromMatchedOverrides(project, target, target_eff, matched, config_name, compiler_name, out);
    return out;
}

std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>> ResolveTargetFilesGrouped(
    const Project& project,
    const Target& target,
    const EffectiveTargetSettings& target_eff,
    const std::vector<std::string>& files,
    const std::string& config_name,
    const std::string& compiler_name
) {
    ConditionContext cond_ctx = BuildConditionContext(project);
    if (!config_name.empty()) cond_ctx.config = config_name;
    if (!compiler_name.empty()) {
        std::string comp_low = ToLowerRes(compiler_name);
        if (comp_low == "cl" || comp_low == "cl.exe") cond_ctx.compilerFamily = "msvc";
        else if (comp_low == "clang-cl" || comp_low == "clangcl") cond_ctx.compilerFamily = "clangcl";
        else if (comp_low.find("clang") != std::string::npos) cond_ctx.compilerFamily = "clang";
        else cond_ctx.compilerFamily = "gcc";
        cond_ctx.toolchain = compiler_name;
    }

    struct PrecompiledOv {
        const FlagOverride* ov = nullptr;
        CompiledGlob glob;
        bool active = true;
    };
    std::vector<PrecompiledOv> compiled_ovs;
    compiled_ovs.reserve(target.overrides.size());
    for (const auto& ov : target.overrides) {
        PrecompiledOv po;
        po.ov = &ov;
        po.active = ov.condition.empty() || EvaluateCondition(ov.condition, cond_ctx).active;
        if (po.active) {
            po.glob = CompiledGlob::Compile(ov.match);
        }
        compiled_ovs.push_back(std::move(po));
    }

    // Group files by override signature (bitstring / index signature)
    std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>> sig_cache;
    std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>> result;
    result.reserve(files.size());

    for (const auto& f : files) {
        std::string sig;
        std::vector<const FlagOverride*> matched;
        for (size_t i = 0; i < compiled_ovs.size(); ++i) {
            if (compiled_ovs[i].active && compiled_ovs[i].glob.Matches(f)) {
                sig += std::to_string(i) + ",";
                matched.push_back(compiled_ovs[i].ov);
            }
        }

        auto cache_it = sig_cache.find(sig);
        if (cache_it == sig_cache.end()) {
            auto resolved = std::make_shared<EffectiveFileSettings>();
            ResolveFromMatchedOverrides(project, target, target_eff, matched, config_name, compiler_name, *resolved);
            cache_it = sig_cache.emplace(sig, std::move(resolved)).first;
        }
        result[f] = cache_it->second;
    }

    return result;
}

std::vector<std::string> ValidateTargetOverrides(const Target& target) {
    std::vector<std::string> warnings;
    for (const auto& ov : target.overrides) {
        if (!ov.condition.empty()) {
            std::string cerr;
            if (!ValidateCondition(ov.condition, cerr)) {
                warnings.push_back("Target '" + target.name + "' override '" + ov.match + "' has invalid condition '" + ov.condition + "': " + cerr);
            }
        }
        if (ov.match.empty()) {
            warnings.push_back("Target '" + target.name + "' has an override with an empty match pattern.");
            continue;
        }
        CompiledGlob cg = CompiledGlob::Compile(ov.match);
        bool any = false;
        for (const auto& f : target.expandedFiles) {
            if (cg.Matches(f)) {
                any = true;
                break;
            }
        }
        if (!any) {
            warnings.push_back("Target '" + target.name + "' override '" + ov.match + "' matches no files.");
        }
    }
    return warnings;
}

static size_t EditDistance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> dp(m + 1), next_dp(m + 1);
    for (size_t j = 0; j <= m; ++j) dp[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        next_dp[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            next_dp[j] = std::min({ next_dp[j - 1] + 1, dp[j] + 1, dp[j - 1] + cost });
        }
        dp.swap(next_dp);
    }
    return dp[m];
}

std::string SuggestOverrideFieldName(const std::string& unknown_key) {
    static const char* kKnown[] = {
        "match", "if", "includeDirs", "systemIncludeDirs", "defines",
        "compileFlags", "linkFlags", "cxxStd", "cStd", "warnings",
        "optimize", "debugInfo", "excludeFromPch", "excludeFromUnity", "configs"
    };
    std::string low = ToLowerRes(unknown_key);
    std::string best;
    size_t best_dist = 4;
    for (const char* k : kKnown) {
        size_t d = EditDistance(low, ToLowerRes(k));
        if (d < best_dist) {
            best_dist = d;
            best = k;
        }
    }
    return best;
}

bool CheckRuntimeConsistency(
    const Project& project,
    const std::vector<const Target*>& build_order,
    const std::unordered_map<std::string, const Target*>& target_map,
    const std::string& config_name,
    std::string& out_error
) {
    out_error.clear();
    ResolvedConfigParams root_cfg = ResolveConfigParams(project, config_name);

    auto get_target_runtime = [&](const Target* t) -> std::string {
        std::string rt = root_cfg.runtime;
        if (!t->pub.runtime.empty()) rt = t->pub.runtime;
        if (!t->priv.runtime.empty()) rt = t->priv.runtime;
        auto it = t->configs.find(config_name);
        if (it != t->configs.end() && !it->second.runtime.empty()) {
            rt = it->second.runtime;
        }
        return ToLowerRes(rt);
    };

    for (const Target* t : build_order) {
        if (t->kind != TargetKind::Executable && t->kind != TargetKind::Shared) continue;
        std::string t_rt = get_target_runtime(t);

        for (const auto* dep_list : { &t->deps, &t->publicDeps }) {
            for (const auto& dep_name : *dep_list) {
                auto it = target_map.find(dep_name);
                if (it != target_map.end() && it->second->kind == TargetKind::Static) {
                    std::string dep_rt = get_target_runtime(it->second);
                    if (dep_rt != t_rt) {
                        out_error = "Runtime CRT mismatch: '" + it->second->name + "' uses " + dep_rt +
                                    " CRT, '" + t->name + "' uses " + t_rt + " CRT.";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

} // namespace core
