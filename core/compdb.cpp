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
#include <sstream>
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
    std::string full_low = ToLowerCd(file_p.generic_u8string());
    if (full_low.find("/cmakefiles/") != std::string::npos || full_low.find("/.geniebuild/gen/") != std::string::npos) {
        return true;
    }
    // FetchContent sources live under build/_deps/<pkg>-src/; keep them as real source trees
    if (full_low.find("/_deps/") != std::string::npos && full_low.find("-src/") != std::string::npos) {
        return false;
    }
    if (!compdb_dir.empty() && compdb_dir.lexically_normal() != root_p.lexically_normal()) {
        if (IsPathInsideDir(file_p, compdb_dir)) {
            return true;
        }
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

std::string TrimCd(std::string s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string ReplaceAllCd(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string UnwrapShellCommandCd(std::string cmd) {
    cmd = TrimCd(cmd);
    while (!cmd.empty() && cmd.front() == '@') {
        cmd.erase(0, 1);
        cmd = TrimCd(cmd);
    }
    std::string low = ToLowerCd(cmd);
    size_t c_pos = std::string::npos;
    if (low.rfind("cmd.exe /c ", 0) == 0 || low.rfind("cmd /c ", 0) == 0) {
        c_pos = low.find("/c ") + 3;
    } else {
        size_t exe_pos = low.find("cmd.exe");
        if (exe_pos != std::string::npos) {
            size_t after = exe_pos + 7;
            if (after < low.size() && low[after] == '"') ++after;
            while (after < low.size() && std::isspace(static_cast<unsigned char>(low[after]))) ++after;
            if (after + 3 <= low.size() && low.substr(after, 3) == "/c ") {
                bool valid_prefix = (exe_pos == 0) || cmd[exe_pos - 1] == '/' || cmd[exe_pos - 1] == '\\' || cmd[exe_pos - 1] == '"';
                if (valid_prefix) c_pos = after + 3;
            }
        }
    }
    if (c_pos != std::string::npos && c_pos <= cmd.size()) {
        std::string inner = TrimCd(cmd.substr(c_pos));
        if (inner.size() >= 2 && inner.front() == '"' && inner.back() == '"') {
            inner = inner.substr(1, inner.size() - 2);
        }
        return TrimCd(inner);
    }
    return cmd;
}

std::string EnsureBuildCwdCommandCd(
    const std::string& cmd,
    const std::filesystem::path& compdb_dir,
    const std::filesystem::path& project_root
) {
    std::string trimmed = TrimCd(cmd);
    if (trimmed.empty()) return trimmed;
    if (compdb_dir.empty() || compdb_dir.lexically_normal() == project_root.lexically_normal()) {
        return trimmed;
    }
    std::string low = ToLowerCd(trimmed);
    if (low.rfind("cd ", 0) == 0 || low.rfind("cd\t", 0) == 0) {
        return trimmed;
    }
    std::string bdir_rel = MakeRelFwd(compdb_dir, project_root);
    if (bdir_rel.empty() || bdir_rel == ".") return trimmed;
    std::string quoted_dir = (bdir_rel.find_first_of(" \t") != std::string::npos) ? ("\"" + bdir_rel + "\"") : bdir_rel;
#if defined(_WIN32)
    return "cd /D " + quoted_dir + " && " + trimmed;
#else
    return "cd " + quoted_dir + " && " + trimmed;
#endif
}

bool IsIgnoredCodegenOutputCd(const std::string& low_path) {
    if (low_path.empty()) return true;
    std::string norm = low_path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    if (norm.find("cmakefiles/") != std::string::npos || norm.find(".geniebuild/") != std::string::npos) {
        return true;
    }
    std::filesystem::path p = std::filesystem::u8path(norm);
    std::string fname = p.filename().generic_u8string();
    std::string ext = p.extension().generic_u8string();
    if (fname == ".phony" || fname == "cmake_force" || fname == "cmake_check_build_system" ||
        fname == "cmakecache.txt" || fname == "build.ninja" || fname == "all" ||
        fname == "clean" || fname == "install" || fname == "test" || fname == "package" ||
        fname == "edit_cache" || fname == "rebuild_cache" || fname == "codegen") {
        return true;
    }
    if (EndsWithCiCd(norm, ".stamp") || EndsWithCiCd(norm, ".cmake")) return true;
    if (ext == ".obj" || ext == ".o" || ext == ".i" || ext == ".s" || ext == ".d" ||
        ext == ".a" || ext == ".lib" || ext == ".so" || ext == ".dylib" || ext == ".dll" ||
        ext == ".exe" || ext == ".pdb" || ext == ".rsp" || ext == ".manifest" ||
        ext == ".ilk" || ext == ".exp") {
        return true;
    }
    return false;
}

bool IsHousekeepingCodegenInputCd(const std::string& low_path) {
    if (low_path.empty()) return true;
    std::string norm = low_path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    if (norm.find("cmakefiles/") != std::string::npos) return true;
    std::filesystem::path p = std::filesystem::u8path(norm);
    std::string fname = p.filename().generic_u8string();
    if (fname == "cmake_force" || fname == "cmake_check_build_system" || fname == "build.ninja") return true;
    if (EndsWithCiCd(norm, ".stamp") || EndsWithCiCd(norm, ".make") || EndsWithCiCd(norm, ".cmake")) return true;
    return false;
}

std::string ExpandNinjaVarsAndUnescape(
    const std::string& raw,
    const std::unordered_map<std::string, std::string>& vars
) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '$' && i + 1 < raw.size()) {
            char nxt = raw[i + 1];
            if (nxt == '$' || nxt == ' ' || nxt == ':' || nxt == '\n') {
                if (nxt != '\n') out.push_back(nxt);
                ++i;
                continue;
            }
            if (nxt == '{') {
                size_t close = raw.find('}', i + 2);
                if (close != std::string::npos) {
                    std::string vname = raw.substr(i + 2, close - (i + 2));
                    auto it = vars.find(vname);
                    if (it != vars.end()) out += it->second;
                    i = close;
                    continue;
                }
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

std::vector<std::string> SplitNinjaTokenList(
    const std::string& raw,
    const std::unordered_map<std::string, std::string>& vars
) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '$' && i + 1 < raw.size()) {
            cur.push_back(c);
            cur.push_back(raw[i + 1]);
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                std::string un = TrimCd(ExpandNinjaVarsAndUnescape(cur, vars));
                if (!un.empty()) out.push_back(std::move(un));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        std::string un = TrimCd(ExpandNinjaVarsAndUnescape(cur, vars));
        if (!un.empty()) out.push_back(std::move(un));
    }
    return out;
}

struct ImportedCustomStepCandidate {
    BuildStep step;
    std::string preferredTarget;
};

std::string MakeBuildRelNormalized(
    const std::string& raw_tok,
    const std::filesystem::path& compdb_dir,
    const std::filesystem::path& project_root
) {
    std::string t = TrimCd(raw_tok);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        t = t.substr(1, t.size() - 2);
    }
    if (t.empty()) return "";
    std::filesystem::path p = std::filesystem::u8path(t);
    if (!p.is_absolute()) {
        p = compdb_dir / p;
    }
    return MakeRelFwd(p.lexically_normal(), project_root);
}

std::vector<ImportedCustomStepCandidate> ExtractNinjaCustomSteps(
    const std::filesystem::path& ninja_path,
    const std::filesystem::path& compdb_dir,
    const std::filesystem::path& project_root,
    std::unordered_map<std::string, std::string>& out_target_by_output
) {
    std::vector<ImportedCustomStepCandidate> results;
    std::string raw;
    if (!fs_util::ReadFileTextLong(ninja_path, raw)) return results;

    // Fold Ninja '$' line continuations into logical lines
    std::vector<std::string> lines;
    {
        std::istringstream iss(raw);
        std::string line, logical;
        bool continuing = false;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (continuing) {
                size_t a = 0;
                while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
                line.erase(0, a);
                logical += line;
            } else {
                logical = std::move(line);
            }
            // Odd trailing '$' count means Ninja line continuation
            size_t dollar_run = 0;
            for (size_t k = logical.size(); k > 0 && logical[k - 1] == '$'; --k) ++dollar_run;
            if (dollar_run % 2 == 1) {
                logical.pop_back();
                continuing = true;
            } else {
                continuing = false;
                lines.push_back(std::move(logical));
            }
        }
        if (continuing && !logical.empty()) {
            lines.push_back(std::move(logical));
        }
    }

    std::unordered_map<std::string, std::string> vars;
    std::unordered_map<std::string, std::vector<std::string>> order_deps_by_phony;

    auto find_ninja_colon = [](const std::string& s) -> size_t {
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '$' && i + 1 < s.size()) { ++i; continue; }
            if (s[i] == ':') return i;
        }
        return std::string::npos;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (ln.empty() || ln[0] == '#') continue;

        if (ln[0] != ' ' && ln[0] != '\t' && ln.rfind("build ", 0) != 0 && ln.rfind("rule ", 0) != 0) {
            size_t eq = ln.find('=');
            if (eq != std::string::npos) {
                std::string k = TrimCd(ln.substr(0, eq));
                std::string v = TrimCd(ExpandNinjaVarsAndUnescape(ln.substr(eq + 1), vars));
                if (!k.empty()) vars[k] = v;
            }
            continue;
        }

        if (ln.rfind("build ", 0) != 0) continue;

        size_t colon = find_ninja_colon(ln);
        if (colon == std::string::npos) continue;

        std::string outs_part = TrimCd(ln.substr(6, colon - 6));
        std::string rest_part = TrimCd(ln.substr(colon + 1));

        std::string raw_command, raw_desc, raw_depfile;
        while (i + 1 < lines.size() && !lines[i + 1].empty() && (lines[i + 1][0] == ' ' || lines[i + 1][0] == '\t')) {
            ++i;
            std::string sub = TrimCd(lines[i]);
            size_t eq = sub.find('=');
            if (eq == std::string::npos) continue;
            std::string k = TrimCd(sub.substr(0, eq));
            std::string v = TrimCd(sub.substr(eq + 1));
            if (k == "COMMAND") raw_command = ExpandNinjaVarsAndUnescape(v, vars);
            else if (k == "DESC") raw_desc = ExpandNinjaVarsAndUnescape(v, vars);
            else if (k == "depfile") raw_depfile = ExpandNinjaVarsAndUnescape(v, vars);
        }

        // Parse rule name + inputs (stopping at order-only '||')
        std::vector<std::string> rest_toks = SplitNinjaTokenList(rest_part, vars);
        if (rest_toks.empty()) continue;
        const std::string& rule_name = rest_toks[0];

        std::vector<std::string> raw_outs = SplitNinjaTokenList(outs_part, vars);

        // Track cmake_object_order_depends_target_<T> phony edges to infer consuming targets
        if (rule_name == "phony") {
            for (const auto& ot : raw_outs) {
                const std::string prefix = "cmake_object_order_depends_target_";
                if (ot.rfind(prefix, 0) == 0 && ot.size() > prefix.size()) {
                    std::string tgt = ot.substr(prefix.size());
                    for (size_t ri = 1; ri < rest_toks.size(); ++ri) {
                        if (rest_toks[ri] == "|" || rest_toks[ri] == "||") continue;
                        std::string out_rel = MakeBuildRelNormalized(rest_toks[ri], compdb_dir, project_root);
                        std::string low_rel = ToLowerCd(out_rel);
                        if (!IsIgnoredCodegenOutputCd(low_rel)) {
                            out_target_by_output.emplace(low_rel, tgt);
                        }
                    }
                }
            }
            continue;
        }

        if (rule_name.rfind("CUSTOM_COMMAND", 0) != 0 && raw_command.empty()) continue;
        std::string cmd_clean = UnwrapShellCommandCd(raw_command);
        if (cmd_clean.empty()) continue;

        BuildStep st;
        st.phase = StepPhase::PreBuild;
        st.shell = true;
        st.command = EnsureBuildCwdCommandCd(cmd_clean, compdb_dir, project_root);

        for (const auto& ot : raw_outs) {
            if (ot == "|") continue;
            std::string rel = MakeBuildRelNormalized(ot, compdb_dir, project_root);
            std::string low_rel = ToLowerCd(rel);
            if (!IsIgnoredCodegenOutputCd(low_rel)) {
                if (std::find(st.outputs.begin(), st.outputs.end(), rel) == st.outputs.end()) {
                    st.outputs.push_back(rel);
                    if (IsStepGeneratedSourcePath(rel)) st.generatesSources = true;
                }
            }
        }
        if (st.outputs.empty()) continue;

        bool in_order_only = false;
        for (size_t ri = 1; ri < rest_toks.size(); ++ri) {
            const std::string& itok = rest_toks[ri];
            if (itok == "||") { in_order_only = true; continue; }
            if (itok == "|") continue;
            if (in_order_only) continue;
            std::string rel = MakeBuildRelNormalized(itok, compdb_dir, project_root);
            std::string low_rel = ToLowerCd(rel);
            if (!IsHousekeepingCodegenInputCd(low_rel)) {
                if (std::find(st.inputs.begin(), st.inputs.end(), rel) == st.inputs.end()) {
                    st.inputs.push_back(rel);
                }
            }
        }

        if (!raw_depfile.empty()) {
            st.depfile = MakeBuildRelNormalized(raw_depfile, compdb_dir, project_root);
        }

        std::string sname = TrimCd(raw_desc);
        if (sname.empty()) {
            sname = "codegen: " + std::filesystem::u8path(st.outputs[0]).filename().generic_u8string();
        }
        st.name = sname;

        ImportedCustomStepCandidate cand;
        cand.step = std::move(st);
        results.push_back(std::move(cand));
    }

    return results;
}

std::vector<ImportedCustomStepCandidate> ExtractMakeCustomSteps(
    const std::filesystem::path& compdb_dir,
    const std::filesystem::path& project_root
) {
    std::vector<ImportedCustomStepCandidate> results;
    std::filesystem::path cm_root = compdb_dir / "CMakeFiles";
    if (!fs_util::ExistsLong(cm_root)) return results;

    std::vector<std::pair<std::filesystem::path, std::string>> make_files;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(fs_util::ToWinLongPath(cm_root), ec)) {
        if (ec) break;
        if (!de.is_directory(ec)) continue;
        std::string dname = de.path().filename().generic_u8string();
        if (!EndsWithCiCd(dname, ".dir")) continue;
        std::filesystem::path bm = de.path() / "build.make";
        if (fs_util::ExistsLong(bm)) {
            std::string tname = dname.substr(0, dname.size() - 4);
            make_files.emplace_back(bm, tname);
        }
    }
    std::sort(make_files.begin(), make_files.end(), [](const auto& a, const auto& b) {
        return a.first.generic_u8string() < b.first.generic_u8string();
    });

    auto find_make_colon = [](const std::string& ln) -> size_t {
        for (size_t i = 0; i < ln.size(); ++i) {
            if (ln[i] == ':') {
                // Skip Windows drive letter colons like C:/... or D:\...
                if (i == 1 && std::isalpha(static_cast<unsigned char>(ln[0]))) continue;
                if (i + 1 < ln.size() && (ln[i + 1] == '/' || ln[i + 1] == '\\')) continue;
                return i;
            }
        }
        return std::string::npos;
    };

    for (const auto& [bm_path, dir_target] : make_files) {
        std::string raw;
        if (!fs_util::ReadFileTextLong(bm_path, raw)) continue;

        std::string cmake_cmd = "cmake";
        struct RuleAccum {
            std::string outputRaw;
            std::vector<std::string> depsRaw;
            std::vector<std::string> recipeLines;
        };
        std::vector<std::string> rule_order;
        std::unordered_map<std::string, RuleAccum> rules_by_out;
        std::string current_rule_key;

        std::istringstream iss(raw);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                current_rule_key.clear();
                continue;
            }
            if (line[0] == '#') continue;

            if (line[0] == '\t') {
                if (!current_rule_key.empty()) {
                    std::string cmd_ln = TrimCd(line.substr(1));
                    while (!cmd_ln.empty() && cmd_ln.front() == '@') {
                        cmd_ln.erase(0, 1);
                        cmd_ln = TrimCd(cmd_ln);
                    }
                    if (!cmd_ln.empty() && cmd_ln.find("cmake_echo_color") == std::string::npos &&
                        cmd_ln.find("cmake_progress_report") == std::string::npos) {
                        cmd_ln = ReplaceAllCd(cmd_ln, "$(CMAKE_COMMAND)", cmake_cmd);
                        cmd_ln = UnwrapShellCommandCd(cmd_ln);
                        if (!cmd_ln.empty()) {
                            rules_by_out[current_rule_key].recipeLines.push_back(std::move(cmd_ln));
                        }
                    }
                }
                continue;
            }

            current_rule_key.clear();
            if (line.rfind("CMAKE_COMMAND", 0) == 0) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string v = TrimCd(line.substr(eq + 1));
                    if (!v.empty()) cmake_cmd = v;
                    continue;
                }
            }
            if (line.find('=') != std::string::npos && find_make_colon(line) == std::string::npos) {
                continue;
            }

            size_t colon = find_make_colon(line);
            if (colon == std::string::npos) continue;

            std::string lhs = TrimCd(line.substr(0, colon));
            std::string rhs = TrimCd(line.substr(colon + 1));
            if (lhs.empty() || lhs.find("$(") != std::string::npos || lhs.find('%') != std::string::npos) continue;
            std::string out_rel = MakeBuildRelNormalized(lhs, compdb_dir, project_root);
            if (IsIgnoredCodegenOutputCd(ToLowerCd(out_rel))) continue;

            std::string key = ToLowerCd(out_rel);
            if (rules_by_out.find(key) == rules_by_out.end()) {
                rule_order.push_back(key);
                rules_by_out[key].outputRaw = out_rel;
            }
            current_rule_key = key;

            std::vector<std::string> deps = CompDbManager::SplitCommandLine(rhs);
            for (const auto& d : deps) {
                if (d.find("$(") != std::string::npos) continue;
                std::string dep_rel = MakeBuildRelNormalized(d, compdb_dir, project_root);
                if (!IsHousekeepingCodegenInputCd(ToLowerCd(dep_rel))) {
                    auto& acc = rules_by_out[key];
                    if (std::find(acc.depsRaw.begin(), acc.depsRaw.end(), dep_rel) == acc.depsRaw.end()) {
                        acc.depsRaw.push_back(dep_rel);
                    }
                }
            }
        }

        for (const auto& key : rule_order) {
            const auto& acc = rules_by_out[key];
            if (acc.recipeLines.empty()) continue;

            std::string joined_cmd;
            for (size_t ri = 0; ri < acc.recipeLines.size(); ++ri) {
                if (ri > 0) joined_cmd += " && ";
                joined_cmd += acc.recipeLines[ri];
            }

            BuildStep st;
            st.name = "codegen: " + std::filesystem::u8path(acc.outputRaw).filename().generic_u8string();
            st.phase = StepPhase::PreBuild;
            st.shell = true;
            st.inputs = acc.depsRaw;
            st.outputs = { acc.outputRaw };
            st.command = EnsureBuildCwdCommandCd(joined_cmd, compdb_dir, project_root);
            st.generatesSources = IsStepGeneratedSourcePath(acc.outputRaw);

            ImportedCustomStepCandidate cand;
            cand.step = std::move(st);
            cand.preferredTarget = dir_target;
            results.push_back(std::move(cand));
        }
    }

    return results;
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

    struct ExcludedGenRecord {
        std::string fileRel;
        std::string fileName;
        std::string targetName;
    };
    std::vector<ExcludedGenRecord> excluded_gen_records;
    std::unordered_map<std::string, std::string> gen_tu_target_by_rel;

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
            std::string g_out = entry.value("output", "");
            std::string g_tgt = extract_cmake_target(g_out);
            std::string g_rel = MakeRelFwd(file_abs, project_root);
            if (!g_tgt.empty()) {
                gen_tu_target_by_rel.emplace(ToLowerCd(g_rel), g_tgt);
            }
            excluded_gen_records.push_back(ExcludedGenRecord{
                g_rel,
                file_abs.filename().generic_u8string(),
                g_tgt
            });
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

    // Pre-pass: map each target's produced archive/import-lib filenames (e.g. libglfw3.a -> glfw)
    struct CachedLinkInfo {
        bool valid = false;
        std::vector<std::string> tokens;
        std::filesystem::path entryDir;
        std::filesystem::path cmDir;
    };
    std::unordered_map<std::string, CachedLinkInfo> parsed_links;
    std::unordered_map<std::string, std::string> lib_file_to_target;
    for (const auto& tname : target_order) {
        auto cm_it = cmake_dir_by_target.find(tname);
        if (cm_it == cmake_dir_by_target.end()) continue;
        std::filesystem::path link_txt = cm_it->second.first / "link.txt";
        std::string link_raw;
        if (!fs_util::ReadFileTextLong(link_txt, link_raw)) continue;

        CachedLinkInfo cli;
        cli.valid = true;
        cli.entryDir = cm_it->second.second;
        cli.cmDir = cm_it->second.first;
        std::vector<std::string> link_raw_toks = SplitCommandLine(link_raw);
        ExpandResponseFilesRecursive(link_raw_toks, cli.entryDir, project_root, cli.tokens, rsp_cache);

        for (size_t li = 0; li < cli.tokens.size(); ++li) {
            const std::string& lt = cli.tokens[li];
            std::string f_low = ToLowerCd(std::filesystem::u8path(lt).filename().generic_u8string());
            if ((f_low == "ar" || f_low == "ar.exe" || f_low == "gcc-ar" || f_low == "gcc-ar.exe" ||
                 f_low == "llvm-ar" || f_low == "llvm-ar.exe") && li + 2 < cli.tokens.size()) {
                // Pattern: ar qc <archive> ...
                std::string arch_tok = cli.tokens[li + 2];
                std::string arch_f = ToLowerCd(std::filesystem::u8path(arch_tok).filename().generic_u8string());
                if (arch_f != "objects.a" && (EndsWithCiCd(arch_f, ".a") || EndsWithCiCd(arch_f, ".lib"))) {
                    lib_file_to_target[arch_f] = tname;
                    std::string stem = ToLowerCd(ExtractTargetTokenFromLibPath(arch_tok));
                    if (!stem.empty()) lib_file_to_target[stem] = tname;
                }
            } else if (lt.rfind("-Wl,--out-implib,", 0) == 0) {
                std::string imp = lt.substr(17);
                std::string imp_f = ToLowerCd(std::filesystem::u8path(imp).filename().generic_u8string());
                if (!imp_f.empty()) lib_file_to_target[imp_f] = tname;
            }
        }
        parsed_links[tname] = std::move(cli);
    }

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
        auto pl_it = parsed_links.find(tname);
        if (pl_it != parsed_links.end() && pl_it->second.valid) {
            resolved_from_link_txt = true;
            const auto& link_toks = pl_it->second.tokens;
            const std::filesystem::path& link_entry_dir = pl_it->second.entryDir;

            bool saw_ar = false;
            bool saw_shared = has_shared_hint;
            bool saw_exe_out = false;
            bool saw_lib_out = false;
            std::unordered_set<std::string> seen_deps, seen_libs, seen_lflags, seen_rc_files;

            for (size_t li = 0; li < link_toks.size(); ++li) {
                const std::string& lt = link_toks[li];
                std::string low_lt = lt;
                std::replace(low_lt.begin(), low_lt.end(), '\\', '/');
                low_lt = ToLowerCd(low_lt);
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
                if (lt == "-static" || lt == "-static-libgcc" || lt == "-static-libstdc++" ||
                    lt == "-mwindows" || lt == "-mconsole") {
                    if (seen_lflags.insert(lt).second) ts.linkFlags.push_back(lt);
                }
                // Detect CMake resource object files (e.g. CMakeFiles/foo.dir/assets/app.rc.obj)
                if (EndsWithCiCd(fname_low, ".rc.obj") || EndsWithCiCd(fname_low, ".rc.res")) {
                    std::string norm_rc = lt;
                    std::replace(norm_rc.begin(), norm_rc.end(), '\\', '/');
                    size_t dpos = norm_rc.find(".dir/");
                    if (dpos != std::string::npos) {
                        std::string rc_rel = norm_rc.substr(dpos + 5);
                        if (EndsWithCiCd(rc_rel, ".obj") || EndsWithCiCd(rc_rel, ".res")) {
                            rc_rel.resize(rc_rel.size() - 4);
                        }
                        if (!rc_rel.empty() && fs_util::ExistsLong(project_root / std::filesystem::u8path(rc_rel))) {
                            if (seen_rc_files.insert(rc_rel).second) ts.files.push_back(rc_rel);
                        }
                    }
                }

                // Extract inter-target deps and external/system libs from link.txt / linkLibs.rsp
                if (lt.rfind("-l", 0) == 0 && lt.size() > 2) {
                    std::string lname = lt.substr(2);
                    if (lname != tname && seen_libs.insert(lname).second) {
                        ts.libs.push_back(lname);
                    }
                } else if (!lt.empty() && lt[0] != '-' &&
                           (EndsWithCiCd(lt, ".a") || EndsWithCiCd(lt, ".lib") || EndsWithCiCd(lt, ".dll.a") ||
                            EndsWithCiCd(lt, ".so") || EndsWithCiCd(lt, ".dylib")) &&
                           low_lt.find("cmakefiles/") == std::string::npos &&
                           fname_low != "objects.a") {
                    std::string mapped_tgt;
                    auto mit = lib_file_to_target.find(fname_low);
                    if (mit != lib_file_to_target.end()) {
                        mapped_tgt = mit->second;
                    } else {
                        std::string cand = ExtractTargetTokenFromLibPath(lt);
                        if (all_target_names.count(cand)) {
                            mapped_tgt = cand;
                        } else {
                            auto mit2 = lib_file_to_target.find(ToLowerCd(cand));
                            if (mit2 != lib_file_to_target.end()) mapped_tgt = mit2->second;
                        }
                    }

                    if (!mapped_tgt.empty()) {
                        if (mapped_tgt != tname && seen_deps.insert(mapped_tgt).second) {
                            ts.deps.push_back(mapped_tgt);
                        }
                    } else if (lt.rfind("CMakeFiles", 0) != 0 && fname_low != ("lib" + ToLowerCd(tname) + ".a")) {
                        std::string resolved_lib = resolve_entry_path(lt, link_entry_dir);
                        if (seen_libs.insert(resolved_lib).second) ts.libs.push_back(resolved_lib);
                    }
                }
            }

            if (saw_shared) ts.kind = TargetKind::Shared;
            else if (saw_exe_out) ts.kind = TargetKind::Executable;
            else if (saw_ar || saw_lib_out) ts.kind = TargetKind::Static;
            else ts.kind = TargetKind::Executable;
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
            std::string frel_low = ToLowerCd(frel);
            bool in_skipped_dir = (frel_low.rfind("build/", 0) == 0 || frel_low == "build" ||
                                   frel_low.find("/_deps/") != std::string::npos ||
                                   frel_low.find("/tests/") != std::string::npos || frel_low == "tests");
            if (!in_skipped_dir && !frel.empty() && dit != folder_disk_counts.end() && dit->second > 0 && fg.files.size() == dit->second) {
                fg.useFolderRule = true;
            }
            ts.folders.push_back(std::move(fg));
        }

        plan.targets.push_back(std::move(ts));
    }

    // Import custom codegen BuildSteps (V2-6) from Ninja build.ninja or CMake build.make next to compile_commands.json
    std::unordered_map<std::string, std::string> custom_target_by_output;
    std::vector<ImportedCustomStepCandidate> custom_candidates;
    std::filesystem::path ninja_manifest = compdb_dir / "build.ninja";
    if (fs_util::ExistsLong(ninja_manifest)) {
        custom_candidates = ExtractNinjaCustomSteps(ninja_manifest, compdb_dir, project_root, custom_target_by_output);
    } else if (fs_util::ExistsLong(compdb_dir / "CMakeFiles")) {
        custom_candidates = ExtractMakeCustomSteps(compdb_dir, project_root);
    }

    std::unordered_map<std::string, size_t> target_idx_map;
    for (size_t i = 0; i < plan.targets.size(); ++i) {
        target_idx_map[plan.targets[i].name] = i;
    }

    std::unordered_set<std::string> covered_gen_outputs;
    std::unordered_set<std::string> seen_step_sigs;
    if (!plan.targets.empty()) {
        for (auto& cand : custom_candidates) {
            std::string sig = cand.step.command;
            for (const auto& o : cand.step.outputs) sig += "|" + ToLowerCd(o);
            if (!seen_step_sigs.insert(sig).second) continue;

            size_t owner_idx = 0;
            bool owner_found = false;
            for (const auto& o : cand.step.outputs) {
                std::string o_low = ToLowerCd(o);
                auto git = gen_tu_target_by_rel.find(o_low);
                if (git != gen_tu_target_by_rel.end()) {
                    auto tit = target_idx_map.find(git->second);
                    if (tit != target_idx_map.end()) {
                        owner_idx = tit->second;
                        owner_found = true;
                        break;
                    }
                }
                auto nit = custom_target_by_output.find(o_low);
                if (!owner_found && nit != custom_target_by_output.end()) {
                    auto tit = target_idx_map.find(nit->second);
                    if (tit != target_idx_map.end()) {
                        owner_idx = tit->second;
                        owner_found = true;
                        break;
                    }
                }
            }
            if (!owner_found && !cand.preferredTarget.empty()) {
                auto tit = target_idx_map.find(cand.preferredTarget);
                if (tit != target_idx_map.end()) {
                    owner_idx = tit->second;
                    owner_found = true;
                }
            }

            // Ensure unique step names within the owning target
            auto& dest_steps = plan.targets[owner_idx].steps;
            std::string base_name = cand.step.name;
            int dup_idx = 2;
            while (std::any_of(dest_steps.begin(), dest_steps.end(), [&](const BuildStep& s) { return s.name == cand.step.name; })) {
                cand.step.name = base_name + " (" + std::to_string(dup_idx++) + ")";
            }

            for (const auto& o : cand.step.outputs) {
                covered_gen_outputs.insert(ToLowerCd(o));
            }
            dest_steps.push_back(std::move(cand.step));
            plan.importedStepCount++;
        }
    }

    for (const auto& rec : excluded_gen_records) {
        if (covered_gen_outputs.find(ToLowerCd(rec.fileRel)) != covered_gen_outputs.end()) {
            continue;
        }
        if (plan.warningsList.size() < 40) {
            plan.warningsList.push_back(
                rec.fileName + " generated by original build system; recreate with a build step (V2-6)."
            );
        }
    }

    if (plan.importedStepCount > 0) {
        plan.warningsList.push_back(
            "Imported " + std::to_string(plan.importedStepCount) +
            " custom codegen build step(s); project will require trust confirmation before executing steps."
        );
    }
    plan.warningsList.push_back("Review Target Kinds, Dependencies, and Libraries if the source build used non-CMake tools.");
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
        t.priv.linkFlags = ts.linkFlags;
        t.priv.includeDirs = ts.includeDirs;
        t.priv.systemIncludeDirs = ts.systemIncludeDirs;
        t.priv.defines = ts.defines;
        t.priv.compileFlags = ts.compileFlags;
        t.steps = ts.steps;

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
