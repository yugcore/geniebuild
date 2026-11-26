#include "tool_registry.h"
#include "project.h"
#include "target.h"
#include "build_steps.h"

#include <reproc++/reproc.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace core {

namespace {

std::string ToLowerTool(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void AppendToolArg(BuildCommand& cmd, std::ostringstream& ss, const std::string& arg) {
    cmd.arguments.push_back(arg);
    if (arg.find(' ') != std::string::npos && !(arg.front() == '"' && arg.back() == '"')) {
        ss << " \"" << arg << "\"";
    } else {
        ss << " " << arg;
    }
}

void HashBytesFnv1a(uint64_t& h, const std::string& data) {
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= 0xFF;
    h *= 1099511628211ULL;
}

std::string QueryToolFirstLine(const std::string& exe_path, const std::vector<std::string>& args) {
    if (exe_path.empty()) return "";
    reproc::process proc;
    reproc::options opts;
    opts.redirect.out.type = reproc::redirect::pipe;
    opts.redirect.err.type = reproc::redirect::stdout_;
    opts.deadline = reproc::milliseconds(1500);

    std::vector<std::string> argv;
    argv.push_back(exe_path);
    for (const auto& a : args) argv.push_back(a);

    if (proc.start(argv, opts)) return "";
    std::string out;
    uint8_t buf[1024];
    while (true) {
        auto [n, ec] = proc.read(reproc::stream::out, buf, sizeof(buf));
        if (ec || n == 0) break;
        out.append(reinterpret_cast<const char*>(buf), n);
        if (out.size() > 512) break;
    }
    proc.wait(reproc::milliseconds(500));
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) return line;
    }
    return "";
}

std::string FindOnPathTool(const std::string& prog_name) {
    if (prog_name.empty()) return "";
    std::error_code ec;
    std::filesystem::path direct(prog_name);
    if (direct.is_absolute() || prog_name.find('/') != std::string::npos || prog_name.find('\\') != std::string::npos) {
        if (std::filesystem::exists(direct, ec) && std::filesystem::is_regular_file(direct, ec)) {
            return direct.generic_u8string();
        }
#if defined(_WIN32)
        if (!direct.has_extension()) {
            std::filesystem::path exe_p(direct.generic_u8string() + ".exe");
            if (std::filesystem::exists(exe_p, ec) && std::filesystem::is_regular_file(exe_p, ec)) {
                return exe_p.generic_u8string();
            }
        }
#endif
        return "";
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";
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
#if defined(_WIN32)
        for (const char* ext : { ".exe", ".cmd", ".bat", "" }) {
            std::filesystem::path cand = dir / (prog_name + ext);
            if (std::filesystem::exists(cand, ec) && std::filesystem::is_regular_file(cand, ec)) {
                return cand.generic_u8string();
            }
        }
#else
        std::filesystem::path cand = dir / prog_name;
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_regular_file(cand, ec)) {
            return cand.generic_u8string();
        }
#endif
    }
    return "";
}

std::string SanitizeObjKey(const std::string& rel_path) {
    std::string s = rel_path;
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

} // namespace

SourceFileKind ToolRegistry::ClassifySourceFile(
    const std::string& path_str,
    const std::vector<CustomToolSpec>& custom_tools
) {
    std::filesystem::path p(path_str);
    std::string raw_ext = p.extension().generic_u8string();
    std::string low_ext = ToLowerTool(raw_ext);

    if (low_ext == ".c") return SourceFileKind::C;
    if (low_ext == ".cpp" || low_ext == ".cc" || low_ext == ".cxx") return SourceFileKind::Cxx;
    if (low_ext == ".rc") return SourceFileKind::Rc;
    if (raw_ext == ".S") return SourceFileKind::AsmPre;
    if (low_ext == ".s") return SourceFileKind::AsmRaw;
    if (low_ext == ".asm") return SourceFileKind::AsmNasm;
    if (low_ext == ".cu") return SourceFileKind::Cuda;
    if (low_ext == ".ispc") return SourceFileKind::Ispc;

    for (const auto& ct : custom_tools) {
        if (!ct.ext.empty() && ToLowerTool(ct.ext) == low_ext) {
            return SourceFileKind::Custom;
        }
    }
    return SourceFileKind::Unknown;
}

bool ToolRegistry::IsAnyCompilableSource(
    const std::string& path_str,
    const std::vector<CustomToolSpec>& custom_tools
) {
    return ClassifySourceFile(path_str, custom_tools) != SourceFileKind::Unknown;
}

std::string ToolRegistry::ResolveToolExecutable(
    const std::string& tool_id,
    const std::map<std::string, std::string>& tool_overrides
) {
    auto it = tool_overrides.find(tool_id);
    if (it != tool_overrides.end() && !it->second.empty()) {
        std::string found = FindOnPathTool(it->second);
        if (!found.empty()) return found;
        return "";
    }
    return FindOnPathTool(tool_id);
}

std::vector<DetectedToolInfo> ToolRegistry::DetectTools(
    const std::map<std::string, std::string>& tool_overrides,
    bool query_versions
) {
    struct Spec {
        const char* id;
        const char* ver_arg;
    };
    static const Spec kTools[] = {
        { "windres", "--version" },
        { "rc", "/?" },
        { "nasm", "-v" },
        { "ml64", "" },
        { "nvcc", "--version" },
        { "ispc", "--version" }
    };

    std::vector<DetectedToolInfo> out;
    for (const auto& t : kTools) {
        DetectedToolInfo info;
        info.id = t.id;
        info.path = ResolveToolExecutable(t.id, tool_overrides);
        info.found = !info.path.empty();
        if (info.found && query_versions) {
            if (t.ver_arg[0] != '\0') {
                info.version = QueryToolFirstLine(info.path, { t.ver_arg });
            } else {
                info.version = QueryToolFirstLine(info.path, {});
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

ToolCompileSpec ToolRegistry::FormulateExtraSourceCommand(
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
    const std::string& asm_mode
) {
    ToolCompileSpec spec;
    SourceFileKind kind = ClassifySourceFile(source_rel_path, custom_tools);
    std::string src_fwd = SanitizeObjKey(source_rel_path);
    std::filesystem::path src_p = std::filesystem::u8path(src_fwd);
    std::string stem = src_p.stem().generic_u8string();
    std::string raw_ext = src_p.extension().generic_u8string();
    std::string low_ext = ToLowerTool(raw_ext);

    std::string safe_rel = src_fwd;
    std::replace(safe_rel.begin(), safe_rel.end(), '/', '_');
    std::replace(safe_rel.begin(), safe_rel.end(), ':', '_');

    std::ostringstream ss;

    if (kind == SourceFileKind::Rc) {
        spec.usesRcHash = true;
        spec.linkOutput = true;
        if (is_msvc) {
            std::string rc_exe = ResolveToolExecutable("rc", tool_overrides);
            if (rc_exe.empty()) {
                spec.supported = false;
                spec.requiredTool = "rc";
                spec.errorMessage = source_rel_path + " needs rc, which was not found. Install Windows SDK or set it in Settings -> Tools.";
                return spec;
            }
            spec.outputPath = obj_dir / (safe_rel + ".res");
            std::string out_s = spec.outputPath.generic_u8string();
            spec.command.program = rc_exe;
            ss << rc_exe;
            AppendToolArg(spec.command, ss, "/nologo");
            AppendToolArg(spec.command, ss, "/fo");
            AppendToolArg(spec.command, ss, out_s);
            AppendToolArg(spec.command, ss, "/I.");
            AppendToolArg(spec.command, ss, "/Iinclude");
            for (const auto& inc : eff.includeDirs) AppendToolArg(spec.command, ss, "/I" + inc);
            for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "/D" + def);
            AppendToolArg(spec.command, ss, src_fwd);
            spec.command.display_command = ss.str();
        } else {
            std::string windres_exe = ResolveToolExecutable("windres", tool_overrides);
            if (windres_exe.empty()) {
                spec.supported = false;
                spec.requiredTool = "windres";
                spec.errorMessage = source_rel_path + " needs windres, which was not found. Install MinGW binutils or set it in Settings -> Tools.";
                return spec;
            }
            spec.outputPath = obj_dir / (safe_rel + ".o");
            std::string out_s = spec.outputPath.generic_u8string();
            spec.command.program = windres_exe;
            ss << windres_exe;
            AppendToolArg(spec.command, ss, src_fwd);
            AppendToolArg(spec.command, ss, "-O");
            AppendToolArg(spec.command, ss, "coff");
            AppendToolArg(spec.command, ss, "-o");
            AppendToolArg(spec.command, ss, out_s);
            AppendToolArg(spec.command, ss, "-I.");
            AppendToolArg(spec.command, ss, "-Iinclude");
            for (const auto& inc : eff.includeDirs) AppendToolArg(spec.command, ss, "-I" + inc);
            for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "-D" + def);
            spec.command.display_command = ss.str();
        }
        return spec;
    }

    if (kind == SourceFileKind::AsmPre || kind == SourceFileKind::AsmRaw) {
        if (is_msvc) {
            spec.supported = false;
            spec.errorMessage = source_rel_path + " (" + raw_ext + ") requires a GCC/Clang driver (ml64 cannot preprocess .S; use .asm or gcc/clang).";
            return spec;
        }
        std::string drv = project.GetCCompiler();
        if (drv.empty()) drv = compiler_name;
        spec.outputPath = obj_dir / (safe_rel + ".o");
        spec.usesDepfile = (kind == SourceFileKind::AsmPre);
        if (spec.usesDepfile) {
            spec.depfilePath = std::filesystem::path(spec.outputPath.generic_u8string() + ".d");
        }
        spec.command.program = drv;
        ss << drv;
        AppendToolArg(spec.command, ss, "-c");
        AppendToolArg(spec.command, ss, src_fwd);
        AppendToolArg(spec.command, ss, "-o");
        AppendToolArg(spec.command, ss, spec.outputPath.generic_u8string());
        if (spec.usesDepfile) {
            AppendToolArg(spec.command, ss, "-MMD");
            AppendToolArg(spec.command, ss, "-MF");
            AppendToolArg(spec.command, ss, spec.depfilePath.generic_u8string());
            AppendToolArg(spec.command, ss, "-I.");
            AppendToolArg(spec.command, ss, "-Iinclude");
            for (const auto& inc : eff.includeDirs) AppendToolArg(spec.command, ss, "-I" + inc);
            for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "-D" + def);
        }
        spec.command.display_command = ss.str();
        return spec;
    }

    if (kind == SourceFileKind::AsmNasm) {
        bool use_ml64 = is_msvc && (ToLowerTool(asm_mode) != "nasm");
        if (use_ml64) {
            std::string ml64_exe = ResolveToolExecutable("ml64", tool_overrides);
            if (ml64_exe.empty()) {
                spec.supported = false;
                spec.requiredTool = "ml64";
                spec.errorMessage = source_rel_path + " needs ml64, which was not found. Install MSVC build tools or set asm to nasm in Settings -> Tools.";
                return spec;
            }
            spec.outputPath = obj_dir / (safe_rel + ".obj");
            spec.command.program = ml64_exe;
            ss << ml64_exe;
            AppendToolArg(spec.command, ss, "/nologo");
            AppendToolArg(spec.command, ss, "/c");
            AppendToolArg(spec.command, ss, "/Fo" + spec.outputPath.generic_u8string());
            for (const auto& inc : eff.includeDirs) AppendToolArg(spec.command, ss, "/I" + inc);
            for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "/D" + def);
            AppendToolArg(spec.command, ss, src_fwd);
            spec.command.display_command = ss.str();
        } else {
            std::string nasm_exe = ResolveToolExecutable("nasm", tool_overrides);
            if (nasm_exe.empty()) {
                spec.supported = false;
                spec.requiredTool = "nasm";
                spec.errorMessage = source_rel_path + " needs nasm, which was not found. Install it or set it in Settings -> Tools.";
                return spec;
            }
            spec.outputPath = obj_dir / (safe_rel + (is_msvc ? ".obj" : ".o"));
            spec.usesDepfile = true;
            spec.depfilePath = std::filesystem::path(spec.outputPath.generic_u8string() + ".d");
            spec.command.program = nasm_exe;
            ss << nasm_exe;
            AppendToolArg(spec.command, ss, "-f");
#if defined(_WIN32)
            AppendToolArg(spec.command, ss, "win64");
#else
            AppendToolArg(spec.command, ss, "elf64");
#endif
            AppendToolArg(spec.command, ss, "-MD");
            AppendToolArg(spec.command, ss, spec.depfilePath.generic_u8string());
            for (const auto& inc : eff.includeDirs) {
                std::string inc_slash = inc;
                std::replace(inc_slash.begin(), inc_slash.end(), '\\', '/');
                if (!inc_slash.empty() && inc_slash.back() != '/') inc_slash += '/';
                AppendToolArg(spec.command, ss, "-I" + inc_slash);
            }
            for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "-D" + def);
            AppendToolArg(spec.command, ss, src_fwd);
            AppendToolArg(spec.command, ss, "-o");
            AppendToolArg(spec.command, ss, spec.outputPath.generic_u8string());
            spec.command.display_command = ss.str();
        }
        return spec;
    }

    if (kind == SourceFileKind::Cuda) {
        std::string nvcc_exe = ResolveToolExecutable("nvcc", tool_overrides);
        if (nvcc_exe.empty()) {
            spec.supported = false;
            spec.requiredTool = "nvcc";
            spec.errorMessage = source_rel_path + " needs nvcc, which was not found. Install CUDA Toolkit or set it in Settings -> Tools.";
            return spec;
        }
        spec.outputPath = obj_dir / (safe_rel + (is_msvc ? ".obj" : ".o"));
        spec.usesDepfile = true;
        spec.depfilePath = std::filesystem::path(spec.outputPath.generic_u8string() + ".d");
        spec.command.program = nvcc_exe;
        ss << nvcc_exe;
        AppendToolArg(spec.command, ss, "-c");
        AppendToolArg(spec.command, ss, src_fwd);
        AppendToolArg(spec.command, ss, "-o");
        AppendToolArg(spec.command, ss, spec.outputPath.generic_u8string());
        AppendToolArg(spec.command, ss, "-MD");
        AppendToolArg(spec.command, ss, "-MF");
        AppendToolArg(spec.command, ss, spec.depfilePath.generic_u8string());
        if (!compiler_name.empty()) {
            AppendToolArg(spec.command, ss, "-ccbin");
            AppendToolArg(spec.command, ss, compiler_name);
        }
        if (!eff.cxxStd.empty()) {
            AppendToolArg(spec.command, ss, "-std=" + eff.cxxStd);
        }
        AppendToolArg(spec.command, ss, "-I.");
        AppendToolArg(spec.command, ss, "-Iinclude");
        for (const auto& inc : eff.includeDirs) AppendToolArg(spec.command, ss, "-I" + inc);
        for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "-D" + def);
        spec.command.display_command = ss.str();
        return spec;
    }

    if (kind == SourceFileKind::Ispc) {
        std::string ispc_exe = ResolveToolExecutable("ispc", tool_overrides);
        if (ispc_exe.empty()) {
            spec.supported = false;
            spec.requiredTool = "ispc";
            spec.errorMessage = source_rel_path + " needs ispc, which was not found. Install ISPC or set it in Settings -> Tools.";
            return spec;
        }
        spec.outputPath = obj_dir / (safe_rel + (is_msvc ? ".obj" : ".o"));
        spec.usesDepfile = true;
        spec.depfilePath = std::filesystem::path(spec.outputPath.generic_u8string() + ".d");
        spec.generatedHeaderPath = gen_dir / (stem + "_ispc.h");
        spec.command.program = ispc_exe;
        ss << ispc_exe;
        AppendToolArg(spec.command, ss, src_fwd);
        AppendToolArg(spec.command, ss, "-o");
        AppendToolArg(spec.command, ss, spec.outputPath.generic_u8string());
        AppendToolArg(spec.command, ss, "-h");
        AppendToolArg(spec.command, ss, spec.generatedHeaderPath.generic_u8string());
        AppendToolArg(spec.command, ss, "-M");
        AppendToolArg(spec.command, ss, "-MF");
        AppendToolArg(spec.command, ss, spec.depfilePath.generic_u8string());
        for (const auto& inc : eff.includeDirs) AppendToolArg(spec.command, ss, "-I" + inc);
        for (const auto& def : eff.defines) AppendToolArg(spec.command, ss, "-D" + def);
        spec.command.display_command = ss.str();
        return spec;
    }

    if (kind == SourceFileKind::Custom) {
        for (const auto& ct : custom_tools) {
            if (ToLowerTool(ct.ext) == low_ext) {
                std::string resolved_prog = FindOnPathTool(ct.program);
                if (resolved_prog.empty()) {
                    spec.supported = false;
                    spec.requiredTool = ct.program;
                    spec.errorMessage = source_rel_path + " needs " + ct.program + ", which was not found. Install it or set it in Settings -> Tools.";
                    return spec;
                }
                StepExpandContext ctx = MakeStepExpandContext(project, target, {});
                ctx.inputs = { src_fwd };
                std::string out_tpl = ct.output.empty() ? "{objdir}/{name}.o" : ct.output;
                std::string out_str = ExpandStepPlaceholders(out_tpl, ctx, false, nullptr);
                spec.outputPath = project.ResolvePath(out_str);
                ctx.outputs = { spec.outputPath.generic_u8string() };
                spec.linkOutput = ct.link;

                std::string expanded_args = ExpandStepPlaceholders(ct.args, ctx, true, nullptr);
                spec.command.program = resolved_prog;
                ss << resolved_prog;
                for (const auto& tok : CompilerManager::SplitFlags(expanded_args)) {
                    AppendToolArg(spec.command, ss, tok);
                }
                spec.command.display_command = ss.str();
                return spec;
            }
        }
    }

    spec.supported = false;
    spec.errorMessage = "Unsupported source file type: " + source_rel_path;
    return spec;
}

std::vector<std::string> ToolRegistry::ScanRcDependencies(
    const std::filesystem::path& rc_full_path,
    const std::filesystem::path& project_root,
    const std::vector<std::string>& include_dirs
) {
    std::vector<std::string> deps;
    std::unordered_set<std::string> visited;

    std::vector<std::filesystem::path> search_dirs;
    search_dirs.push_back(rc_full_path.parent_path());
    search_dirs.push_back(project_root);
    search_dirs.push_back(project_root / "include");
    for (const auto& id : include_dirs) {
        std::filesystem::path p = std::filesystem::u8path(id);
        search_dirs.push_back(p.is_absolute() ? p : (project_root / p));
    }

    auto resolve_ref = [&](const std::string& ref, const std::filesystem::path& cur_dir) -> std::filesystem::path {
        std::error_code ec;
        std::filesystem::path rp = std::filesystem::u8path(ref);
        if (rp.is_absolute() && std::filesystem::exists(rp, ec)) return rp;
        std::filesystem::path local = cur_dir / rp;
        if (std::filesystem::exists(local, ec)) return local;
        for (const auto& sd : search_dirs) {
            std::filesystem::path cand = sd / rp;
            if (std::filesystem::exists(cand, ec)) return cand;
        }
        return {};
    };

    auto scan_file = [&](auto& self, const std::filesystem::path& file_p) -> void {
        std::string key = ToLowerTool(file_p.lexically_normal().generic_u8string());
        if (!visited.insert(key).second) return;

        std::ifstream f(file_p);
        if (!f.is_open()) return;
        deps.push_back(file_p.lexically_normal().generic_u8string());

        std::string line;
        while (std::getline(f, line)) {
            // Strip // comments
            size_t cpos = line.find("//");
            if (cpos != std::string::npos) line.resize(cpos);

            std::string low = ToLowerTool(line);
            bool is_include = (low.find("#include") != std::string::npos);
            bool is_res_asset =
                (low.find("icon") != std::string::npos) ||
                (low.find("bitmap") != std::string::npos) ||
                (low.find("rcdata") != std::string::npos) ||
                (low.find("cursor") != std::string::npos) ||
                (low.find("font") != std::string::npos) ||
                (low.find("rt_manifest") != std::string::npos) ||
                (low.find("manifest") != std::string::npos);
            if (!is_include && !is_res_asset) continue;

            size_t q1 = line.find('"');
            if (q1 == std::string::npos) continue;
            size_t q2 = line.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 <= q1 + 1) continue;

            std::string ref = line.substr(q1 + 1, q2 - q1 - 1);
            // Unescape double backslashes used in .rc files ("res\\\\app.ico")
            std::string unesc;
            for (size_t i = 0; i < ref.size(); ++i) {
                if (ref[i] == '\\' && i + 1 < ref.size() && ref[i + 1] == '\\') {
                    unesc.push_back('/');
                    ++i;
                } else if (ref[i] == '\\') {
                    unesc.push_back('/');
                } else {
                    unesc.push_back(ref[i]);
                }
            }
            std::filesystem::path resolved = resolve_ref(unesc, file_p.parent_path());
            if (!resolved.empty()) {
                if (is_include) {
                    self(self, resolved);
                } else {
                    std::string rstr = resolved.lexically_normal().generic_u8string();
                    if (visited.insert(ToLowerTool(rstr)).second) {
                        deps.push_back(std::move(rstr));
                    }
                }
            }
        }
    };

    scan_file(scan_file, rc_full_path);
    std::sort(deps.begin(), deps.end());
    return deps;
}

std::string ToolRegistry::ComputeRcContentHash(
    const std::filesystem::path& rc_full_path,
    const std::filesystem::path& project_root,
    const std::vector<std::string>& include_dirs
) {
    std::vector<std::string> files = ScanRcDependencies(rc_full_path, project_root, include_dirs);
    uint64_t h = 14695981039346656037ULL;
    for (const auto& f : files) {
        HashBytesFnv1a(h, f);
        std::ifstream in(std::filesystem::u8path(f), std::ios::binary);
        if (in.is_open()) {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            HashBytesFnv1a(h, content);
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string ToolRegistry::FormatTypeBreakdown(
    const std::vector<std::string>& files,
    const std::vector<CustomToolSpec>& custom_tools
) {
    size_t n_cxx = 0, n_c = 0, n_rc = 0, n_asm = 0, n_cu = 0, n_ispc = 0, n_custom = 0;
    for (const auto& f : files) {
        switch (ClassifySourceFile(f, custom_tools)) {
            case SourceFileKind::Cxx: n_cxx++; break;
            case SourceFileKind::C: n_c++; break;
            case SourceFileKind::Rc: n_rc++; break;
            case SourceFileKind::AsmPre:
            case SourceFileKind::AsmRaw:
            case SourceFileKind::AsmNasm: n_asm++; break;
            case SourceFileKind::Cuda: n_cu++; break;
            case SourceFileKind::Ispc: n_ispc++; break;
            case SourceFileKind::Custom: n_custom++; break;
            default: break;
        }
    }

    std::vector<std::string> parts;
    if (n_cxx > 0) parts.push_back(std::to_string(n_cxx) + " C++");
    if (n_c > 0) parts.push_back(std::to_string(n_c) + " C");
    if (n_asm > 0) parts.push_back(std::to_string(n_asm) + " asm");
    if (n_rc > 0) parts.push_back(std::to_string(n_rc) + " rc");
    if (n_cu > 0) parts.push_back(std::to_string(n_cu) + " CUDA");
    if (n_ispc > 0) parts.push_back(std::to_string(n_ispc) + " ISPC");
    if (n_custom > 0) parts.push_back(std::to_string(n_custom) + " custom");

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += " · ";
        out += parts[i];
    }
    return out;
}

} // namespace core
