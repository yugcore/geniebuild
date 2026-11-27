#include "compdb.h"
#include "compiler.h"
#include "fs_util.h"
#include "pch.h"
#include "project.h"
#include "settings_resolver.h"
#include "source_rules.h"
#include "tool_registry.h"
#include "toolchain.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace core {

namespace {

std::string ToLowerCd(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string NormRelPath(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.rfind("./", 0) == 0) s.erase(0, 2);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string MakeRelFwd(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path norm_p = p.lexically_normal();
    std::filesystem::path norm_r = root.lexically_normal();
    std::filesystem::path rel = std::filesystem::relative(norm_p, norm_r, ec);
    if (!ec) {
        std::string rs = rel.generic_u8string();
        if (!rs.empty() && rs.rfind("..", 0) != 0) {
            return NormRelPath(rs);
        }
    }
    return norm_p.generic_u8string();
}

std::string ResolveCompilerAbsPath(const std::string& prog) {
    if (prog.empty()) return prog;
    std::filesystem::path p = std::filesystem::u8path(prog);
    if (p.is_absolute()) return p.generic_u8string();
    std::string resolved = ToolRegistry::ResolveToolExecutable(prog);
    if (!resolved.empty()) {
        std::filesystem::path rp = std::filesystem::u8path(resolved);
        std::error_code ec;
        std::filesystem::path abs = rp.is_absolute() ? rp : std::filesystem::absolute(rp, ec);
        if (!ec) return abs.generic_u8string();
        return rp.generic_u8string();
    }
    return prog;
}

void NormalizePchFlagsForCompDb(
    std::vector<std::string>& args,
    bool is_msvc,
    const std::string& real_pch_abs
) {
    std::vector<std::string> out;
    out.reserve(args.size());
    bool has_fi_or_include = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (is_msvc) {
            // Drop /Yu..., /Yc..., /Fp... ; keep /FI<pch> pointing to real pch.h
            if (a.rfind("/Yu", 0) == 0 || a.rfind("-Yu", 0) == 0 ||
                a.rfind("/Yc", 0) == 0 || a.rfind("-Yc", 0) == 0 ||
                a.rfind("/Fp", 0) == 0 || a.rfind("-Fp", 0) == 0) {
                continue;
            }
            if (a.rfind("/FI", 0) == 0 || a.rfind("-FI", 0) == 0) {
                has_fi_or_include = true;
                if (!real_pch_abs.empty()) {
                    out.push_back("/FI" + real_pch_abs);
                } else {
                    out.push_back(a);
                }
                continue;
            }
        } else {
            // Drop -include-pch <path> and -Winvalid-pch; replace generated pch.h wrapper in -include <path>
            if (a == "-include-pch") {
                if (i + 1 < args.size()) ++i;
                continue;
            }
            if (a == "-Winvalid-pch") continue;
            if (a == "-include" && i + 1 < args.size()) {
                std::string nxt = args[i + 1];
                std::string nxt_low = ToLowerCd(nxt);
                if (!real_pch_abs.empty() && nxt_low.find(".geniebuild/obj") != std::string::npos) {
                    out.push_back("-include");
                    out.push_back(real_pch_abs);
                    has_fi_or_include = true;
                    ++i;
                    continue;
                }
            }
        }
        out.push_back(a);
    }

    if (!real_pch_abs.empty() && !has_fi_or_include) {
        if (is_msvc) {
            out.push_back("/FI" + real_pch_abs);
        } else {
            out.push_back("-include");
            out.push_back(real_pch_abs);
        }
    }
    args.swap(out);
}

bool IsPathInsideDir(const std::filesystem::path& file_p, const std::filesystem::path& dir_p) {
    if (file_p.empty() || dir_p.empty()) return false;
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(file_p.lexically_normal(), dir_p.lexically_normal(), ec);
    if (ec) return false;
    std::string s = rel.generic_u8string();
    return !s.empty() && s.rfind("..", 0) != 0 && s != ".";
}

bool LooksGeneratedSourceFile(const std::filesystem::path& file_p, const std::filesystem::path& compdb_dir, const std::filesystem::path& root_p) {
    std::string fname = ToLowerCd(file_p.filename().generic_u8string());
    if (fname.rfind("moc_", 0) == 0 || fname.rfind("ui_", 0) == 0 || fname.rfind("qrc_", 0) == 0 ||
        fname.find(".pb.cc") != std::string::npos || fname.find(".pb.cpp") != std::string::npos) {
        return true;
    }
    if (!compdb_dir.empty() && compdb_dir.lexically_normal() != root_p.lexically_normal()) {
        if (IsPathInsideDir(file_p, compdb_dir)) {
            return true;
        }
    }
    std::string full_low = ToLowerCd(file_p.generic_u8string());
    if (full_low.find("/cmakefiles/") != std::string::npos || full_low.find("/.geniebuild/gen/") != std::string::npos) {
        return true;
    }
    return false;
}

std::vector<std::string> VectorIntersection(const std::vector<std::vector<std::string>>& sets) {
    if (sets.empty()) return {};
    std::vector<std::string> common = sets[0];
    // Deduplicate common preserving first-seen order
    std::vector<std::string> dedup;
    std::unordered_set<std::string> seen0;
    for (const auto& s : common) {
        if (seen0.insert(s).second) dedup.push_back(s);
    }
    for (size_t i = 1; i < sets.size(); ++i) {
        std::unordered_set<std::string> cur(sets[i].begin(), sets[i].end());
        dedup.erase(
            std::remove_if(dedup.begin(), dedup.end(), [&](const std::string& v) { return cur.find(v) == cur.end(); }),
            dedup.end()
        );
        if (dedup.empty()) break;
    }
    return dedup;
}

std::vector<std::string> VectorSubtract(const std::vector<std::string>& base, const std::unordered_set<std::string>& sub) {
    std::vector<std::string> out;
    for (const auto& v : base) {
        if (sub.find(v) == sub.end()) out.push_back(v);
    }
    return out;
}

bool IsConfigSpecificCompileFlag(const std::string& flag) {
    if (flag == "-O0" || flag == "-O1" || flag == "-O2" || flag == "-O3" ||
        flag == "-Os" || flag == "-Oz" || flag == "-Ofast" || flag == "-Og" ||
        flag == "/Od" || flag == "/O1" || flag == "/O2" || flag == "/Ox" || flag == "/Oi") {
        return true;
    }
    if (flag == "-g" || flag == "-g0" || flag == "-g1" || flag == "-g2" || flag == "-g3" ||
        flag == "-ggdb" || flag.rfind("-gdwarf", 0) == 0 ||
        flag == "/Zi" || flag == "/ZI" || flag == "/Z7" || flag == "/DEBUG") {
        return true;
    }
    return false;
}

bool IsConfigSpecificDefine(const std::string& def) {
    return def == "NDEBUG" || def == "_DEBUG";
}

void ExpandResponseFilesRecursive(
    const std::vector<std::string>& raw_args,
    const std::filesystem::path& entry_dir,
    const std::filesystem::path& project_root,
    std::vector<std::string>& out_args,
    std::unordered_map<std::string, std::vector<std::string>>& rsp_cache,
    int depth = 0
) {
    for (const std::string& a : raw_args) {
        if (depth < 4 && a.size() > 1 && a[0] == '@') {
            std::string rsp_spec = a.substr(1);
            if (rsp_spec.size() >= 2 &&
                ((rsp_spec.front() == '"' && rsp_spec.back() == '"') ||
                 (rsp_spec.front() == '\'' && rsp_spec.back() == '\''))) {
                rsp_spec = rsp_spec.substr(1, rsp_spec.size() - 2);
            }
            std::filesystem::path rp = std::filesystem::u8path(rsp_spec);
            if (!rp.is_absolute()) {
                std::filesystem::path cand1 = entry_dir / rp;
                if (fs_util::ExistsLong(cand1)) rp = cand1;
                else rp = project_root / rp;
            }
            std::string cache_key = ToLowerCd(rp.lexically_normal().generic_u8string());
            auto cit = rsp_cache.find(cache_key);
            if (cit != rsp_cache.end()) {
                ExpandResponseFilesRecursive(cit->second, rp.parent_path(), project_root, out_args, rsp_cache, depth + 1);
                continue;
            }
            std::string rsp_text;
            if (fs_util::ReadFileTextLong(rp, rsp_text)) {
                std::vector<std::string> rsp_toks = CompDbManager::SplitCommandLine(rsp_text);
                auto& cached = rsp_cache[cache_key];
                cached = rsp_toks;
                ExpandResponseFilesRecursive(cached, rp.parent_path(), project_root, out_args, rsp_cache, depth + 1);
                continue;
            }
        }
        out_args.push_back(a);
    }
}

std::string ExtractTargetTokenFromLibPath(const std::string& tok) {
    std::string low = ToLowerCd(tok);
    std::filesystem::path p = std::filesystem::u8path(tok);
    std::string fname = p.filename().generic_u8string();
    std::string flow = ToLowerCd(fname);
    if (flow.size() > 6 && flow.rfind(".dll.a") == flow.size() - 6) {
        fname.resize(fname.size() - 6);
    } else if (flow.size() > 4 && flow.rfind(".lib") == flow.size() - 4) {
        fname.resize(fname.size() - 4);
    } else if (flow.size() > 6 && flow.rfind(".dylib") == flow.size() - 6) {
        fname.resize(fname.size() - 6);
    } else if (flow.size() > 3 && flow.rfind(".so") == flow.size() - 3) {
        fname.resize(fname.size() - 3);
    } else if (flow.size() > 2 && flow.rfind(".a") == flow.size() - 2) {
        fname.resize(fname.size() - 2);
    } else {
        return "";
    }
    if (fname.rfind("lib", 0) == 0 && fname.size() > 3) {
        fname = fname.substr(3);
    }
    return fname;
}

bool EndsWithCiCd(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    return ToLowerCd(s.substr(s.size() - suf.size())) == ToLowerCd(suf);
}

bool SourceFileLooksLikeEntryPoint(const std::filesystem::path& file_abs) {
    std::string text;
    if (!fs_util::ReadFileTextLong(file_abs, text)) return false;
    if (text.size() > 512 * 1024) text.resize(512 * 1024);
    return text.find("main(") != std::string::npos ||
           text.find("main (") != std::string::npos ||
           text.find("wmain(") != std::string::npos ||
           text.find("WinMain(") != std::string::npos ||
           text.find("wWinMain(") != std::string::npos;
}

} // namespace

CompDbExportResult CompDbManager::ExportCompileCommands(
    const Project& project,
    const CompilerManager& compiler_manager,
    const ToolchainProfile* active_profile_override
) {
    CompDbExportResult res;
    if (!project.HasProject()) {
        res.error = "No project loaded.";
        return res;
    }

    CompileCommandsConfig cc_cfg = project.GetCompileCommandsConfig();
    std::filesystem::path out_path = cc_cfg.path.empty()
        ? (project.GetRootPath() / "compile_commands.json")
        : project.ResolvePath(cc_cfg.path);
    res.outputPath = out_path;

    ToolchainProfile active_tc;
    if (active_profile_override) {
        active_tc = ToolchainManager::ResolveProfileVariables(*active_profile_override, project.GetRootPath());
    } else {
        active_tc = compiler_manager.ResolveProjectToolchain(project).profile;
    }
    if (active_tc.cxx.empty() && active_tc.cc.empty()) {
        res.error = "No active compiler toolchain resolved.";
        return res;
    }

    CompilerInfo cxx_comp = compiler_manager.MakeCompilerInfoFromProfile(active_tc, false);
    CompilerInfo c_comp = compiler_manager.MakeCompilerInfoFromProfile(active_tc, true);
    cxx_comp.executable_path = std::filesystem::u8path(ResolveCompilerAbsPath(cxx_comp.executable_path.generic_u8string()));
    c_comp.executable_path = std::filesystem::u8path(ResolveCompilerAbsPath(c_comp.executable_path.generic_u8string()));

    bool is_msvc = active_tc.IsMsvcSyntax();
    bool is_clang = active_tc.IsClangDriver();
    std::string root_dir_str = project.GetRootPath().lexically_normal().generic_u8string();

    std::vector<std::string> config_list;
    if (cc_cfg.allConfigs) {
        config_list = project.GetConfigNames();
    }
    if (config_list.empty()) {
        config_list.push_back(project.GetConfiguration().empty() ? "Debug" : project.GetConfiguration());
    }

    const auto& all_targets = project.GetTargets();
    std::unordered_map<std::string, const Target*> tmap;
    for (const auto& t : all_targets) tmap[t.name] = &t;

    ConditionContext cond_ctx = BuildConditionContext(project, &active_tc);
    std::vector<const Target*> build_order;
    std::string order_err;
    if (!ResolveTargetBuildOrder(all_targets, "", build_order, order_err, &cond_ctx)) {
        for (const auto& t : all_targets) build_order.push_back(&t);
    }

    nlohmann::json entries = nlohmann::json::array();

    for (const std::string& cfg_name : config_list) {
        for (const Target* t : build_order) {
            if (!t || t->kind == TargetKind::Interface) continue;

            EffectiveTargetSettings eff = ResolveEffectiveTargetSettings(project, *t, tmap, cfg_name, is_msvc, false);
            std::vector<std::string> compiled = t->GetCompiledFiles();

            // Include existing generated C/C++ files inside .geniebuild/gen/<cfg>/<target> (V2-6 + V2-19)
            std::filesystem::path gen_dir = t->GetGenDir(project.GetRootPath(), cfg_name);
            std::unordered_set<std::string> seen_files(compiled.begin(), compiled.end());
            if (fs_util::ExistsLong(gen_dir)) {
                std::error_code gec;
                for (const auto& ge : std::filesystem::recursive_directory_iterator(fs_util::ToWinLongPath(gen_dir), gec)) {
                    if (gec) break;
                    if (!ge.is_regular_file(gec)) continue;
                    if (IsStepGeneratedSourcePath(ge.path().generic_u8string())) {
                        std::string rel_g = MakeRelFwd(ge.path(), project.GetRootPath());
                        if (seen_files.insert(rel_g).second) compiled.push_back(std::move(rel_g));
                    }
                }
            }

            auto grouped_eff = ResolveTargetFilesGrouped(project, *t, eff, compiled, cfg_name, cxx_comp.name);
            // Collect real PCH source headers so we emit -include <real.h> instead of forwarding/binary PCH
            std::string real_cxx_pch_abs;
            std::string real_c_pch_abs;
            std::unordered_set<std::string> pch_eligible_files;
            TargetPchAnalysis pch_ana = PchManager::AnalyzeAndPlanTargetPch(
                project, *t, eff, compiled, grouped_eff,
                cxx_comp, c_comp, cfg_name, is_msvc, is_clang, false, false
            );
            if (pch_ana.cxxPch.enabled && !pch_ana.cxxPch.headerRel.empty()) {
                real_cxx_pch_abs = project.ResolvePath(pch_ana.cxxPch.headerRel).lexically_normal().generic_u8string();
            }
            if (pch_ana.cPch.enabled && !pch_ana.cPch.headerRel.empty()) {
                real_c_pch_abs = project.ResolvePath(pch_ana.cPch.headerRel).lexically_normal().generic_u8string();
            }
            for (const auto& fu : pch_ana.fileUsages) {
                if (fu.usesPch) pch_eligible_files.insert(fu.file);
            }

            for (const std::string& file_rel : compiled) {
                SourceFileKind fk = ToolRegistry::ClassifySourceFile(file_rel, project.GetCustomTools());
                if (fk != SourceFileKind::C && fk != SourceFileKind::Cxx) {
                    continue; // compile_commands.json covers C/C++ translation units
                }
                bool is_c = (fk == SourceFileKind::C);
                const CompilerInfo& comp = (is_c && !c_comp.executable_path.empty()) ? c_comp : cxx_comp;

                EffectiveFileSettings feff;
                auto git = grouped_eff.find(file_rel);
                if (git != grouped_eff.end() && git->second) {
                    feff = *git->second;
                } else {
                    feff = ResolveFileSettings(project, *t, eff, file_rel, cfg_name, comp.name, is_c);
                }

                // Always produce readable non-hashed output path in compile_commands.json (V2-22)
                std::string obj_ext = is_msvc ? ".obj" : ".o";
                std::string rel_obj = ".geniebuild/obj/" + SanitizeConfigDirName(cfg_name) + "/" + t->name + "/" + NormRelPath(file_rel) + obj_ext;
                std::filesystem::path obj_abs = project.ResolvePath(rel_obj);

                BuildCommand cmd = CompilerManager::FormulateCompileCommand(comp, file_rel, obj_abs, project, feff);
                std::vector<std::string> full_args;
                full_args.reserve(cmd.arguments.size() + 1);
                full_args.push_back(ResolveCompilerAbsPath(cmd.program));
                for (const auto& a : cmd.arguments) full_args.push_back(a);

                const std::string& real_pch = is_c ? real_c_pch_abs : real_cxx_pch_abs;
                if (pch_eligible_files.find(file_rel) != pch_eligible_files.end() && !real_pch.empty()) {
                    NormalizePchFlagsForCompDb(full_args, is_msvc, real_pch);
                }

                // Strip ccache/sccache launcher if ever configured at index 0
                if (full_args.size() > 1) {
                    std::string p0 = ToLowerCd(std::filesystem::u8path(full_args[0]).filename().generic_u8string());
                    if (p0 == "ccache" || p0 == "ccache.exe" || p0 == "sccache" || p0 == "sccache.exe") {
                        full_args.erase(full_args.begin());
                        full_args[0] = ResolveCompilerAbsPath(full_args[0]);
                    }
                }

                nlohmann::json item = nlohmann::json::object();
                item["directory"] = root_dir_str;
                item["file"] = project.ResolvePath(file_rel).lexically_normal().generic_u8string();
                item["arguments"] = std::move(full_args);
                item["output"] = rel_obj;
                entries.push_back(std::move(item));
            }
        }
    }

    res.entryCount = entries.size();
    std::string serialized = entries.dump(2) + "\n";
    bool written = false;
    if (!fs_util::AtomicWriteTextIfChanged(out_path, serialized, &written)) {
        res.error = "Failed to write compile_commands.json to " + out_path.generic_u8string();
        return res;
    }
    res.success = true;
    res.fileModified = written;
    return res;
}

std::vector<std::string> CompDbManager::SplitCommandLine(const std::string& command_str) {
    std::vector<std::string> args;
    std::string cur;
    bool in_quote = false;
    char qchar = '\0';
    bool has_tok = false;

    for (size_t i = 0; i < command_str.size(); ++i) {
        char c = command_str[i];
        if (!in_quote && (c == '"' || c == '\'')) {
            in_quote = true;
            qchar = c;
            has_tok = true;
        } else if (in_quote && c == qchar) {
            in_quote = false;
            qchar = '\0';
        } else if (c == '\\' && i + 1 < command_str.size() && command_str[i + 1] == '"') {
            cur.push_back('"');
            ++i;
            has_tok = true;
        } else if (!in_quote && std::isspace(static_cast<unsigned char>(c))) {
            if (has_tok || !cur.empty()) {
                args.push_back(std::move(cur));
                cur.clear();
                has_tok = false;
            }
        } else {
            cur.push_back(c);
            has_tok = true;
        }
    }
    if (has_tok || !cur.empty()) {
        args.push_back(std::move(cur));
    }
    return args;
}

ImportPlan CompDbManager::AnalyzeCompileCommands(
    const std::filesystem::path& compdb_path,
    const std::filesystem::path& project_root
) {
    ImportPlan plan;
    plan.compdbPath = compdb_path;
    plan.projectRoot = project_root;

    std::string raw;
    if (!fs_util::ReadFileTextLong(compdb_path, raw)) {
        plan.error = "Cannot open compile_commands.json: " + compdb_path.generic_u8string();
        return plan;
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(raw);
    } catch (const std::exception& e) {
        plan.error = std::string("Invalid JSON in compile_commands.json: ") + e.what();
        return plan;
    }
    if (!arr.is_array()) {
        plan.error = "compile_commands.json root must be a JSON array.";
        return plan;
    }

    std::filesystem::path compdb_dir = compdb_path.parent_path();

    struct ParsedTu {
        std::string targetName;
        std::string fileRel;
        std::string folderRel;
        bool isC = false;
        bool sharedHint = false;
        std::vector<std::string> includeDirs;
        std::vector<std::string> systemIncludeDirs;
        std::vector<std::string> defines;
        std::vector<std::string> compileFlags;
        std::string cxxStd;
        std::string cStd;
        std::string warnings;
    };

    std::vector<ParsedTu> parsed_tus;
    parsed_tus.reserve(arr.size());
    std::unordered_set<std::string> seen_tu_keys;
    std::unordered_map<std::string, std::vector<std::string>> rsp_cache;
    std::unordered_map<std::string, std::pair<std::filesystem::path, std::filesystem::path>> cmake_dir_by_target;

    auto extract_cmake_dir = [&](const std::string& out_s, const std::filesystem::path& entry_dir) -> std::filesystem::path {
        std::string norm = out_s;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        size_t pos = norm.find("CMakeFiles/");
        if (pos == std::string::npos) return {};
        size_t dir_end = norm.find(".dir/", pos + 11);
        if (dir_end == std::string::npos) {
            if (norm.size() >= 4 && norm.rfind(".dir") == norm.size() - 4) dir_end = norm.size() - 4;
            else return {};
        }
        std::filesystem::path rel_dir = std::filesystem::u8path(norm.substr(0, dir_end + 4));
        if (rel_dir.is_absolute()) return rel_dir;
        std::filesystem::path c1 = compdb_dir / rel_dir;
        if (fs_util::ExistsLong(c1)) return c1;
        std::filesystem::path c2 = entry_dir / rel_dir;
        if (fs_util::ExistsLong(c2)) return c2;
        return {};
    };

    auto resolve_entry_path = [&](const std::string& p, const std::filesystem::path& base_dir) -> std::string {
        std::filesystem::path fp = std::filesystem::u8path(p);
        if (!fp.is_absolute() && !base_dir.empty()) {
            fp = base_dir / fp;
        }
        return MakeRelFwd(fp, project_root);
    };

    auto extract_cmake_target = [](const std::string& out_s) -> std::string {
        std::string norm = out_s;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        size_t pos = norm.find("CMakeFiles/");
        if (pos == std::string::npos) return "";
        size_t start = pos + 11;
        size_t dir_pos = norm.find(".dir/", start);
        if (dir_pos == std::string::npos) dir_pos = norm.find(".dir", start);
        if (dir_pos != std::string::npos && dir_pos > start) {
            return norm.substr(start, dir_pos - start);
        }
        return "";
    };

    for (const auto& entry : arr) {
        if (!entry.is_object()) continue;
        plan.totalEntriesParsed++;

        std::string dir_s = entry.value("directory", "");
        std::string file_s = entry.value("file", "");
        if (file_s.empty()) continue;

        std::filesystem::path entry_dir = dir_s.empty() ? project_root : std::filesystem::u8path(dir_s);
        std::filesystem::path file_abs = std::filesystem::u8path(file_s);
        if (!file_abs.is_absolute()) file_abs = entry_dir / file_abs;
        file_abs = file_abs.lexically_normal();

        if (LooksGeneratedSourceFile(file_abs, compdb_dir, project_root)) {
            plan.excludedGeneratedCount++;
            if (plan.warningsList.size() < 40) {
                plan.warningsList.push_back(
                    file_abs.filename().generic_u8string() + " generated by original build system; recreate with a build step (V2-6)."
                );
            }
            continue;
        }

        std::string file_rel = MakeRelFwd(file_abs, project_root);
        std::vector<std::string> raw_argv;
        if (entry.contains("arguments") && entry["arguments"].is_array()) {
            for (const auto& a : entry["arguments"]) {
                if (a.is_string()) raw_argv.push_back(a.get<std::string>());
            }
        } else if (entry.contains("command") && entry["command"].is_string()) {
            raw_argv = SplitCommandLine(entry["command"].get<std::string>());
        }
        if (raw_argv.empty()) continue;

        std::vector<std::string> argv;
        argv.reserve(raw_argv.size() + 8);
        argv.push_back(raw_argv[0]);
        if (raw_argv.size() > 1) {
            std::vector<std::string> tail(raw_argv.begin() + 1, raw_argv.end());
            ExpandResponseFilesRecursive(tail, entry_dir, project_root, argv, rsp_cache);
        }

        std::string out_s = entry.value("output", "");
        if (out_s.empty()) {
            for (size_t i = 1; i < argv.size(); ++i) {
                if (argv[i] == "-o" && i + 1 < argv.size()) { out_s = argv[i + 1]; break; }
                if (argv[i].rfind("/Fo", 0) == 0 && argv[i].size() > 3) { out_s = argv[i].substr(3); break; }
            }
        }

        std::string tname = extract_cmake_target(out_s);
        if (!tname.empty() && cmake_dir_by_target.find(tname) == cmake_dir_by_target.end()) {
            std::filesystem::path cdir = extract_cmake_dir(out_s, entry_dir);
            if (!cdir.empty()) {
                cmake_dir_by_target[tname] = { cdir, entry_dir };
            }
        }
        if (tname.empty()) {
            std::filesystem::path rp(file_rel);
            std::string top = rp.has_parent_path() ? rp.parent_path().begin()->generic_u8string() : "";
            if (top.empty() || top == "." || top == "src") {
                tname = project_root.filename().generic_u8string();
                if (tname.empty()) tname = "app";
            } else {
                tname = top;
            }
        }

        if (!seen_tu_keys.insert(tname + "|" + file_rel).second) continue;

        ParsedTu tu;
        tu.targetName = tname;
        tu.fileRel = file_rel;
        std::filesystem::path parent_p = std::filesystem::u8path(file_rel).parent_path();
        tu.folderRel = parent_p.empty() ? "" : NormRelPath(parent_p.generic_u8string());
        tu.isC = (ToLowerCd(std::filesystem::u8path(file_rel).extension().generic_u8string()) == ".c");

        std::string file_fname_low = ToLowerCd(file_abs.filename().generic_u8string());
        std::string expected_exports_def = ToLowerCd(tname) + "_exports";
        auto push_define = [&](const std::string& def_tok) {
            if (def_tok.empty() || IsConfigSpecificDefine(def_tok)) return;
            std::string dl = ToLowerCd(def_tok);
            size_t eq = dl.find('=');
            std::string name_part = (eq == std::string::npos) ? dl : dl.substr(0, eq);
            if (name_part == expected_exports_def || name_part == "_usrdll" || name_part == "_windll" ||
                (name_part.size() > 16 && name_part.rfind("_build_dll_export") == name_part.size() - 17)) {
                tu.sharedHint = true;
            }
            tu.defines.push_back(def_tok);
        };

        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string& a = argv[i];
            if (a.empty()) continue;

            // Skip compiler noise and per-TU file/output arguments
            if (a == "-c" || a == "/c" || a == "-MD" || a == "-MMD" || a == "/showIncludes" || a == "/FS" || a == "-MP") continue;
            if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") { if (i + 1 < argv.size()) ++i; continue; }
            if (a.rfind("/Fo", 0) == 0 || a.rfind("/Fd", 0) == 0 || a.rfind("/Fa", 0) == 0 || a.rfind("/Fp", 0) == 0) continue;
            if (ToLowerCd(std::filesystem::u8path(a).filename().generic_u8string()) == file_fname_low &&
                (a.find(file_abs.filename().generic_u8string()) != std::string::npos || a == file_s || a == file_rel)) {
                continue;
            }

            if (a == "-I" && i + 1 < argv.size()) {
                tu.includeDirs.push_back(resolve_entry_path(argv[++i], entry_dir));
            } else if ((a.rfind("-I", 0) == 0 || a.rfind("/I", 0) == 0) && a.size() > 2) {
                tu.includeDirs.push_back(resolve_entry_path(a.substr(2), entry_dir));
            } else if (a == "-isystem" && i + 1 < argv.size()) {
                tu.systemIncludeDirs.push_back(resolve_entry_path(argv[++i], entry_dir));
            } else if (a.rfind("-isystem", 0) == 0 && a.size() > 8) {
                tu.systemIncludeDirs.push_back(resolve_entry_path(a.substr(8), entry_dir));
            } else if ((a == "-D" || a == "/D") && i + 1 < argv.size()) {
                push_define(argv[++i]);
            } else if ((a.rfind("-D", 0) == 0 || a.rfind("/D", 0) == 0) && a.size() > 2) {
                push_define(a.substr(2));
            } else if (a.rfind("-std=", 0) == 0) {
                std::string std_val = a.substr(5);
                if (tu.isC) tu.cStd = std_val;
                else tu.cxxStd = std_val;
            } else if (a.rfind("/std:", 0) == 0) {
                std::string std_val = a.substr(5);
                if (tu.isC) tu.cStd = std_val;
                else tu.cxxStd = std_val;
            } else if (a == "-Wall" || a == "-Wextra" || a == "/W3" || a == "/W4" || a == "/Wall" || a == "-w" || a == "/W0") {
                if (!tu.warnings.empty() && tu.warnings.find(a) == std::string::npos) {
                    tu.warnings += " " + a;
                } else {
                    tu.warnings = a;
                }
            } else if (a == "-shared" || a == "/LD" || a == "/LDd" || a == "/DLL") {
                tu.sharedHint = true;
                tu.compileFlags.push_back(a);
            } else if (IsConfigSpecificCompileFlag(a)) {
                continue;
            } else {
                tu.compileFlags.push_back(a);
            }
        }

        parsed_tus.push_back(std::move(tu));
    }

    if (parsed_tus.empty()) {
        plan.error = "No non-generated C/C++ translation units found in compile_commands.json.";
        return plan;
    }

    // Factor project-wide intersections across all TUs
    {
        std::vector<std::vector<std::string>> all_inc, all_sys, all_def, all_cf;
        std::unordered_map<std::string, int> cxx_hist, c_hist, warn_hist;
        for (const auto& tu : parsed_tus) {
            all_inc.push_back(tu.includeDirs);
            all_sys.push_back(tu.systemIncludeDirs);
            all_def.push_back(tu.defines);
            all_cf.push_back(tu.compileFlags);
            if (!tu.cxxStd.empty()) cxx_hist[tu.cxxStd]++;
            if (!tu.cStd.empty()) c_hist[tu.cStd]++;
            if (!tu.warnings.empty()) warn_hist[tu.warnings]++;
        }
        plan.projectIncludeDirs = VectorIntersection(all_inc);
        plan.projectSystemIncludeDirs = VectorIntersection(all_sys);
        plan.projectDefines = VectorIntersection(all_def);
        plan.projectCompileFlags = VectorIntersection(all_cf);

        auto pick_most = [](const std::unordered_map<std::string, int>& h, const std::string& def_v) {
            std::string best = def_v;
            int cnt = 0;
            for (const auto& [k, v] : h) {
                if (v > cnt) { cnt = v; best = k; }
            }
            return best;
        };
        plan.cxxStd = pick_most(cxx_hist, "c++17");
        plan.cStd = pick_most(c_hist, "c11");
        plan.warnings = pick_most(warn_hist, "-Wall");
    }

    std::unordered_set<std::string> proj_inc_set(plan.projectIncludeDirs.begin(), plan.projectIncludeDirs.end());
    std::unordered_set<std::string> proj_sys_set(plan.projectSystemIncludeDirs.begin(), plan.projectSystemIncludeDirs.end());
    std::unordered_set<std::string> proj_def_set(plan.projectDefines.begin(), plan.projectDefines.end());
    std::unordered_set<std::string> proj_cf_set(plan.projectCompileFlags.begin(), plan.projectCompileFlags.end());

    // Group TUs by target in deterministic order
    std::vector<std::string> target_order;
    std::unordered_map<std::string, std::vector<const ParsedTu*>> by_target;
    for (const auto& tu : parsed_tus) {
        if (by_target.find(tu.targetName) == by_target.end()) target_order.push_back(tu.targetName);
        by_target[tu.targetName].push_back(&tu);
    }

    // Count total real source files per folder on disk once to suggest folder rules (V2-24)
    std::unordered_map<std::string, size_t> folder_disk_counts;
    {
        std::error_code ec;
        for (const auto& tu : parsed_tus) {
            if (tu.folderRel.empty() || folder_disk_counts.count(tu.folderRel)) continue;
            std::filesystem::path fdir = project_root / std::filesystem::u8path(tu.folderRel);
            size_t cnt = 0;
            for (const auto& de : std::filesystem::directory_iterator(fs_util::ToWinLongPath(fdir), ec)) {
                if (ec) break;
                if (!de.is_regular_file(ec)) continue;
                SourceFileKind fk = ToolRegistry::ClassifySourceFile(de.path().generic_u8string());
                if (fk == SourceFileKind::C || fk == SourceFileKind::Cxx) ++cnt;
            }
            folder_disk_counts[tu.folderRel] = cnt;
        }
    }

    std::unordered_set<std::string> all_target_names;
    for (const auto& tname : target_order) all_target_names.insert(tname);

    for (const auto& tname : target_order) {
        const auto& tus = by_target[tname];
        ImportedTargetSuggestion ts;
        ts.name = tname;
        bool has_shared_hint = false;
        for (const ParsedTu* tu : tus) {
            if (tu->sharedHint) { has_shared_hint = true; break; }
        }

        // Parse CMake link.txt + linkLibs.rsp when available for exact target kind, deps, and libs
        bool resolved_from_link_txt = false;
        auto cm_it = cmake_dir_by_target.find(tname);
        if (cm_it != cmake_dir_by_target.end()) {
            std::filesystem::path link_txt = cm_it->second.first / "link.txt";
            std::string link_raw;
            if (fs_util::ReadFileTextLong(link_txt, link_raw)) {
                resolved_from_link_txt = true;
                std::vector<std::string> link_raw_toks = SplitCommandLine(link_raw);
                std::vector<std::string> link_toks;
                ExpandResponseFilesRecursive(link_raw_toks, cm_it->second.second, project_root, link_toks, rsp_cache);

                bool saw_ar = false;
                bool saw_shared = has_shared_hint;
                bool saw_exe_out = false;
                bool saw_lib_out = false;
                std::unordered_set<std::string> seen_deps, seen_libs;

                for (size_t li = 0; li < link_toks.size(); ++li) {
                    const std::string& lt = link_toks[li];
                    std::string low_lt = ToLowerCd(lt);
                    std::string fname_low = ToLowerCd(std::filesystem::u8path(lt).filename().generic_u8string());
                    if (fname_low == "ar" || fname_low == "ar.exe" || fname_low == "gcc-ar" || fname_low == "gcc-ar.exe" ||
                        fname_low == "llvm-ar" || fname_low == "llvm-ar.exe" || fname_low == "lib.exe") {
                        saw_ar = true;
                    }
                    if (lt == "-shared" || lt == "/DLL" || lt == "/LD" || lt == "/LDd") {
                        saw_shared = true;
                    }
                    if ((lt == "-o" || lt == "/OUT:") && li + 1 < link_toks.size()) {
                        std::string out_arg = link_toks[li + 1];
                        if (EndsWithCiCd(out_arg, ".exe")) saw_exe_out = true;
                        else if (EndsWithCiCd(out_arg, ".dll") || EndsWithCiCd(out_arg, ".so") || EndsWithCiCd(out_arg, ".dylib")) saw_shared = true;
                        else if (EndsWithCiCd(out_arg, ".a") || EndsWithCiCd(out_arg, ".lib")) saw_lib_out = true;
                    }
                    if (lt.rfind("/OUT:", 0) == 0) {
                        if (EndsWithCiCd(lt, ".exe")) saw_exe_out = true;
                        else if (EndsWithCiCd(lt, ".dll")) saw_shared = true;
                        else if (EndsWithCiCd(lt, ".lib")) saw_lib_out = true;
                    }

                    // Extract inter-target deps and external/system libs from link.txt / linkLibs.rsp
                    if (lt.rfind("-l", 0) == 0 && lt.size() > 2) {
                        std::string lname = lt.substr(2);
                        if (lname != tname && seen_libs.insert(lname).second) {
                            ts.libs.push_back(lname);
                        }
                    } else if ((EndsWithCiCd(lt, ".a") || EndsWithCiCd(lt, ".lib") || EndsWithCiCd(lt, ".dll.a") ||
                                EndsWithCiCd(lt, ".so") || EndsWithCiCd(lt, ".dylib")) &&
                               low_lt.find("cmakefiles/") == std::string::npos &&
                               fname_low != "objects.a") {
                        std::string cand = ExtractTargetTokenFromLibPath(lt);
                        if (!cand.empty() && cand != tname && all_target_names.count(cand)) {
                            if (seen_deps.insert(cand).second) ts.deps.push_back(cand);
                        } else if (!cand.empty() && cand != tname && lt.find("-l") != 0 &&
                                   lt.rfind("CMakeFiles", 0) != 0 && fname_low != ("lib" + ToLowerCd(tname) + ".a")) {
                            std::string norm_lib = lt;
                            std::replace(norm_lib.begin(), norm_lib.end(), '\\', '/');
                            if (seen_libs.insert(norm_lib).second) ts.libs.push_back(norm_lib);
                        }
                    }
                }

                if (saw_shared) ts.kind = TargetKind::Shared;
                else if (saw_exe_out) ts.kind = TargetKind::Executable;
                else if (saw_ar || saw_lib_out) ts.kind = TargetKind::Static;
                else ts.kind = TargetKind::Executable;
            }
        }

        if (!resolved_from_link_txt) {
            std::string low_tn = ToLowerCd(tname);
            if (has_shared_hint) {
                ts.kind = TargetKind::Shared;
            } else if (low_tn.find("lib") != std::string::npos || low_tn == "imgui" || low_tn == "glfw" ||
                       low_tn == "reproc" || low_tn == "reproc++" || low_tn == "nfd" || low_tn == "core") {
                ts.kind = TargetKind::Static;
            } else {
                bool has_main = false;
                for (const ParsedTu* tu : tus) {
                    if (SourceFileLooksLikeEntryPoint(project_root / std::filesystem::u8path(tu->fileRel))) {
                        has_main = true;
                        break;
                    }
                }
                ts.kind = has_main ? TargetKind::Executable : TargetKind::Static;
            }
        }

        std::vector<std::vector<std::string>> t_inc, t_sys, t_def, t_cf;
        for (const ParsedTu* tu : tus) {
            ts.files.push_back(tu->fileRel);
            t_inc.push_back(VectorSubtract(tu->includeDirs, proj_inc_set));
            t_sys.push_back(VectorSubtract(tu->systemIncludeDirs, proj_sys_set));
            t_def.push_back(VectorSubtract(tu->defines, proj_def_set));
            t_cf.push_back(VectorSubtract(tu->compileFlags, proj_cf_set));
        }

        ts.includeDirs = VectorIntersection(t_inc);
        ts.systemIncludeDirs = VectorIntersection(t_sys);
        ts.defines = VectorIntersection(t_def);
        ts.compileFlags = VectorIntersection(t_cf);

        std::unordered_set<std::string> t_inc_set(proj_inc_set); t_inc_set.insert(ts.includeDirs.begin(), ts.includeDirs.end());
        std::unordered_set<std::string> t_sys_set(proj_sys_set); t_sys_set.insert(ts.systemIncludeDirs.begin(), ts.systemIncludeDirs.end());
        std::unordered_set<std::string> t_def_set(proj_def_set); t_def_set.insert(ts.defines.begin(), ts.defines.end());
        std::unordered_set<std::string> t_cf_set(proj_cf_set);   t_cf_set.insert(ts.compileFlags.begin(), ts.compileFlags.end());

        std::vector<std::string> folder_order;
        std::unordered_map<std::string, std::vector<const ParsedTu*>> by_folder;
        for (const ParsedTu* tu : tus) {
            if (by_folder.find(tu->folderRel) == by_folder.end()) folder_order.push_back(tu->folderRel);
            by_folder[tu->folderRel].push_back(tu);
        }

        for (const auto& frel : folder_order) {
            const auto& ftus = by_folder[frel];
            ImportedFolderGroup fg;
            fg.folderRel = frel;
            std::vector<std::vector<std::string>> f_inc, f_sys, f_def, f_cf;
            for (const ParsedTu* tu : ftus) {
                fg.files.push_back(tu->fileRel);
                f_inc.push_back(VectorSubtract(tu->includeDirs, t_inc_set));
                f_sys.push_back(VectorSubtract(tu->systemIncludeDirs, t_sys_set));
                f_def.push_back(VectorSubtract(tu->defines, t_def_set));
                f_cf.push_back(VectorSubtract(tu->compileFlags, t_cf_set));
            }
            fg.includeDirs = VectorIntersection(f_inc);
            fg.systemIncludeDirs = VectorIntersection(f_sys);
            fg.defines = VectorIntersection(f_def);
            fg.compileFlags = VectorIntersection(f_cf);

            auto dit = folder_disk_counts.find(frel);
            if (!frel.empty() && dit != folder_disk_counts.end() && dit->second > 0 && fg.files.size() == dit->second) {
                fg.useFolderRule = true;
            }
            ts.folders.push_back(std::move(fg));
        }

        plan.targets.push_back(std::move(ts));
    }

    plan.warningsList.push_back("compile_commands.json lacks link information; review Target Kinds, Dependencies, and Libraries before building.");
    plan.success = true;
    return plan;
}

bool CompDbManager::WriteImportedProject(
    const ImportPlan& plan,
    const std::filesystem::path& project_root,
    std::string& out_error
) {
    if (!plan.success || plan.targets.empty()) {
        out_error = "Import plan has no targets.";
        return false;
    }

    std::filesystem::path gdir = project_root / ".geniebuild";
    std::filesystem::path pfile = gdir / "project.json";
    std::filesystem::path backup = gdir / "project.import.backup.json";
    std::error_code ec;
    fs_util::CreateDirsLong(gdir, ec);
    if (fs_util::ExistsLong(pfile)) {
        fs_util::CopyFileLong(pfile, backup, ec);
    }

    Project proj;
    proj.SetRootPath(project_root);
    proj.SetCxxStd(plan.cxxStd);
    proj.SetCStd(plan.cStd);
    proj.SetWarnings(plan.warnings);
    proj.SetIncludeDirs(plan.projectIncludeDirs);
    proj.SetDefines(plan.projectDefines);
    proj.SetCompileFlags(plan.projectCompileFlags);

    // Preserve first executable as startup target and batch-assign targets without O(T^2) re-expansion
    std::vector<Target> built_targets;
    built_targets.reserve(plan.targets.size());
    std::string startup;
    for (size_t i = 0; i < plan.targets.size(); ++i) {
        const auto& ts = plan.targets[i];
        Target t;
        t.name = ts.name;
        t.kind = ts.kind;
        t.deps = ts.deps;
        t.priv.libs = ts.libs;
        t.priv.includeDirs = ts.includeDirs;
        t.priv.systemIncludeDirs = ts.systemIncludeDirs;
        t.priv.defines = ts.defines;
        t.priv.compileFlags = ts.compileFlags;

        std::unordered_set<std::string> covered_by_rule;
        for (const auto& fg : ts.folders) {
            if (fg.useFolderRule && !fg.folderRel.empty()) {
                SourceRule sr;
                sr.include = fg.folderRel + "/*.{c,cpp,cc,cxx}";
                t.sources.push_back(std::move(sr));
                for (const auto& f : fg.files) covered_by_rule.insert(f);
            }
            if (!fg.includeDirs.empty() || !fg.systemIncludeDirs.empty() || !fg.defines.empty() || !fg.compileFlags.empty()) {
                FlagOverride fo;
                fo.match = fg.folderRel.empty() ? "*" : (fg.folderRel + "/*");
                fo.settings.includeDirs = fg.includeDirs;
                fo.settings.systemIncludeDirs = fg.systemIncludeDirs;
                fo.settings.defines = fg.defines;
                fo.settings.compileFlags = fg.compileFlags;
                t.overrides.push_back(std::move(fo));
            }
        }
        for (const auto& f : ts.files) {
            if (covered_by_rule.find(f) == covered_by_rule.end()) {
                t.files.push_back(f);
            }
        }

        if (startup.empty() && t.kind == TargetKind::Executable) {
            startup = t.name;
        }
        built_targets.push_back(std::move(t));
    }
    if (startup.empty() && !built_targets.empty()) {
        startup = built_targets[0].name;
    }

    std::string berr;
    if (!proj.SetTargetsBatch(built_targets, startup, berr, false)) {
        out_error = berr;
        return false;
    }

    if (!proj.SaveImmediate()) {
        out_error = "Failed to save imported project.json.";
        return false;
    }
    return true;
}

} // namespace core
