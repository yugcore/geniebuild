#include "target.h"
#include "project.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <unordered_set>

namespace core {

static std::string ToLowerTarget(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string TargetKindToString(TargetKind kind) {
    switch (kind) {
        case TargetKind::Executable: return "executable";
        case TargetKind::Static:     return "static";
        case TargetKind::Shared:     return "shared";
        case TargetKind::Interface:  return "interface";
    }
    return "executable";
}

TargetKind TargetKindFromString(const std::string& str) {
    std::string low = ToLowerTarget(str);
    if (low == "static") return TargetKind::Static;
    if (low == "shared") return TargetKind::Shared;
    if (low == "interface") return TargetKind::Interface;
    return TargetKind::Executable;
}

std::string Target::GetEffectiveOutputName() const {
    return outputName.empty() ? name : outputName;
}

std::string Target::GetExportsMacro() const {
    std::string base = name;
    std::string out;
    out.reserve(base.size() + 8);
    for (unsigned char c : base) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::toupper(c)));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "TARGET";
    out += "_EXPORTS";
    return out;
}

bool Target::HasV2Fields() const {
    return kind != TargetKind::Executable ||
           !condition.empty() ||
           !sources.empty() ||
           !exclude.empty() ||
           !deps.empty() ||
           !publicDeps.empty() ||
           !outputName.empty() ||
           exportAll ||
           freestanding ||
           !linkerScript.empty() ||
           !entry.empty() ||
           !cxxFlags.empty() ||
           linkLibgcc ||
           !run.IsEmpty() ||
           !debug.IsEmpty() ||
           !pub.IsEmpty() ||
           !priv.IsEmpty() ||
           !configs.empty() ||
           !overrides.empty() ||
           !steps.empty() ||
           !pch.IsEmpty() ||
           unity.enabled;
}

std::filesystem::path Target::GetObjectDir(const std::filesystem::path& project_root, const std::string& config, const std::string& tc_hash8) const {
    std::string cfg_seg = SanitizeConfigDirName(config);
    if (!tc_hash8.empty()) {
        cfg_seg += "-" + tc_hash8;
    }
    return project_root / ".geniebuild" / "obj" / cfg_seg / name;
}

std::filesystem::path Target::GetGenDir(const std::filesystem::path& project_root, const std::string& config) const {
    return project_root / ".geniebuild" / "gen" / SanitizeConfigDirName(config) / name;
}

std::filesystem::path Target::GetBinaryPath(const std::filesystem::path& abs_output_dir, const std::string& config, bool is_msvc) const {
    std::string out = GetEffectiveOutputName();
    std::string cfg_dir = SanitizeConfigDirName(config);
    switch (kind) {
        case TargetKind::Executable: {
#if defined(_WIN32)
            if (freestanding) {
                return abs_output_dir / cfg_dir / "bin" / out;
            }
            return abs_output_dir / cfg_dir / "bin" / (out + ".exe");
#else
            return abs_output_dir / cfg_dir / "bin" / out;
#endif
        }
        case TargetKind::Static: {
            std::string lib_name = is_msvc ? (out + ".lib") : ("lib" + out + ".a");
            return abs_output_dir / cfg_dir / "lib" / lib_name;
        }
        case TargetKind::Shared: {
#if defined(_WIN32)
            return abs_output_dir / cfg_dir / "bin" / (out + ".dll");
#else
            return abs_output_dir / cfg_dir / "bin" / ("lib" + out + ".so");
#endif
        }
        case TargetKind::Interface:
            return std::filesystem::path();
    }
    return std::filesystem::path();
}

std::filesystem::path Target::GetImportLibPath(const std::filesystem::path& abs_output_dir, const std::string& config, bool is_msvc) const {
    if (kind != TargetKind::Shared) {
        return std::filesystem::path();
    }
    std::string out = GetEffectiveOutputName();
    std::string lib_name = is_msvc ? (out + ".lib") : ("lib" + out + ".dll.a");
    return abs_output_dir / SanitizeConfigDirName(config) / "lib" / lib_name;
}

bool Target::IsFileEnabled(const std::string& rel_path) const {
    std::string norm = rel_path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    if (norm.rfind("./", 0) == 0) norm = norm.substr(2);
    return (expandedFilesSet.find(norm) != expandedFilesSet.end()) &&
           (disabledFiles.find(norm) == disabledFiles.end());
}

size_t Target::GetEnabledFilesCount() const {
    size_t count = 0;
    for (const auto& f : expandedFiles) {
        if (disabledFiles.find(f) == disabledFiles.end()) {
            count++;
        }
    }
    return count;
}

std::vector<std::string> Target::GetCompiledFiles() const {
    std::vector<std::string> out;
    out.reserve(expandedFiles.size());
    for (const auto& f : expandedFiles) {
        if (disabledFiles.find(f) == disabledFiles.end()) {
            out.push_back(f);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Dependency Resolution & Topological Sort
// -----------------------------------------------------------------------------

bool ValidateAllTargetConditions(
    const std::vector<Target>& targets,
    std::string& out_error
) {
    out_error.clear();
    std::string err;
    for (const auto& t : targets) {
        if (!t.condition.empty() && !ValidateCondition(t.condition, err)) {
            out_error = "Target '" + t.name + "' has invalid condition '" + t.condition + "': " + err;
            return false;
        }
        for (const auto& r : t.sources) {
            if (!r.condition.empty() && !ValidateCondition(r.condition, err)) {
                out_error = "Target '" + t.name + "' source rule '" + r.include + "' has invalid condition '" + r.condition + "': " + err;
                return false;
            }
        }
        for (const auto& ov : t.overrides) {
            if (!ov.condition.empty() && !ValidateCondition(ov.condition, err)) {
                out_error = "Target '" + t.name + "' override '" + ov.match + "' has invalid condition '" + ov.condition + "': " + err;
                return false;
            }
        }
        for (const auto& st : t.steps) {
            if (!st.condition.empty() && !ValidateCondition(st.condition, err)) {
                out_error = "Target '" + t.name + "' step '" + st.name + "' has invalid condition '" + st.condition + "': " + err;
                return false;
            }
        }
        auto check_blk_libs = [&](const SettingsBlock& blk, const std::string& scope) -> bool {
            for (const auto& [lib_name, cond] : blk.libConditions) {
                if (!cond.empty() && !ValidateCondition(cond, err)) {
                    out_error = "Target '" + t.name + "' (" + scope + ") library '" + lib_name + "' has invalid condition '" + cond + "': " + err;
                    return false;
                }
            }
            return true;
        };
        if (!check_blk_libs(t.pub, "public") || !check_blk_libs(t.priv, "private")) {
            return false;
        }
        for (const auto& [cname, cblk] : t.configs) {
            if (!check_blk_libs(cblk, "config:" + cname)) {
                return false;
            }
        }
    }
    return true;
}

bool ResolveTargetBuildOrder(
    const std::vector<Target>& targets,
    const std::string& root_target,
    std::vector<const Target*>& out_order,
    std::string& out_error,
    const ConditionContext* cond_ctx
) {
    out_order.clear();
    out_error.clear();

    if (targets.empty()) {
        out_error = "No targets defined in project.";
        return false;
    }

    if (cond_ctx) {
        if (!ValidateAllTargetConditions(targets, out_error)) {
            return false;
        }
    }

    std::unordered_map<std::string, const Target*> by_name;
    std::unordered_map<std::string, ConditionEvalResult> active_eval;
    for (const auto& t : targets) {
        if (t.name.empty()) {
            out_error = "Target name cannot be empty.";
            return false;
        }
        if (!by_name.emplace(t.name, &t).second) {
            out_error = "Duplicate target name: '" + t.name + "'.";
            return false;
        }
        if (cond_ctx && !t.condition.empty()) {
            active_eval[t.name] = EvaluateCondition(t.condition, *cond_ctx);
        } else {
            active_eval[t.name] = ConditionEvalResult{true, true, "", ""};
        }
    }

    // Validate duplicate (outputName, kind) among active targets that produce outputs
    std::map<std::pair<int, std::string>, std::string> output_owners;
    for (const auto& t : targets) {
        if (t.kind == TargetKind::Interface) continue;
        if (!active_eval[t.name].active) continue;
        std::string eff_out = ToLowerTarget(t.GetEffectiveOutputName());
        auto key = std::make_pair(static_cast<int>(t.kind), eff_out);
        auto it = output_owners.find(key);
        if (it != output_owners.end()) {
            out_error = "Two " + TargetKindToString(t.kind) + " targets ('" + it->second +
                        "' and '" + t.name + "') share output name '" + t.GetEffectiveOutputName() + "'.";
            return false;
        }
        output_owners[key] = t.name;
    }

    std::vector<const Target*> seeds;
    if (!root_target.empty()) {
        auto it = by_name.find(root_target);
        if (it == by_name.end()) {
            out_error = "Target '" + root_target + "' not found.";
            return false;
        }
        if (!active_eval[root_target].active) {
            out_error = "Target '" + root_target + "' is inactive (" + active_eval[root_target].reason + ").";
            return false;
        }
        seeds.push_back(it->second);
    } else {
        for (const auto& t : targets) {
            if (active_eval[t.name].active) {
                seeds.push_back(&t);
            }
        }
        if (seeds.empty()) {
            out_error = "No targets are active for the current platform/configuration.";
            return false;
        }
    }

    enum class State { Unvisited, Visiting, Visited };
    std::unordered_map<std::string, State> state;
    std::vector<std::string> stack;

    auto visit = [&](auto& self, const Target* cur) -> bool {
        auto st_it = state.find(cur->name);
        if (st_it != state.end()) {
            if (st_it->second == State::Visited) return true;
            if (st_it->second == State::Visiting) {
                // Build exact cycle chain
                auto cyc_it = std::find(stack.begin(), stack.end(), cur->name);
                std::string chain;
                for (auto it = cyc_it; it != stack.end(); ++it) {
                    if (!chain.empty()) chain += " -> ";
                    chain += *it;
                }
                chain += " -> " + cur->name;
                out_error = "Dependency cycle detected: " + chain;
                return false;
            }
        }

        state[cur->name] = State::Visiting;
        stack.push_back(cur->name);

        auto process_dep_list = [&](const std::vector<std::string>& dep_list) -> bool {
            for (const auto& dep_name : dep_list) {
                auto dep_it = by_name.find(dep_name);
                if (dep_it == by_name.end()) {
                    out_error = "Target '" + cur->name + "' depends on unknown target '" + dep_name + "'.";
                    return false;
                }
                const auto& dep_ev = active_eval[dep_name];
                if (!dep_ev.active) {
                    out_error = "Target '" + cur->name + "' depends on '" + dep_name + "', which is inactive (" + dep_ev.reason + ").";
                    return false;
                }
                if (!self(self, dep_it->second)) {
                    return false;
                }
            }
            return true;
        };

        if (!process_dep_list(cur->deps) || !process_dep_list(cur->publicDeps)) {
            return false;
        }

        stack.pop_back();
        state[cur->name] = State::Visited;
        out_order.push_back(cur);
        return true;
    };

    for (const Target* seed : seeds) {
        if (!visit(visit, seed)) {
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Usage Propagation & Effective Target Settings
// -----------------------------------------------------------------------------

static void AppendUnique(std::vector<std::string>& dst, const std::vector<std::string>& src) {
    for (const auto& item : src) {
        if (item.empty()) continue;
        if (std::find(dst.begin(), dst.end(), item) == dst.end()) {
            dst.push_back(item);
        }
    }
}

static void AppendAll(std::vector<std::string>& dst, const std::vector<std::string>& src) {
    for (const auto& item : src) {
        if (!item.empty()) {
            dst.push_back(item);
        }
    }
}

static void AppendConditionalLibs(
    std::vector<std::string>& dst,
    const std::vector<std::string>& libs,
    const std::map<std::string, std::string>& conds,
    const ConditionContext& ctx
) {
    for (const auto& lib : libs) {
        if (lib.empty()) continue;
        auto it = conds.find(lib);
        if (it != conds.end() && !it->second.empty()) {
            if (!EvaluateCondition(it->second, ctx).active) {
                continue;
            }
        }
        if (std::find(dst.begin(), dst.end(), lib) == dst.end()) {
            dst.push_back(lib);
        }
    }
}

static void ApplySettingsBlock(EffectiveTargetSettings& eff, const SettingsBlock& blk, const ConditionContext& ctx) {
    AppendUnique(eff.includeDirs, blk.includeDirs);
    AppendUnique(eff.systemIncludeDirs, blk.systemIncludeDirs);
    AppendUnique(eff.defines, blk.defines);
    AppendAll(eff.compileFlags, blk.compileFlags);
    AppendAll(eff.linkFlags, blk.linkFlags);
    AppendUnique(eff.libDirs, blk.libDirs);
    AppendConditionalLibs(eff.libs, blk.libs, blk.libConditions, ctx);
    AppendUnique(eff.copyDlls, blk.copyDlls);
    if (!blk.cxxStd.empty()) eff.cxxStd = blk.cxxStd;
    if (!blk.cStd.empty()) eff.cStd = blk.cStd;
    if (!blk.warnings.empty()) eff.warnings = blk.warnings;
}

// Applies public compile settings (and transitive publicDeps' public compile settings)
static void CollectPublicCompileUsage(
    const Target& dep_target,
    const std::unordered_map<std::string, const Target*>& target_map,
    std::unordered_set<std::string>& visited,
    EffectiveTargetSettings& eff
) {
    if (!visited.insert(dep_target.name).second) return;

    AppendUnique(eff.includeDirs, dep_target.pub.includeDirs);
    AppendUnique(eff.systemIncludeDirs, dep_target.pub.systemIncludeDirs);
    AppendUnique(eff.defines, dep_target.pub.defines);
    AppendAll(eff.compileFlags, dep_target.pub.compileFlags);

    for (const auto& pub_dep_name : dep_target.publicDeps) {
        auto it = target_map.find(pub_dep_name);
        if (it != target_map.end()) {
            CollectPublicCompileUsage(*it->second, target_map, visited, eff);
        }
    }
}

// Collects link libraries & link requirements in dependent-before-dependency order for GNU ld
static void CollectLinkRequirements(
    const Project& project,
    const Target& dep_target,
    const std::unordered_map<std::string, const Target*>& target_map,
    const std::string& config,
    bool is_msvc,
    std::unordered_set<std::string>& visited,
    std::vector<const Target*>& post_order_deps
) {
    if (!visited.insert(dep_target.name).second) return;

    // If dep is static or interface, its own deps & publicDeps must also reach the final link.
    // If dep is shared, its publicDeps propagate too.
    if (dep_target.kind == TargetKind::Static || dep_target.kind == TargetKind::Interface) {
        for (const auto& d : dep_target.deps) {
            auto it = target_map.find(d);
            if (it != target_map.end()) {
                CollectLinkRequirements(project, *it->second, target_map, config, is_msvc, visited, post_order_deps);
            }
        }
    }
    for (const auto& pd : dep_target.publicDeps) {
        auto it = target_map.find(pd);
        if (it != target_map.end()) {
            CollectLinkRequirements(project, *it->second, target_map, config, is_msvc, visited, post_order_deps);
        }
    }

    post_order_deps.push_back(&dep_target);
}

EffectiveTargetSettings ResolveEffectiveTargetSettings(
    const Project& project,
    const Target& target,
    const std::unordered_map<std::string, const Target*>& target_map,
    const std::string& config,
    bool is_msvc,
    bool static_needs_pic
) {
    EffectiveTargetSettings eff;
    ConditionContext cond_ctx = BuildConditionContext(project);

    // 1. Root settings + root per-config overrides
    eff.includeDirs = project.GetIncludeDirs();
    eff.libDirs = project.GetLibDirs();
    AppendConditionalLibs(eff.libs, project.GetLibs(), project.GetLibConditions(), cond_ctx);
    eff.defines = project.GetDefines();
    eff.copyDlls = project.GetCopyDlls();
    eff.compileFlags = project.GetCompileFlags();
    eff.linkFlags = project.GetLinkFlags();
    eff.cxxStd = project.GetCxxStd();
    eff.cStd = project.GetCStd();
    eff.warnings = project.GetWarnings();

    ConfigOverrides root_cfg = project.GetConfigOverrides(config);
    AppendUnique(eff.defines, root_cfg.defines);
    AppendAll(eff.compileFlags, root_cfg.compileFlags);
    AppendAll(eff.linkFlags, root_cfg.linkFlags);

    // 2. Target's own private, public, and target-config settings + kernel fields
    eff.cxxFlags = target.cxxFlags;
    eff.freestanding = target.freestanding;
    eff.linkerScript = target.linkerScript;
    eff.entry = target.entry;
    eff.linkLibgcc = target.linkLibgcc;
    ApplySettingsBlock(eff, target.priv, cond_ctx);
    ApplySettingsBlock(eff, target.pub, cond_ctx);
    auto tcfg_it = target.configs.find(config);
    if (tcfg_it != target.configs.end()) {
        ApplySettingsBlock(eff, tcfg_it->second, cond_ctx);
    }

    // V2-6 / V2-7: If any active preBuild step sets generatesSources: true OR target has .ispc files, add {gen} to private includeDirs
    bool needs_gen_inc = false;
    for (const auto& step : target.steps) {
        if (step.phase == StepPhase::PreBuild && step.generatesSources) {
            if (step.condition.empty() || EvaluateCondition(step.condition, cond_ctx).active) {
                needs_gen_inc = true;
                break;
            }
        }
    }
    if (!needs_gen_inc) {
        for (const auto& f : target.expandedFiles) {
            if (f.size() >= 5 && ToLowerTarget(f.substr(f.size() - 5)) == ".ispc") {
                needs_gen_inc = true;
                break;
            }
        }
    }
    if (needs_gen_inc) {
        std::string gen_rel = GetTargetGenDirRel(config, target.name);
        if (std::find(eff.includeDirs.begin(), eff.includeDirs.end(), gen_rel) == eff.includeDirs.end()) {
            eff.includeDirs.push_back(gen_rel);
        }
    }

    // Auto-define <NAME>_EXPORTS when compiling a shared target
    if (target.kind == TargetKind::Shared) {
        std::string macro = target.GetExportsMacro();
        if (std::find(eff.defines.begin(), eff.defines.end(), macro) == eff.defines.end()) {
            eff.defines.push_back(macro);
        }
    }

#if !defined(_WIN32)
    if (target.kind == TargetKind::Shared || (target.kind == TargetKind::Static && static_needs_pic)) {
        if (std::find(eff.compileFlags.begin(), eff.compileFlags.end(), "-fPIC") == eff.compileFlags.end()) {
            eff.compileFlags.push_back("-fPIC");
        }
    }
#else
    (void)static_needs_pic;
#endif

    // 3. Inherited compile usage from direct deps & publicDeps (and transitively publicDeps)
    std::unordered_set<std::string> compile_visited;
    compile_visited.insert(target.name);
    for (const auto& d : target.deps) {
        auto it = target_map.find(d);
        if (it != target_map.end()) {
            CollectPublicCompileUsage(*it->second, target_map, compile_visited, eff);
        }
    }
    for (const auto& pd : target.publicDeps) {
        auto it = target_map.find(pd);
        if (it != target_map.end()) {
            CollectPublicCompileUsage(*it->second, target_map, compile_visited, eff);
        }
    }

    // 4. Link requirements (in dependent-before-dependency order for GNU ld)
    std::unordered_set<std::string> link_visited;
    link_visited.insert(target.name);
    std::vector<const Target*> post_order_deps;

    for (const auto& d : target.deps) {
        auto it = target_map.find(d);
        if (it != target_map.end()) {
            CollectLinkRequirements(project, *it->second, target_map, config, is_msvc, link_visited, post_order_deps);
        }
    }
    for (const auto& pd : target.publicDeps) {
        auto it = target_map.find(pd);
        if (it != target_map.end()) {
            CollectLinkRequirements(project, *it->second, target_map, config, is_msvc, link_visited, post_order_deps);
        }
    }

    // post_order_deps has dependencies before dependents; reverse so dependents appear first on linker CLI
    std::reverse(post_order_deps.begin(), post_order_deps.end());
    std::filesystem::path abs_out = project.GetAbsoluteOutputDir();

    for (const Target* dep : post_order_deps) {
        // Propagate public link settings
        AppendUnique(eff.libDirs, dep->pub.libDirs);
        AppendConditionalLibs(eff.libs, dep->pub.libs, dep->pub.libConditions, cond_ctx);
        AppendAll(eff.linkFlags, dep->pub.linkFlags);
        AppendUnique(eff.copyDlls, dep->pub.copyDlls);

        // Static libraries also propagate their private link requirements to final links
        if (dep->kind == TargetKind::Static) {
            AppendUnique(eff.libDirs, dep->priv.libDirs);
            AppendConditionalLibs(eff.libs, dep->priv.libs, dep->priv.libConditions, cond_ctx);
            AppendAll(eff.linkFlags, dep->priv.linkFlags);
            AppendUnique(eff.copyDlls, dep->priv.copyDlls);

            std::filesystem::path archive_p = dep->GetBinaryPath(abs_out, config, is_msvc);
            eff.depLinkLibraries.push_back(archive_p);
            eff.staticDepLibraries.push_back(archive_p);
        } else if (dep->kind == TargetKind::Shared) {
            std::filesystem::path implib_p = dep->GetImportLibPath(abs_out, config, is_msvc);
            eff.depLinkLibraries.push_back(implib_p);
            eff.sharedDepImportLibs.emplace_back(dep->name, implib_p);
        }
    }

    return eff;
}

bool IsStaticTargetConsumedByShared(
    const std::vector<Target>& targets,
    const std::string& static_target_name
) {
    std::unordered_map<std::string, const Target*> by_name;
    for (const auto& t : targets) {
        by_name[t.name] = &t;
    }

    for (const auto& t : targets) {
        if (t.kind != TargetKind::Shared) continue;
        std::unordered_set<std::string> visited;
        std::vector<const Target*> stack = { &t };
        while (!stack.empty()) {
            const Target* cur = stack.back();
            stack.pop_back();
            if (!visited.insert(cur->name).second) continue;
            for (const auto& d : cur->deps) {
                if (d == static_target_name) return true;
                auto it = by_name.find(d);
                if (it != by_name.end()) stack.push_back(it->second);
            }
            for (const auto& pd : cur->publicDeps) {
                if (pd == static_target_name) return true;
                auto it = by_name.find(pd);
                if (it != by_name.end()) stack.push_back(it->second);
            }
        }
    }
    return false;
}

void CreateKernelStarterTemplate(
    const std::filesystem::path& project_root,
    const std::string& target_name,
    KernelBootProtocol protocol,
    Target& out_target,
    std::vector<std::string>* out_created_files
) {
    out_target = Target{};
    out_target.name = target_name.empty() ? "kernel" : target_name;
    out_target.kind = TargetKind::Executable;
    out_target.freestanding = true;
    out_target.linkerScript = "kernel/linker.ld";
    out_target.entry = "_start";
    out_target.outputName = "kernel.elf";
    out_target.linkLibgcc = true;
    out_target.priv.compileFlags = {
        "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-mno-red-zone", "-mcmodel=kernel"
    };
    out_target.priv.linkFlags = { "-nostdlib", "-static", "-z", "max-page-size=0x1000" };
    out_target.cxxFlags = {
        "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit"
    };

    auto write_if_missing = [&](const std::string& rel, const std::string& content) {
        std::filesystem::path abs_p = project_root / rel;
        std::error_code ec;
        if (std::filesystem::exists(abs_p, ec)) return;
        std::filesystem::create_directories(abs_p.parent_path(), ec);
        std::ofstream f(abs_p, std::ios::binary | std::ios::trunc);
        if (f.is_open()) {
            f << content;
            if (out_created_files) out_created_files->push_back(rel);
        }
    };

    const char* kLinkerLd =
R"(ENTRY(_start)
SECTIONS {
    . = 1M;
    .text ALIGN(4K) : {
        KEEP(*(.multiboot2))
        *(.text*)
    }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    }
}
)";

    const char* kKernelC =
R"(#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void serial_puts(const char* s) {
    while (*s) {
        outb(0x3F8, (uint8_t)*s++);
    }
}

void kernel_main(void) {
    serial_puts("Hello from kernel\n");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
)";

    if (protocol == KernelBootProtocol::Multiboot2) {
        SourceRule r1; r1.include = "kernel/**/*.{c,cpp,S,asm}";
        SourceRule r2; r2.include = "boot/**/*.S";
        out_target.sources = { r1, r2 };

        const char* kBootS =
R"(.section .multiboot2, "a"
.align 8
mb2_start:
    .long 0xE85250D6
    .long 0
    .long mb2_end - mb2_start
    .long -(0xE85250D6 + 0 + (mb2_end - mb2_start))
    .short 0
    .short 0
    .long 8
mb2_end:

.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %rsp
    call kernel_main
.halt_loop:
    cli
    hlt
    jmp .halt_loop

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
)";

        const char* kGrubCfg =
R"(set timeout=0
set default=0
menuentry "GenieBuild Kernel" {
    multiboot2 /boot/kernel.elf
    boot
}
)";

        write_if_missing("boot/boot.S", kBootS);
        write_if_missing("kernel/kernel.c", kKernelC);
        write_if_missing("kernel/linker.ld", kLinkerLd);
        write_if_missing("iso/boot/grub/grub.cfg", kGrubCfg);

        BuildStep iso_step;
        iso_step.name = "iso";
        iso_step.phase = StepPhase::PostBuild;
        iso_step.inputs = { "{bin}", "iso/**" };
        iso_step.outputs = { "{bindir}/os.iso" };
        iso_step.shell = true;
#if defined(_WIN32)
        iso_step.command = "if not exist \"{gen}\\isodir\\boot\\grub\" mkdir \"{gen}\\isodir\\boot\\grub\" && copy /Y \"{bin}\" \"{gen}\\isodir\\boot\\kernel.elf\" >nul && copy /Y \"iso\\boot\\grub\\grub.cfg\" \"{gen}\\isodir\\boot\\grub\\grub.cfg\" >nul && grub-mkrescue -o \"{bindir}/os.iso\" \"{gen}/isodir\"";
#else
        iso_step.command = "mkdir -p {gen}/isodir/boot/grub && cp {bin} {gen}/isodir/boot/kernel.elf && cp iso/boot/grub/grub.cfg {gen}/isodir/boot/grub/grub.cfg && grub-mkrescue -o {bindir}/os.iso {gen}/isodir";
#endif
        out_target.steps.push_back(std::move(iso_step));

        out_target.run.command = "qemu-system-x86_64";
        out_target.run.args = { "-cdrom", "{bindir}/os.iso", "-serial", "stdio", "-m", "256M", "-no-reboot" };
        out_target.run.cwd = "{bindir}";
        out_target.run.buildBeforeRun = true;
    } else if (protocol == KernelBootProtocol::Limine) {
        SourceRule r1; r1.include = "kernel/**/*.{c,cpp,S,asm}";
        out_target.sources = { r1 };

        const char* kLimineKernelC =
R"(#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void _start(void) {
    const char* s = "Hello from Limine kernel\n";
    while (*s) outb(0x3F8, (uint8_t)*s++);
    for (;;) __asm__ volatile ("hlt");
}
)";
        const char* kLimineConf =
R"(timeout: 0
/GenieBuild Kernel
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
)";
        write_if_missing("kernel/kernel.c", kLimineKernelC);
        write_if_missing("kernel/linker.ld", kLinkerLd);
        write_if_missing("limine.conf", kLimineConf);

        out_target.run.command = "qemu-system-x86_64";
        out_target.run.args = { "-cdrom", "{bindir}/os.iso", "-serial", "stdio", "-m", "256M", "-no-reboot" };
        out_target.run.cwd = "{bindir}";
        out_target.run.buildBeforeRun = true;
    } else {
        SourceRule r1; r1.include = "kernel/**/*.{c,cpp,S,asm}";
        out_target.sources = { r1 };
        const char* kMinC =
R"(void _start(void) {
    for (;;) __asm__ volatile ("hlt");
}
)";
        write_if_missing("kernel/kernel.c", kMinC);
        write_if_missing("kernel/linker.ld", kLinkerLd);
    }
}

} // namespace core
