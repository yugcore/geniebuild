#include "build_steps.h"
#include "project.h"
#include "target.h"
#include "toolchain.h"
#include "msvc_env.h"
#include "source_rules.h"
#include "condition.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace core {

namespace {

std::string ToLowerStep(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string NormalizeStepPath(const std::string& p) {
    std::string r = p;
    std::replace(r.begin(), r.end(), '\\', '/');
    if (r.rfind("./", 0) == 0) {
        r = r.substr(2);
    }
    return r;
}

std::string MakeRelativeOrAbsPath(const std::filesystem::path& p, const std::filesystem::path& root) {
    if (p.empty()) return "";
    if (!root.empty() && p.is_absolute()) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(p, root, ec);
        if (!ec) {
            std::string rel_s = NormalizeStepPath(rel.generic_u8string());
            if (!rel_s.empty() && rel_s.rfind("..", 0) != 0) {
                return rel_s;
            }
        }
    }
    return NormalizeStepPath(p.lexically_normal().generic_u8string());
}

std::string QuoteIfNeeded(const std::string& s) {
    if (s.empty()) return "\"\"";
    if (s.find_first_of(" \t") != std::string::npos && !(s.front() == '"' && s.back() == '"')) {
        return "\"" + s + "\"";
    }
    return s;
}

bool ContainsGlobMeta(const std::string& s) {
    return s.find('*') != std::string::npos ||
           s.find('?') != std::string::npos ||
           s.find('{') != std::string::npos;
}

void WarnOnce(std::vector<std::string>* out_warnings, const std::string& msg) {
    if (!out_warnings) return;
    if (std::find(out_warnings->begin(), out_warnings->end(), msg) == out_warnings->end()) {
        out_warnings->push_back(msg);
    }
}

std::string ExpandDollarVariables(
    const std::string& text,
    const std::string& root,
    const std::string& config,
    const std::string& target,
    const std::string& toolchain,
    std::vector<std::string>* out_warnings
) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            size_t close = text.find('}', i + 2);
            if (close != std::string::npos) {
                std::string key = text.substr(i + 2, close - (i + 2));
                if (key == "root") {
                    out += root;
                } else if (key == "config") {
                    out += config;
                } else if (key == "target") {
                    out += target;
                } else if (key == "toolchain") {
                    out += toolchain;
                } else if (key.rfind("env:", 0) == 0) {
                    std::string env_name = key.substr(4);
                    const char* val = env_name.empty() ? nullptr : std::getenv(env_name.c_str());
                    if (val) {
                        out += val;
                    } else {
                        WarnOnce(out_warnings, "Environment variable '${env:" + env_name + "}' is unset.");
                    }
                } else {
                    out += text.substr(i, close - i + 1);
                }
                i = close;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string NormalizeTrustKey(const std::filesystem::path& project_root) {
    std::error_code ec;
    std::filesystem::path abs_p = project_root.is_absolute() ? project_root : std::filesystem::absolute(project_root, ec);
    if (ec) abs_p = project_root;
    std::filesystem::path canon = std::filesystem::weakly_canonical(abs_p, ec);
    std::filesystem::path norm = (!ec && !canon.empty()) ? canon : abs_p.lexically_normal();
    std::string s = norm.generic_u8string();
    while (s.size() > 1 && s.back() == '/') {
        // Keep "C:/" intact if Windows drive root
        if (s.size() == 3 && s[1] == ':') break;
        s.pop_back();
    }
#if defined(_WIN32)
    s = ToLowerStep(s);
#endif
    return s;
}

} // namespace

std::string StepPhaseToString(StepPhase phase) {
    return (phase == StepPhase::PostBuild) ? "postBuild" : "preBuild";
}

StepPhase StepPhaseFromString(const std::string& str) {
    std::string low = ToLowerStep(str);
    if (low == "postbuild" || low == "post_build" || low == "post") {
        return StepPhase::PostBuild;
    }
    return StepPhase::PreBuild;
}

std::string GetTargetGenDirRel(const std::string& config, const std::string& target_name) {
    return ".geniebuild/gen/" + SanitizeConfigDirName(config) + "/" + target_name;
}

bool IsStepGeneratedSourcePath(const std::string& path_str) {
    std::filesystem::path p(path_str);
    std::string ext = ToLowerStep(p.extension().generic_u8string());
    return IsCompilableSourceExtension(ext);
}

std::string ExpandProjectVariables(
    const std::string& text,
    const Project& project,
    const std::string& target_name,
    std::vector<std::string>* out_warnings
) {
    return ExpandDollarVariables(
        text,
        NormalizeStepPath(project.GetRootPath().generic_u8string()),
        project.GetConfiguration(),
        target_name,
        project.GetToolchain().empty() ? project.GetCompiler() : project.GetToolchain(),
        out_warnings
    );
}

std::string ExpandStepPlaceholders(
    const std::string& text,
    const StepExpandContext& ctx,
    bool quote_paths_with_spaces,
    std::vector<std::string>* out_warnings
) {
    std::string pass1 = ExpandDollarVariables(text, ctx.root, ctx.config, ctx.target, ctx.toolchain, out_warnings);

    std::string in_joined;
    for (size_t i = 0; i < ctx.inputs.size(); ++i) {
        if (i > 0) in_joined += ' ';
        in_joined += quote_paths_with_spaces ? QuoteIfNeeded(ctx.inputs[i]) : ctx.inputs[i];
    }

    std::string first_in = ctx.inputs.empty() ? "" : ctx.inputs[0];
    std::string first_out = ctx.outputs.empty() ? "" : ctx.outputs[0];

    std::string stem;
    std::string name;
    std::string dir;
    if (!first_in.empty()) {
        std::filesystem::path inp(first_in);
        stem = inp.stem().generic_u8string();
        name = inp.filename().generic_u8string();
        dir = NormalizeStepPath(inp.parent_path().generic_u8string());
        if (dir.empty()) dir = ".";
    }

    auto format_path_val = [&](const std::string& v) -> std::string {
        return quote_paths_with_spaces ? QuoteIfNeeded(v) : v;
    };

    std::string out;
    out.reserve(pass1.size() + 32);
    for (size_t i = 0; i < pass1.size(); ++i) {
        if (pass1[i] == '{') {
            size_t close = pass1.find('}', i + 1);
            if (close != std::string::npos) {
                std::string key = pass1.substr(i + 1, close - (i + 1));
                if (key == "in") {
                    out += in_joined;
                } else if (key == "out") {
                    out += format_path_val(first_out);
                } else if (key == "stem") {
                    out += stem;
                } else if (key == "name") {
                    out += name;
                } else if (key == "dir") {
                    out += format_path_val(dir);
                } else if (key == "objcopy") {
                    out += format_path_val(ctx.objcopyTool.empty() ? std::string("objcopy") : ctx.objcopyTool);
                } else if (key == "ld") {
                    out += format_path_val(ctx.ldTool.empty() ? std::string("ld") : ctx.ldTool);
                } else if (key == "gen") {
                    out += format_path_val(ctx.genDir);
                } else if (key == "bindir") {
                    out += format_path_val(ctx.binDir);
                } else if (key == "libdir") {
                    out += format_path_val(ctx.libDir);
                } else if (key == "objdir") {
                    out += format_path_val(ctx.objDir);
                } else if (key == "bin") {
                    out += format_path_val(ctx.binFile);
                } else if (key.rfind("bin:", 0) == 0) {
                    std::string tname = key.substr(4);
                    auto it = ctx.targetBinFiles.find(tname);
                    if (it != ctx.targetBinFiles.end()) {
                        out += format_path_val(it->second);
                    } else {
                        WarnOnce(out_warnings, "Unknown target '" + tname + "' in placeholder '{bin:" + tname + "}'.");
                    }
                } else if (key == "root") {
                    out += format_path_val(ctx.root);
                } else if (key == "config") {
                    out += ctx.config;
                } else if (key == "target") {
                    out += ctx.target;
                } else {
                    out += pass1.substr(i, close - i + 1);
                }
                i = close;
                continue;
            }
        }
        out.push_back(pass1[i]);
    }
    return out;
}

std::vector<std::string> ExtractStepBinTargetRefs(const BuildStep& step) {
    std::vector<std::string> refs;
    auto scan = [&](const std::string& s) {
        size_t pos = 0;
        while ((pos = s.find("{bin:", pos)) != std::string::npos) {
            size_t close = s.find('}', pos + 5);
            if (close == std::string::npos) break;
            std::string name = s.substr(pos + 5, close - (pos + 5));
            if (!name.empty() && std::find(refs.begin(), refs.end(), name) == refs.end()) {
                refs.push_back(name);
            }
            pos = close + 1;
        }
    };
    scan(step.command);
    scan(step.depfile);
    for (const auto& i : step.inputs) scan(i);
    for (const auto& o : step.outputs) scan(o);
    return refs;
}

BuildCommand BuildStepCommand(const std::string& expanded_cmd, bool shell) {
    BuildCommand cmd;
    cmd.display_command = expanded_cmd;
    if (shell) {
#if defined(_WIN32)
        cmd.program = "cmd";
        cmd.arguments = { "/c", expanded_cmd };
#else
        cmd.program = "sh";
        cmd.arguments = { "-c", expanded_cmd };
#endif
        return cmd;
    }

    std::vector<std::string> parts = CompilerManager::SplitFlags(expanded_cmd);
    if (!parts.empty()) {
        cmd.program = parts[0];
        if (parts.size() > 1) {
            cmd.arguments.assign(parts.begin() + 1, parts.end());
        }
    }
    return cmd;
}

StepExpandContext MakeStepExpandContext(
    const Project& project,
    const Target& target,
    const std::unordered_map<std::string, std::string>& target_bin_map,
    const ToolchainProfile* toolchain
) {
    StepExpandContext ctx;
    std::string config = project.GetConfiguration();
    if (config.empty()) config = "Debug";
    const std::string cfg_dir = SanitizeConfigDirName(config);
    bool is_msvc = false;
    if (toolchain) {
        is_msvc = toolchain->IsMsvcSyntax();
    } else {
        std::string comp_low = ToLowerStep(project.GetCompiler());
        is_msvc = (comp_low == "cl" || comp_low == "cl.exe" || comp_low.rfind("msvc", 0) == 0);
    }

    std::filesystem::path abs_out = project.GetAbsoluteOutputDir();
    std::filesystem::path root = project.GetRootPath();

    ctx.root = NormalizeStepPath(root.generic_u8string());
    ctx.config = config;
    ctx.target = target.name;
    ctx.toolchain = toolchain && !toolchain->name.empty()
        ? toolchain->name
        : (project.GetToolchain().empty() ? project.GetCompiler() : project.GetToolchain());
    ctx.toolchainHash8 = toolchain ? toolchain->idHash8 : "";
    ctx.objcopyTool = (toolchain && !toolchain->objcopy.empty()) ? toolchain->objcopy : "objcopy";
    ctx.ldTool = (toolchain && !toolchain->ld.empty()) ? toolchain->ld : "ld";
    ctx.genDir = GetTargetGenDirRel(config, target.name);
    ctx.binDir = MakeRelativeOrAbsPath(abs_out / cfg_dir / "bin", root);
    ctx.libDir = MakeRelativeOrAbsPath(abs_out / cfg_dir / "lib", root);
    std::string obj_cfg = cfg_dir + (!ctx.toolchainHash8.empty() ? ("-" + ctx.toolchainHash8) : "");
    ctx.objDir = ".geniebuild/obj/" + obj_cfg + "/" + target.name;

    std::filesystem::path bin_p = target.GetBinaryPath(abs_out, config, is_msvc);
    ctx.binFile = MakeRelativeOrAbsPath(bin_p, root);
    ctx.targetBinFiles = target_bin_map;
    return ctx;
}

std::vector<ExpandedStepJob> ExpandStepJobs(
    const Project& project,
    const Target& target,
    size_t step_index,
    const BuildStep& step,
    DirectoryCache& cache,
    const std::unordered_map<std::string, std::string>& target_bin_map,
    const std::vector<std::string>* virtual_outputs,
    std::vector<std::string>* out_warnings,
    const ToolchainProfile* toolchain
) {
    std::vector<ExpandedStepJob> jobs;
    StepExpandContext base_ctx = MakeStepExpandContext(project, target, target_bin_map, toolchain);
    std::vector<std::string> bin_refs = ExtractStepBinTargetRefs(step);

    auto expand_input_glob = [&](const std::string& raw_pat) -> std::vector<std::string> {
        std::string pat = NormalizeStepPath(ExpandStepPlaceholders(raw_pat, base_ctx, false, out_warnings));
        if (pat.empty()) return {};

        if (!ContainsGlobMeta(pat)) {
            return { pat };
        }

        std::vector<std::string> found = ExpandGlobFiles(project.GetRootPath(), pat, cache);
        if (virtual_outputs && !virtual_outputs->empty()) {
            CompiledGlob cg = CompiledGlob::Compile(pat);
            if (cg.IsValid()) {
                for (const auto& vo : *virtual_outputs) {
                    std::string nvo = NormalizeStepPath(vo);
                    if (cg.Matches(nvo) && std::find(found.begin(), found.end(), nvo) == found.end()) {
                        found.push_back(nvo);
                    }
                }
                std::sort(found.begin(), found.end());
            }
        }
        return found;
    };

    auto build_job = [&](size_t job_idx, const std::vector<std::string>& job_inputs, const std::string& label) {
        StepExpandContext jctx = base_ctx;
        jctx.inputs = job_inputs;

        for (const auto& out_tpl : step.outputs) {
            std::string expanded_out = NormalizeStepPath(ExpandStepPlaceholders(out_tpl, jctx, false, out_warnings));
            if (!expanded_out.empty()) {
                jctx.outputs.push_back(expanded_out);
            }
        }

        ExpandedStepJob job;
        job.targetName = target.name;
        job.stepName = step.name;
        job.stepIndex = step_index;
        job.jobIndex = job_idx;
        job.phase = step.phase;
        job.label = label;
        job.inputs = job_inputs;
        job.outputs = jctx.outputs;
        if (!step.depfile.empty()) {
            job.depfile = NormalizeStepPath(ExpandStepPlaceholders(step.depfile, jctx, false, out_warnings));
        }

        std::string expanded_cmd = ExpandStepPlaceholders(step.command, jctx, !step.shell, out_warnings);
        job.cmd = BuildStepCommand(expanded_cmd, step.shell);
        job.cmdHash = CompilerManager::ComputeCommandHash(job.cmd) + (step.shell ? "|shell" : "|direct");
        job.shell = step.shell;
        job.always = step.always;
        job.generatesSources = step.generatesSources;
        job.referencedTargets = bin_refs;

        std::string key_suffix = !job_inputs.empty() && !step.foreachPattern.empty()
            ? job_inputs[0]
            : ("job" + std::to_string(job_idx));
        job.stateKey = "step:" + std::to_string(step_index) + ":" + step.name + ":" + key_suffix;

        jobs.push_back(std::move(job));
    };

    if (!step.foreachPattern.empty()) {
        std::vector<std::string> matched = expand_input_glob(step.foreachPattern);
        for (size_t i = 0; i < matched.size(); ++i) {
            std::string lbl = std::filesystem::path(matched[i]).filename().generic_u8string();
            build_job(i, { matched[i] }, lbl);
        }
    } else {
        std::vector<std::string> all_inputs;
        for (const auto& inp_pat : step.inputs) {
            std::vector<std::string> expanded = expand_input_glob(inp_pat);
            for (const auto& item : expanded) {
                if (std::find(all_inputs.begin(), all_inputs.end(), item) == all_inputs.end()) {
                    all_inputs.push_back(item);
                }
            }
        }
        build_job(0, all_inputs, "");
    }

    return jobs;
}

std::vector<std::string> ParseStepDepfile(const std::filesystem::path& dep_path) {
    std::vector<std::string> deps;
    std::ifstream file(dep_path);
    if (!file.is_open()) return deps;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Find Makefile target separator ':' that is NOT a Windows drive letter ("C:/" or "C:\")
    size_t colon_pos = std::string::npos;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == ':') {
            bool is_drive = (i == 1 && std::isalpha(static_cast<unsigned char>(content[0])) &&
                             i + 1 < content.size() && (content[i + 1] == '/' || content[i + 1] == '\\'));
            bool is_mid_drive = (i >= 2 && std::isspace(static_cast<unsigned char>(content[i - 2])) &&
                                 std::isalpha(static_cast<unsigned char>(content[i - 1])) &&
                                 i + 1 < content.size() && (content[i + 1] == '/' || content[i + 1] == '\\'));
            if (!is_drive && !is_mid_drive) {
                colon_pos = i;
                break;
            }
        }
    }
    if (colon_pos == std::string::npos) return deps;

    std::string rest = content.substr(colon_pos + 1);
    std::string cur;
    bool escaped = false;

    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (escaped) {
            if (c != '\n' && c != '\r') {
                cur += c;
            }
            escaped = false;
        } else if (c == '\\') {
            // Only treat backslash as escape when followed by whitespace, newline, or '#' or backslash
            if (i + 1 < rest.size() && (rest[i + 1] == ' ' || rest[i + 1] == '\n' || rest[i + 1] == '\r' || rest[i + 1] == '#' || rest[i + 1] == '\\')) {
                escaped = true;
            } else {
                cur += c;
            }
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                deps.push_back(NormalizeStepPath(cur));
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        deps.push_back(NormalizeStepPath(cur));
    }
    return deps;
}

bool CheckStepJobNeedsRun(
    const Project& project,
    const ExpandedStepJob& job,
    const std::unordered_map<std::string, std::string>& prev_state,
    std::string& out_reason
) {
    if (job.always) {
        out_reason = "always: true";
        return true;
    }

    auto state_it = prev_state.find(job.stateKey);
    if (state_it == prev_state.end()) {
        out_reason = "no previous build state";
        return true;
    }
    if (state_it->second != job.cmdHash) {
        out_reason = "command hash changed";
        return true;
    }

    if (job.outputs.empty()) {
        // Without outputs, rerun if any input changed since we have no output timestamps
        // (unless no inputs, in which case command hash match keeps it skipped)
        if (!job.inputs.empty()) {
            out_reason = "step has inputs and no declared outputs";
            return true;
        }
        out_reason = "up to date";
        return false;
    }

    std::error_code ec;
    std::filesystem::file_time_type oldest_out = std::filesystem::file_time_type::max();
    for (const auto& out_rel : job.outputs) {
        std::filesystem::path out_p = project.ResolvePath(out_rel);
        if (!std::filesystem::exists(out_p, ec) || ec) {
            out_reason = "output missing: " + out_rel;
            return true;
        }
        auto t = std::filesystem::last_write_time(out_p, ec);
        if (ec) {
            out_reason = "output timestamp unreadable: " + out_rel;
            return true;
        }
        if (t < oldest_out) oldest_out = t;
    }

    for (const auto& in_rel : job.inputs) {
        std::filesystem::path in_p = project.ResolvePath(in_rel);
        if (!std::filesystem::exists(in_p, ec) || ec) {
            out_reason = "input missing or generated: " + in_rel;
            return true;
        }
        auto t = std::filesystem::last_write_time(in_p, ec);
        if (ec || t > oldest_out) {
            out_reason = "input newer: " + in_rel;
            return true;
        }
    }

    if (!job.depfile.empty()) {
        std::filesystem::path dp = project.ResolvePath(job.depfile);
        if (std::filesystem::exists(dp, ec) && !ec) {
            std::vector<std::string> deps = ParseStepDepfile(dp);
            for (const auto& dep_str : deps) {
                std::filesystem::path dep_p = project.ResolvePath(dep_str);
                if (std::filesystem::exists(dep_p, ec) && !ec) {
                    auto t = std::filesystem::last_write_time(dep_p, ec);
                    if (!ec && t > oldest_out) {
                        out_reason = "depfile dependency newer: " + dep_str;
                        return true;
                    }
                }
            }
        }
    }

    out_reason = "up to date";
    return false;
}

bool IsStepProgramAvailable(
    const std::string& program,
    const std::filesystem::path& project_root,
    const std::vector<std::string>& planned_outputs,
    std::string& out_resolved
) {
    out_resolved.clear();
    if (program.empty()) return false;

    std::string norm_prog = NormalizeStepPath(program);
    for (const auto& po : planned_outputs) {
        std::string npo = NormalizeStepPath(po);
        if (npo == norm_prog) {
            out_resolved = po;
            return true;
        }
    }

    std::error_code ec;
    auto check_file = [&](const std::filesystem::path& cand) -> bool {
        if (std::filesystem::exists(cand, ec) && !ec && std::filesystem::is_regular_file(cand, ec)) {
            out_resolved = cand.generic_u8string();
            return true;
        }
        return false;
    };

    // If program has directory separators or is absolute
    if (program.find('/') != std::string::npos || program.find('\\') != std::string::npos) {
        std::filesystem::path p = std::filesystem::u8path(program);
        std::filesystem::path resolved = p.is_absolute() ? p : (project_root / p);
        if (check_file(resolved)) return true;
#if defined(_WIN32)
        if (!resolved.has_extension()) {
            for (const char* ext : { ".exe", ".cmd", ".bat", ".com" }) {
                if (check_file(std::filesystem::path(resolved.generic_u8string() + ext))) return true;
            }
        }
#endif
        return false;
    }

    // Search PATH
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;

#if defined(_WIN32)
    const char delim = ';';
#else
    const char delim = ':';
#endif
    std::stringstream ss(path_env);
    std::string dir_str;
    while (std::getline(ss, dir_str, delim)) {
        if (dir_str.empty()) continue;
        std::filesystem::path dir(dir_str);
        if (check_file(dir / program)) return true;
#if defined(_WIN32)
        std::filesystem::path p(program);
        if (!p.has_extension()) {
            for (const char* ext : { ".exe", ".cmd", ".bat", ".com" }) {
                if (check_file(dir / (program + ext))) return true;
            }
        }
#endif
    }
    return false;
}

std::string PreviewStepCommand(
    const Project& project,
    const Target& target,
    const BuildStep& step,
    DirectoryCache& cache,
    std::vector<std::string>* out_outputs
) {
    if (out_outputs) out_outputs->clear();
    std::unordered_map<std::string, std::string> bin_map;
    std::string comp_low = ToLowerStep(project.GetCompiler());
    bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
    std::filesystem::path abs_out = project.GetAbsoluteOutputDir();
    std::string cfg = project.GetConfiguration();
    for (const auto& t : project.GetTargets()) {
        std::filesystem::path bp = t.GetBinaryPath(abs_out, cfg, is_msvc);
        if (!bp.empty()) {
            bin_map[t.name] = MakeRelativeOrAbsPath(bp, project.GetRootPath());
        }
    }

    auto jobs = ExpandStepJobs(project, target, 0, step, cache, bin_map, nullptr, nullptr);
    if (!jobs.empty()) {
        if (out_outputs) *out_outputs = jobs[0].outputs;
        return jobs[0].cmd.display_command;
    }

    // Fallback preview with placeholder sample if foreach matched 0 files on disk
    StepExpandContext ctx = MakeStepExpandContext(project, target, bin_map);
    if (!step.foreachPattern.empty()) {
        ctx.inputs = { "<no files match '" + step.foreachPattern + "'>" };
    }
    for (const auto& ot : step.outputs) {
        std::string eo = NormalizeStepPath(ExpandStepPlaceholders(ot, ctx, false, nullptr));
        ctx.outputs.push_back(eo);
        if (out_outputs) out_outputs->push_back(eo);
    }
    return ExpandStepPlaceholders(step.command, ctx, !step.shell, nullptr);
}

std::vector<StepDryRunEntry> DryRunTargetSteps(
    const Project& project,
    const Target& target,
    const std::vector<Target>& all_targets,
    DirectoryCache& cache
) {
    std::vector<StepDryRunEntry> entries;
    ConditionContext cond_ctx = BuildConditionContext(project);

    std::string comp_low = ToLowerStep(project.GetCompiler());
    bool is_msvc = (comp_low == "cl" || comp_low == "cl.exe");
    std::filesystem::path abs_out = project.GetAbsoluteOutputDir();
    std::string cfg = project.GetConfiguration();

    std::unordered_map<std::string, std::string> bin_map;
    for (const auto& t : all_targets) {
        std::filesystem::path bp = t.GetBinaryPath(abs_out, cfg, is_msvc);
        if (!bp.empty()) {
            bin_map[t.name] = MakeRelativeOrAbsPath(bp, project.GetRootPath());
        }
    }

    // Load prev_state from .geniebuild/obj/<Config>/<target>/state.json
    std::unordered_map<std::string, std::string> prev_state;
    std::filesystem::path state_path = target.GetObjectDir(project.GetRootPath(), cfg) / "state.json";
    std::ifstream sf(state_path);
    if (sf.is_open()) {
        try {
            nlohmann::json j;
            sf >> j;
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if (it.value().is_string()) {
                        prev_state[it.key()] = it.value().get<std::string>();
                    }
                }
            }
        } catch (...) {}
    }

    std::vector<std::string> virtual_outputs;
    for (size_t si = 0; si < target.steps.size(); ++si) {
        const auto& step = target.steps[si];
        ConditionEvalResult cer = EvaluateCondition(step.condition, cond_ctx);
        if (!cer.valid || !cer.active) {
            StepDryRunEntry e;
            e.targetName = target.name;
            e.stepName = step.name;
            e.phase = step.phase;
            e.active = false;
            e.wouldRun = false;
            e.reason = cer.reason;
            entries.push_back(std::move(e));
            continue;
        }

        auto jobs = ExpandStepJobs(project, target, si, step, cache, bin_map, &virtual_outputs, nullptr);
        if (jobs.empty()) {
            StepDryRunEntry e;
            e.targetName = target.name;
            e.stepName = step.name;
            e.phase = step.phase;
            e.active = true;
            e.wouldRun = false;
            e.reason = step.foreachPattern.empty() ? "no jobs" : ("no files match '" + step.foreachPattern + "'");
            entries.push_back(std::move(e));
            continue;
        }

        for (const auto& job : jobs) {
            StepDryRunEntry e;
            e.targetName = target.name;
            e.stepName = step.name;
            e.label = job.label;
            e.phase = step.phase;
            e.active = true;
            e.expandedCommand = job.cmd.display_command;
            e.outputs = job.outputs;
            e.wouldRun = CheckStepJobNeedsRun(project, job, prev_state, e.reason);
            entries.push_back(std::move(e));
            for (const auto& o : job.outputs) {
                virtual_outputs.push_back(o);
            }
        }
    }
    return entries;
}

// -----------------------------------------------------------------------------
// TrustStore
// -----------------------------------------------------------------------------

std::filesystem::path TrustStore::GetTrustFilePath() {
    std::filesystem::path p;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        p = std::filesystem::path(appdata) / "GenieBuild" / "trust.json";
    }
#endif
    if (p.empty()) {
        const char* home = std::getenv("USERPROFILE");
        if (!home) home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            p = std::filesystem::path(home) / ".geniebuild" / "trust.json";
        } else {
            p = std::filesystem::temp_directory_path() / "GenieBuild" / "trust.json";
        }
    }
    return p;
}

bool TrustStore::IsTrusted(const std::filesystem::path& project_root) {
    if (project_root.empty()) return true;
    std::string key = NormalizeTrustKey(project_root);
    std::filesystem::path tf = GetTrustFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(tf, ec) || ec) return false;
    try {
        std::ifstream f(tf);
        if (!f.is_open()) return false;
        nlohmann::json j;
        f >> j;
        if (j.is_object() && j.contains("trustedPaths") && j["trustedPaths"].is_array()) {
            for (const auto& item : j["trustedPaths"]) {
                if (item.is_string() && NormalizeTrustKey(std::filesystem::u8path(item.get<std::string>())) == key) {
                    return true;
                }
            }
        }
    } catch (...) {}
    return false;
}

void TrustStore::SetTrusted(const std::filesystem::path& project_root, bool trusted) {
    if (project_root.empty()) return;
    std::string key = NormalizeTrustKey(project_root);
    std::filesystem::path tf = GetTrustFilePath();

    std::vector<std::string> paths;
    std::error_code ec;
    if (std::filesystem::exists(tf, ec) && !ec) {
        try {
            std::ifstream f(tf);
            if (f.is_open()) {
                nlohmann::json j;
                f >> j;
                if (j.is_object() && j.contains("trustedPaths") && j["trustedPaths"].is_array()) {
                    for (const auto& item : j["trustedPaths"]) {
                        if (item.is_string()) {
                            std::string s = item.get<std::string>();
                            if (NormalizeTrustKey(std::filesystem::u8path(s)) != key) {
                                paths.push_back(s);
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }

    if (trusted) {
        paths.push_back(key);
    }

    try {
        std::filesystem::create_directories(tf.parent_path(), ec);
        nlohmann::json j = nlohmann::json::object();
        j["trustedPaths"] = paths;
        std::ofstream out(tf);
        if (out.is_open()) {
            out << j.dump(2) << std::endl;
        }
    } catch (...) {}
}

size_t TrustStore::CountCustomCommands(const Project& project) {
    size_t count = 0;
    for (const auto& t : project.GetTargets()) {
        count += t.steps.size();
        if (!t.run.command.empty()) count++;
    }
    return count;
}

std::vector<StepPreset> GetBuiltInStepPresets() {
    std::vector<StepPreset> presets;

    {
        StepPreset p;
        p.id = "elf_to_bin";
        p.label = "ELF -> raw binary";
        p.requiredTool = "objcopy";
        p.missingHint = "Requires objcopy from GNU binutils or your cross toolchain.";
        p.step.name = "raw binary";
        p.step.phase = StepPhase::PostBuild;
        p.step.inputs = { "{bin}" };
        p.step.outputs = { "{bindir}/{target}.bin" };
        p.step.command = "{objcopy} -O binary {bin} {bindir}/{target}.bin";
        presets.push_back(std::move(p));
    }

    {
        StepPreset p;
        p.id = "grub_iso";
        p.label = "GRUB ISO (os.iso)";
        p.requiredTool = "grub-mkrescue";
        p.missingHint = "On Windows, install grub-mkrescue + xorriso via MSYS2 or WSL.";
        p.step.name = "iso";
        p.step.phase = StepPhase::PostBuild;
        p.step.inputs = { "{bin}", "iso/**" };
        p.step.outputs = { "{bindir}/os.iso" };
        p.step.shell = true;
#if defined(_WIN32)
        p.step.command = "if not exist \"{gen}\\isodir\\boot\\grub\" mkdir \"{gen}\\isodir\\boot\\grub\" && copy /Y \"{bin}\" \"{gen}\\isodir\\boot\\{target}.elf\" >nul && copy /Y \"iso\\boot\\grub\\grub.cfg\" \"{gen}\\isodir\\boot\\grub\\grub.cfg\" >nul && grub-mkrescue -o \"{bindir}/os.iso\" \"{gen}/isodir\"";
#else
        p.step.command = "mkdir -p {gen}/isodir/boot/grub && cp {bin} {gen}/isodir/boot/{target}.elf && cp iso/boot/grub/grub.cfg {gen}/isodir/boot/grub/grub.cfg && grub-mkrescue -o {bindir}/os.iso {gen}/isodir";
#endif
        presets.push_back(std::move(p));
    }

    {
        StepPreset p;
        p.id = "limine_iso";
        p.label = "Limine ISO (os.iso)";
        p.requiredTool = "xorriso";
        p.missingHint = "Install xorriso + limine CLI tools (MSYS2 or WSL on Windows).";
        p.step.name = "limine-iso";
        p.step.phase = StepPhase::PostBuild;
        p.step.inputs = { "{bin}", "limine.conf" };
        p.step.outputs = { "{bindir}/os.iso" };
        p.step.shell = true;
#if defined(_WIN32)
        p.step.command = "if not exist \"{gen}\\isodir\\boot\" mkdir \"{gen}\\isodir\\boot\" && copy /Y \"{bin}\" \"{gen}\\isodir\\boot\\kernel.elf\" >nul && copy /Y \"limine.conf\" \"{gen}\\isodir\\boot\\limine.conf\" >nul && xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \"{gen}/isodir\" -o \"{bindir}/os.iso\" && limine bios-install \"{bindir}/os.iso\"";
#else
        p.step.command = "mkdir -p {gen}/isodir/boot && cp {bin} {gen}/isodir/boot/kernel.elf && cp limine.conf {gen}/isodir/boot/limine.conf && xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table {gen}/isodir -o {bindir}/os.iso && limine bios-install {bindir}/os.iso";
#endif
        presets.push_back(std::move(p));
    }

    {
        StepPreset p;
        p.id = "copy_assets";
        p.label = "Copy assets folder";
        p.requiredTool = "";
        p.step.name = "copy-assets";
        p.step.phase = StepPhase::PostBuild;
        p.step.inputs = { "assets/**" };
        p.step.outputs = { "{bindir}/assets/.copied" };
        p.step.shell = true;
#if defined(_WIN32)
        p.step.command = "if not exist \"{bindir}\\assets\" mkdir \"{bindir}\\assets\" && xcopy /E /I /Y \"assets\" \"{bindir}\\assets\" >nul && type nul > \"{bindir}\\assets\\.copied\"";
#else
        p.step.command = "mkdir -p {bindir}/assets && cp -r assets/. {bindir}/assets/ && touch {bindir}/assets/.copied";
#endif
        presets.push_back(std::move(p));
    }

    return presets;
}

BuildCommand FormulateRunCommand(
    const Project& project,
    const Target& target,
    const RunConfig& run_cfg,
    const ToolchainProfile* toolchain,
    std::filesystem::path& out_cwd,
    std::unordered_map<std::string, std::string>& out_env
) {
    BuildCommand cmd;
    std::unordered_map<std::string, std::string> bin_map;
    bool is_msvc = toolchain ? toolchain->IsMsvcSyntax() : false;
    std::filesystem::path abs_out = project.GetAbsoluteOutputDir();
    std::string config = project.GetConfiguration();
    for (const auto& t : project.GetTargets()) {
        if (t.kind == TargetKind::Interface) continue;
        std::filesystem::path bp = t.GetBinaryPath(abs_out, config, is_msvc);
        if (!bp.empty()) {
            bin_map[t.name] = MakeRelativeOrAbsPath(bp, project.GetRootPath());
        }
    }

    StepExpandContext ctx = MakeStepExpandContext(project, target, bin_map, toolchain);
    std::string raw_prog = run_cfg.command.empty() ? "{bin}" : run_cfg.command;
    std::string exp_prog = ExpandStepPlaceholders(raw_prog, ctx, false, nullptr);
    // Resolve relative executable paths inside project root cleanly
    std::filesystem::path prog_p = std::filesystem::u8path(exp_prog);
    if (exp_prog.find('/') != std::string::npos || exp_prog.find('\\') != std::string::npos) {
        std::filesystem::path res_p = project.ResolvePath(exp_prog);
        std::error_code ec;
        if (std::filesystem::exists(res_p, ec)) {
            exp_prog = res_p.generic_u8string();
        }
    } else if (prog_p.is_relative() && !ctx.binFile.empty() && raw_prog == "{bin}") {
        exp_prog = project.ResolvePath(ctx.binFile).generic_u8string();
    }

    cmd.program = exp_prog;
    std::ostringstream ss;
    ss << QuoteIfNeeded(cmd.program);
    for (const auto& a : run_cfg.args) {
        std::string ea = ExpandStepPlaceholders(a, ctx, false, nullptr);
        cmd.arguments.push_back(ea);
        ss << " " << QuoteIfNeeded(ea);
    }
    cmd.display_command = ss.str();

    std::string raw_cwd = run_cfg.cwd.empty() ? "{bindir}" : run_cfg.cwd;
    std::string exp_cwd = ExpandStepPlaceholders(raw_cwd, ctx, false, nullptr);
    out_cwd = project.ResolvePath(exp_cwd);

    std::map<std::string, std::string> combined_env;
    if (toolchain) {
        for (const auto& [k, v] : toolchain->env) combined_env[k] = v;
    }
    for (const auto& [k, v] : run_cfg.env) {
        combined_env[k] = ExpandStepPlaceholders(v, ctx, false, nullptr);
    }
    out_env = MsvcEnvDetector::BuildMergedProcessEnv(combined_env);
    return cmd;
}

} // namespace core
