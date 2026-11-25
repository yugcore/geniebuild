#include "target.h"
#include "project.h"

#include <algorithm>
#include <cctype>
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
           !sources.empty() ||
           !exclude.empty() ||
           !deps.empty() ||
           !publicDeps.empty() ||
           !outputName.empty() ||
           exportAll ||
           !pub.IsEmpty() ||
           !priv.IsEmpty() ||
           !configs.empty() ||
           !overrides.empty();
}

std::filesystem::path Target::GetObjectDir(const std::filesystem::path& project_root, const std::string& config) const {
    return project_root / ".geniebuild" / "obj" / SanitizeConfigDirName(config) / name;
}

std::filesystem::path Target::GetBinaryPath(const std::filesystem::path& abs_output_dir, const std::string& config, bool is_msvc) const {
    std::string out = GetEffectiveOutputName();
    std::string cfg_dir = SanitizeConfigDirName(config);
    switch (kind) {
        case TargetKind::Executable: {
#if defined(_WIN32)
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

bool ResolveTargetBuildOrder(
    const std::vector<Target>& targets,
    const std::string& root_target,
    std::vector<const Target*>& out_order,
    std::string& out_error
) {
    out_order.clear();
    out_error.clear();

    if (targets.empty()) {
        out_error = "No targets defined in project.";
        return false;
    }

    std::unordered_map<std::string, const Target*> by_name;
    for (const auto& t : targets) {
        if (t.name.empty()) {
            out_error = "Target name cannot be empty.";
            return false;
        }
        if (!by_name.emplace(t.name, &t).second) {
            out_error = "Duplicate target name: '" + t.name + "'.";
            return false;
        }
    }

    // Validate duplicate (outputName, kind) for targets that produce outputs
    std::map<std::pair<int, std::string>, std::string> output_owners;
    for (const auto& t : targets) {
        if (t.kind == TargetKind::Interface) continue;
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
        seeds.push_back(it->second);
    } else {
        for (const auto& t : targets) {
            seeds.push_back(&t);
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

static void ApplySettingsBlock(EffectiveTargetSettings& eff, const SettingsBlock& blk) {
    AppendUnique(eff.includeDirs, blk.includeDirs);
    AppendUnique(eff.systemIncludeDirs, blk.systemIncludeDirs);
    AppendUnique(eff.defines, blk.defines);
    AppendAll(eff.compileFlags, blk.compileFlags);
    AppendAll(eff.linkFlags, blk.linkFlags);
    AppendUnique(eff.libDirs, blk.libDirs);
    AppendUnique(eff.libs, blk.libs);
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

    // 1. Root settings + root per-config overrides
    eff.includeDirs = project.GetIncludeDirs();
    eff.libDirs = project.GetLibDirs();
    eff.libs = project.GetLibs();
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

    // 2. Target's own private, public, and target-config settings
    ApplySettingsBlock(eff, target.priv);
    ApplySettingsBlock(eff, target.pub);
    auto tcfg_it = target.configs.find(config);
    if (tcfg_it != target.configs.end()) {
        ApplySettingsBlock(eff, tcfg_it->second);
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
        AppendUnique(eff.libs, dep->pub.libs);
        AppendAll(eff.linkFlags, dep->pub.linkFlags);
        AppendUnique(eff.copyDlls, dep->pub.copyDlls);

        // Static libraries also propagate their private link requirements to final links
        if (dep->kind == TargetKind::Static) {
            AppendUnique(eff.libDirs, dep->priv.libDirs);
            AppendUnique(eff.libs, dep->priv.libs);
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

} // namespace core
