#include "source_rules.h"
#include "project.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace core {

static std::string ToLowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string NormalizeRelPath(const std::string& p) {
    std::string r = p;
    std::replace(r.begin(), r.end(), '\\', '/');
    while (r.rfind("./", 0) == 0) {
        r = r.substr(2);
    }
    while (!r.empty() && r.front() == '/') {
        r.erase(r.begin());
    }
    while (r.size() > 1 && r.back() == '/') {
        r.pop_back();
    }
    // Collapse double slashes
    std::string out;
    out.reserve(r.size());
    bool prev_slash = false;
    for (char c : r) {
        if (c == '/') {
            if (!prev_slash) out.push_back(c);
            prev_slash = true;
        } else {
            out.push_back(c);
            prev_slash = false;
        }
    }
    return out;
}

static std::vector<std::string> SplitSegments(const std::string& path) {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start < path.size()) {
        size_t pos = path.find('/', start);
        if (pos == std::string::npos) {
            std::string seg = path.substr(start);
            if (!seg.empty()) segs.push_back(std::move(seg));
            break;
        }
        std::string seg = path.substr(start, pos - start);
        if (!seg.empty()) segs.push_back(std::move(seg));
        start = pos + 1;
    }
    return segs;
}

bool IsCompilableSourceExtension(const std::string& ext_lower) {
    return (ext_lower == ".cpp" || ext_lower == ".c" || ext_lower == ".cc" || ext_lower == ".cxx");
}

bool IsHeaderExtension(const std::string& ext_lower) {
    return (ext_lower == ".h" || ext_lower == ".hpp" || ext_lower == ".hh" || ext_lower == ".hxx" || ext_lower == ".inl");
}

// -----------------------------------------------------------------------------
// CompiledGlob Implementation
// -----------------------------------------------------------------------------

std::vector<std::string> CompiledGlob::ExpandBraces(const std::string& pattern) {
    // Find first top-level '{...}'
    size_t open = std::string::npos;
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{') {
            if (depth == 0) open = i;
            depth++;
        } else if (pattern[i] == '}' && depth > 0) {
            depth--;
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }

    if (open == std::string::npos || close == std::string::npos) {
        return { pattern };
    }

    std::string prefix = pattern.substr(0, open);
    std::string inner = pattern.substr(open + 1, close - open - 1);
    std::string suffix = pattern.substr(close + 1);

    // Split inner on top-level commas
    std::vector<std::string> alts;
    int inner_depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '{') inner_depth++;
        else if (inner[i] == '}' && inner_depth > 0) inner_depth--;
        else if (inner[i] == ',' && inner_depth == 0) {
            alts.push_back(inner.substr(start, i - start));
            start = i + 1;
        }
    }
    alts.push_back(inner.substr(start));

    std::vector<std::string> result;
    for (const auto& alt : alts) {
        std::vector<std::string> sub = ExpandBraces(prefix + alt + suffix);
        result.insert(result.end(), sub.begin(), sub.end());
    }
    return result;
}

bool CompiledGlob::MatchSegment(const std::string& pattern_seg, const std::string& text_seg, bool case_insensitive) {
    size_t pi = 0, ti = 0;
    size_t star_pi = std::string::npos;
    size_t star_ti = 0;

    auto eq = [case_insensitive](char a, char b) {
        if (case_insensitive) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        }
        return a == b;
    };

    while (ti < text_seg.size()) {
        if (pi < pattern_seg.size() && (pattern_seg[pi] == '?' || eq(pattern_seg[pi], text_seg[ti]))) {
            ++pi;
            ++ti;
        } else if (pi < pattern_seg.size() && pattern_seg[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
        } else {
            return false;
        }
    }

    while (pi < pattern_seg.size() && pattern_seg[pi] == '*') {
        ++pi;
    }
    return pi == pattern_seg.size();
}

CompiledGlob CompiledGlob::Compile(const std::string& pattern) {
    CompiledGlob g;
    g.m_pattern = NormalizeRelPath(pattern);
#if defined(_WIN32)
    g.m_case_insensitive = true;
#else
    g.m_case_insensitive = false;
#endif
    if (g.m_pattern.empty()) {
        return g;
    }

    std::vector<std::string> expanded = ExpandBraces(g.m_pattern);
    for (const auto& raw_var : expanded) {
        std::string norm_var = NormalizeRelPath(raw_var);
        if (norm_var.empty()) continue;

        auto raw_segs = SplitSegments(norm_var);
        if (raw_segs.empty()) continue;

        std::vector<Segment> compiled_segs;
        compiled_segs.reserve(raw_segs.size());

        for (size_t i = 0; i < raw_segs.size(); ++i) {
            const std::string& s = raw_segs[i];
            Segment seg;
            if (s == "**") {
                seg.kind = SegKind::GlobStar;
                seg.text = "**";
                // Collapse consecutive "**"
                if (!compiled_segs.empty() && compiled_segs.back().kind == SegKind::GlobStar) {
                    continue;
                }
            } else if (s.find('*') != std::string::npos || s.find('?') != std::string::npos) {
                seg.kind = SegKind::Wildcard;
                seg.text = s;
                seg.text_lower = ToLowerStr(s);
            } else {
                seg.kind = SegKind::Literal;
                seg.text = s;
                seg.text_lower = ToLowerStr(s);
                if (i + 1 < raw_segs.size()) {
                    g.m_literal_dir_names.insert(seg.text_lower);
                }
            }
            compiled_segs.push_back(std::move(seg));
        }

        if (!compiled_segs.empty()) {
            g.m_variants.push_back(std::move(compiled_segs));
        }
    }

    // Compute longest literal prefix common to all brace-expanded variants
    if (!g.m_variants.empty()) {
        const auto& first = g.m_variants[0];
        size_t common_lit = 0;
        // Stop before the last segment (unless it's a directory? A file pattern's last segment is filename or **)
        while (common_lit + 1 < first.size() && first[common_lit].kind == SegKind::Literal) {
            bool all_same = true;
            for (size_t v = 1; v < g.m_variants.size(); ++v) {
                const auto& var = g.m_variants[v];
                if (common_lit + 1 >= var.size() || var[common_lit].kind != SegKind::Literal) {
                    all_same = false;
                    break;
                }
                if (g.m_case_insensitive) {
                    if (var[common_lit].text_lower != first[common_lit].text_lower) {
                        all_same = false;
                        break;
                    }
                } else {
                    if (var[common_lit].text != first[common_lit].text) {
                        all_same = false;
                        break;
                    }
                }
            }
            if (!all_same) break;
            ++common_lit;
        }

        std::string prefix;
        for (size_t i = 0; i < common_lit; ++i) {
            if (!prefix.empty()) prefix += "/";
            prefix += first[i].text;
        }
        g.m_longest_literal_prefix = prefix;
    }

    return g;
}

bool CompiledGlob::ExplicitlyNamesDir(const std::string& dir_name) const {
    return m_literal_dir_names.find(ToLowerStr(dir_name)) != m_literal_dir_names.end();
}

bool CompiledGlob::Matches(const std::string& rel_path) const {
    if (m_variants.empty()) return false;
    std::string norm = NormalizeRelPath(rel_path);
    if (norm.empty()) return false;

    std::vector<std::string> path_segs = SplitSegments(norm);
    if (path_segs.empty()) return false;

    for (const auto& pat_segs : m_variants) {
        const size_t P = pat_segs.size();
        const size_t T = path_segs.size();

        // DP matching over path segments (P and T are small, typically < 20)
        // dp[j] = whether pat_segs[0..i-1] matches path_segs[0..j-1]
        std::vector<uint8_t> dp(T + 1, 0), next_dp(T + 1, 0);
        dp[0] = 1;

        for (size_t i = 0; i < P; ++i) {
            std::fill(next_dp.begin(), next_dp.end(), 0);
            const auto& ps = pat_segs[i];

            if (ps.kind == SegKind::GlobStar) {
                bool seen_true = false;
                for (size_t j = 0; j <= T; ++j) {
                    if (dp[j]) seen_true = true;
                    if (seen_true) next_dp[j] = 1;
                }
            } else if (ps.kind == SegKind::Literal) {
                for (size_t j = 0; j < T; ++j) {
                    if (!dp[j]) continue;
                    bool eq = m_case_insensitive
                        ? (ToLowerStr(path_segs[j]) == ps.text_lower)
                        : (path_segs[j] == ps.text);
                    if (eq) next_dp[j + 1] = 1;
                }
            } else {
                for (size_t j = 0; j < T; ++j) {
                    if (!dp[j]) continue;
                    if (MatchSegment(ps.text, path_segs[j], m_case_insensitive)) {
                        next_dp[j + 1] = 1;
                    }
                }
            }
            dp.swap(next_dp);
        }

        if (dp[T]) {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// DirectoryCache Implementation
// -----------------------------------------------------------------------------

void DirectoryCache::MarkDirty(const std::string& rel_dir) {
    std::string norm = NormalizeRelPath(rel_dir);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_dirs.find(norm);
    if (it != m_dirs.end()) {
        it->second.dirty = true;
    }
}

void DirectoryCache::MarkAllDirty() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& kv : m_dirs) {
        kv.second.dirty = true;
    }
}

void DirectoryCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dirs.clear();
}

bool DirectoryCache::HasDirty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& kv : m_dirs) {
        if (kv.second.dirty) return true;
    }
    return false;
}

static std::vector<CachedDirEntry> EnumerateDirectoryOS(
    const std::filesystem::path& dir_path,
    bool follow_links
) {
    std::vector<CachedDirEntry> result;
#if defined(_WIN32)
    std::wstring search_w = dir_path.wstring();
    if (!search_w.empty() && search_w.back() != L'\\' && search_w.back() != L'/') {
        search_w += L'\\';
    }
    search_w += L'*';

    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileExW(
        search_w.c_str(),
        FindExInfoBasic,
        &ffd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH
    );
    if (hFind == INVALID_HANDLE_VALUE) {
        return result;
    }

    do {
        const wchar_t* wname = ffd.cFileName;
        if (wname[0] == L'.' && (wname[1] == L'\0' || (wname[1] == L'.' && wname[2] == L'\0'))) {
            continue;
        }
        bool is_reparse = (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        bool is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_reparse && is_dir && !follow_links) {
            continue;
        }

        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
        if (utf8_len <= 1) continue;
        std::string utf8_name(static_cast<size_t>(utf8_len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, utf8_name.data(), utf8_len, nullptr, nullptr);

        CachedDirEntry e;
        e.name = std::move(utf8_name);
        e.is_directory = is_dir;
        e.is_symlink = is_reparse;
        result.push_back(std::move(e));
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
#else
    std::error_code ec;
    std::filesystem::directory_iterator it(dir_path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return result;
    for (const auto& entry : it) {
        std::error_code eec;
        auto sym_st = entry.symlink_status(eec);
        if (eec) continue;
        bool is_sym = std::filesystem::is_symlink(sym_st);
        auto st = follow_links ? entry.status(eec) : sym_st;
        if (eec) continue;
        bool is_dir = std::filesystem::is_directory(st);
        if (is_sym && is_dir && !follow_links) {
            continue;
        }
        CachedDirEntry e;
        e.name = entry.path().filename().generic_u8string();
        e.is_directory = is_dir;
        e.is_symlink = is_sym;
        result.push_back(std::move(e));
    }
#endif
    return result;
}

const std::vector<CachedDirEntry>& DirectoryCache::GetEntries(
    const std::filesystem::path& root,
    const std::string& rel_dir,
    bool follow_links
) {
    std::string norm = NormalizeRelPath(rel_dir);
    std::filesystem::path full_dir = norm.empty() ? root : (root / std::filesystem::u8path(norm));

    std::error_code ec;
    auto disk_mtime = std::filesystem::last_write_time(full_dir, ec);
    if (ec) {
        static const std::vector<CachedDirEntry> s_empty;
        return s_empty;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_dirs.find(norm);
        if (it != m_dirs.end() && !it->second.dirty && it->second.mtime == disk_mtime) {
            return it->second.entries;
        }
    }

    std::vector<CachedDirEntry> fresh = EnumerateDirectoryOS(full_dir, follow_links);

    std::lock_guard<std::mutex> lock(m_mutex);
    CachedDirectory& cd = m_dirs[norm];
    cd.mtime = disk_mtime;
    cd.dirty = false;
    cd.entries = std::move(fresh);
    return cd.entries;
}

// -----------------------------------------------------------------------------
// Simple Condition Evaluator
// -----------------------------------------------------------------------------

bool EvaluateSimpleCondition(const std::string& expr, const std::string& os_name, const std::string& compiler_family) {
    std::string s = ToLowerStr(expr);
    // Trim
    auto trim = [](std::string& str) {
        while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.erase(str.begin());
        while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.pop_back();
    };
    trim(s);
    if (s.empty()) return true;

    struct Parser {
        const std::string& s;
        size_t pos = 0;
        std::string os;
        std::string comp;

        void skip_ws() {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        bool parse_or() {
            bool lhs = parse_and();
            while (true) {
                skip_ws();
                if (pos + 1 < s.size() && s[pos] == '|' && s[pos + 1] == '|') {
                    pos += 2;
                    bool rhs = parse_and();
                    lhs = lhs || rhs;
                } else break;
            }
            return lhs;
        }
        bool parse_and() {
            bool lhs = parse_unary();
            while (true) {
                skip_ws();
                if (pos + 1 < s.size() && s[pos] == '&' && s[pos + 1] == '&') {
                    pos += 2;
                    bool rhs = parse_unary();
                    lhs = lhs && rhs;
                } else break;
            }
            return lhs;
        }
        bool parse_unary() {
            skip_ws();
            if (pos < s.size() && s[pos] == '!') {
                ++pos;
                return !parse_unary();
            }
            return parse_primary();
        }
        bool parse_primary() {
            skip_ws();
            if (pos < s.size() && s[pos] == '(') {
                ++pos;
                bool v = parse_or();
                skip_ws();
                if (pos < s.size() && s[pos] == ')') ++pos;
                return v;
            }
            size_t start = pos;
            while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_' || s[pos] == ':' || s[pos] == '-')) {
                ++pos;
            }
            std::string tok = s.substr(start, pos - start);
            if (tok == "windows" || tok == "linux" || tok == "macos") return tok == os;
            if (tok == "msvc" || tok == "gcc" || tok == "clang" || tok == "clangcl") return tok == comp;
            if (tok == "x64") return true;
            return true;
        }
    };

    Parser p{ s, 0, ToLowerStr(os_name), ToLowerStr(compiler_family) };
    return p.parse_or();
}

// -----------------------------------------------------------------------------
// Single-walk ExpandRules & PreviewRule & Simplify
// -----------------------------------------------------------------------------

static bool IsPrefixOrEqualDir(const std::string& parent, const std::string& child) {
    if (parent.empty()) return true;
    std::string p_low = ToLowerStr(parent);
    std::string c_low = ToLowerStr(child);
    if (p_low == c_low) return true;
    return c_low.size() > p_low.size() && c_low.rfind(p_low + "/", 0) == 0;
}

static std::vector<std::string> ComputeMergedWalkRoots(const std::vector<std::string>& raw_prefixes) {
    std::vector<std::string> unique;
    for (const auto& p : raw_prefixes) {
        std::string norm = NormalizeRelPath(p);
        if (std::find(unique.begin(), unique.end(), norm) == unique.end()) {
            unique.push_back(norm);
        }
    }
    // Sort shorter (shallower) prefixes first
    std::sort(unique.begin(), unique.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });

    std::vector<std::string> merged;
    for (const auto& candidate : unique) {
        bool covered = false;
        for (const auto& kept : merged) {
            if (IsPrefixOrEqualDir(kept, candidate)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            merged.push_back(candidate);
        }
    }
    return merged;
}

RuleExpansionResult ExpandRules(
    const std::filesystem::path& root,
    const std::vector<TargetRuleInput>& targets,
    DirectoryCache& cache
) {
    RuleExpansionResult result;
    if (root.empty()) return result;

    struct CompiledRuleInfo {
        size_t rule_idx = 0;
        bool active = true;
        bool follow_links = false;
        CompiledGlob include;
        std::vector<CompiledGlob> excludes;
        std::string walk_prefix;
    };

    struct CompiledTargetInfo {
        std::string name;
        std::vector<CompiledRuleInfo> rules;
        std::vector<CompiledGlob> target_excludes;
        std::vector<std::string> explicit_files;
    };

    std::vector<CompiledTargetInfo> compiled_targets;
    compiled_targets.reserve(targets.size());

    std::vector<std::string> all_prefixes;
    std::unordered_set<std::string> literal_dir_overrides;

    for (const auto& t : targets) {
        CompiledTargetInfo ct;
        ct.name = t.name;
        ct.explicit_files = t.explicit_files;

        for (const auto& ex : t.target_excludes) {
            CompiledGlob cg = CompiledGlob::Compile(ex);
            if (cg.IsValid()) ct.target_excludes.push_back(std::move(cg));
        }

        TargetRuleOutput& out = result.per_target[t.name];
        out.rule_match_counts.assign(t.rules.size(), 0);

        for (size_t ri = 0; ri < t.rules.size(); ++ri) {
            const auto& r = t.rules[ri];
            CompiledRuleInfo cr;
            cr.rule_idx = ri;
            cr.follow_links = r.follow_links;
            cr.active = r.condition.empty() ? true : EvaluateSimpleCondition(r.condition, t.condition_os, t.condition_compiler);
            cr.include = CompiledGlob::Compile(r.include);
            for (const auto& ex : r.exclude) {
                CompiledGlob cg = CompiledGlob::Compile(ex);
                if (cg.IsValid()) cr.excludes.push_back(std::move(cg));
            }
            cr.walk_prefix = cr.include.LongestLiteralPrefix();

            if (cr.active && cr.include.IsValid()) {
                all_prefixes.push_back(cr.walk_prefix);
                // Any directory literally named in the rule bypasses ShouldSkipDirectory
                auto segs = SplitSegments(cr.walk_prefix);
                for (const auto& s : segs) {
                    literal_dir_overrides.insert(ToLowerStr(s));
                }
            }
            ct.rules.push_back(std::move(cr));
        }
        compiled_targets.push_back(std::move(ct));
    }

    // Track matched files per target (deduped)
    std::unordered_map<std::string, std::unordered_set<std::string>> matched_sources_set;
    std::unordered_map<std::string, std::unordered_set<std::string>> matched_headers_set;

    if (!all_prefixes.empty()) {
        std::vector<std::string> walk_roots = ComputeMergedWalkRoots(all_prefixes);
        std::unordered_set<std::string> visited_dirs;

        auto should_descend = [&](const std::string& dir_name, const std::string& rel_sub) -> bool {
            // Check if any active rule literally names this directory in its prefix or path
            for (const auto& ct : compiled_targets) {
                for (const auto& cr : ct.rules) {
                    if (!cr.active || !cr.include.IsValid()) continue;
                    if (IsPrefixOrEqualDir(rel_sub, cr.walk_prefix) || cr.include.ExplicitlyNamesDir(dir_name)) {
                        return true;
                    }
                }
            }
            return !Project::ShouldSkipDirectory(dir_name);
        };

        auto walk_dir = [&](auto& self, const std::string& rel_dir) -> void {
            std::string norm_dir = NormalizeRelPath(rel_dir);
            if (!visited_dirs.insert(ToLowerStr(norm_dir)).second) {
                return;
            }

            const auto& entries = cache.GetEntries(root, norm_dir, false);
            for (const auto& entry : entries) {
                std::string rel_entry = norm_dir.empty() ? entry.name : (norm_dir + "/" + entry.name);
                if (entry.is_directory) {
                    if (should_descend(entry.name, rel_entry)) {
                        self(self, rel_entry);
                    }
                } else {
                    std::string ext;
                    size_t dot = entry.name.find_last_of('.');
                    if (dot != std::string::npos) {
                        ext = ToLowerStr(entry.name.substr(dot));
                    }
                    bool is_src = IsCompilableSourceExtension(ext);
                    bool is_hdr = !is_src && IsHeaderExtension(ext);
                    if (!is_src && !is_hdr) continue;

                    for (const auto& ct : compiled_targets) {
                        bool target_excluded = false;
                        for (const auto& tex : ct.target_excludes) {
                            if (tex.Matches(rel_entry)) {
                                target_excluded = true;
                                break;
                            }
                        }
                        if (target_excluded) continue;

                        bool any_rule_matched = false;
                        for (const auto& cr : ct.rules) {
                            if (!cr.active || !cr.include.IsValid()) continue;
                            if (!cr.include.Matches(rel_entry)) continue;

                            bool rule_excluded = false;
                            for (const auto& rex : cr.excludes) {
                                if (rex.Matches(rel_entry)) {
                                    rule_excluded = true;
                                    break;
                                }
                            }
                            if (!rule_excluded) {
                                if (is_src) {
                                    result.per_target[ct.name].rule_match_counts[cr.rule_idx]++;
                                    any_rule_matched = true;
                                } else {
                                    matched_headers_set[ct.name].insert(rel_entry);
                                }
                            }
                        }

                        if (any_rule_matched && is_src) {
                            matched_sources_set[ct.name].insert(rel_entry);
                        }
                    }
                }
            }
        };

        for (const auto& wr : walk_roots) {
            std::error_code ec;
            std::filesystem::path check_p = wr.empty() ? root : (root / std::filesystem::u8path(wr));
            if (std::filesystem::exists(check_p, ec) && std::filesystem::is_directory(check_p, ec)) {
                walk_dir(walk_dir, wr);
            }
        }
    }

    // Union explicit files (explicit beats pattern!) and sort deterministically
    std::unordered_map<std::string, std::vector<std::string>> file_to_targets;
    for (const auto& ct : compiled_targets) {
        auto& src_set = matched_sources_set[ct.name];
        for (const auto& exp : ct.explicit_files) {
            std::string norm = NormalizeRelPath(exp);
            if (!norm.empty()) {
                src_set.insert(norm);
            }
        }

        TargetRuleOutput& out = result.per_target[ct.name];
        out.source_files.assign(src_set.begin(), src_set.end());
        std::sort(out.source_files.begin(), out.source_files.end());

        auto& hdr_set = matched_headers_set[ct.name];
        out.header_files.assign(hdr_set.begin(), hdr_set.end());
        std::sort(out.header_files.begin(), out.header_files.end());

        for (const auto& f : out.source_files) {
            file_to_targets[f].push_back(ct.name);
        }
    }

    for (auto& kv : file_to_targets) {
        if (kv.second.size() > 1) {
            result.multi_target_files[kv.first] = std::move(kv.second);
        }
    }

    return result;
}

RulePreviewResult PreviewRule(
    const std::filesystem::path& root,
    const SourceRule& rule,
    const std::vector<std::string>& target_excludes,
    DirectoryCache& cache,
    size_t max_preview
) {
    RulePreviewResult preview;
    if (root.empty() || rule.include.empty()) return preview;

    TargetRuleInput input;
    input.name = "__preview__";
    input.rules.push_back(rule);
    input.rules[0].condition.clear(); // preview regardless of condition
    input.target_excludes = target_excludes;

    RuleExpansionResult res = ExpandRules(root, { input }, cache);
    const auto& files = res.per_target["__preview__"].source_files;
    preview.total_count = files.size();
    size_t count = std::min(max_preview, files.size());
    preview.preview_files.assign(files.begin(), files.begin() + count);
    return preview;
}

std::vector<SimplifySuggestion> ComputeSimplifySuggestions(
    const std::filesystem::path& root,
    const std::vector<std::string>& explicit_files,
    DirectoryCache& cache
) {
    std::vector<SimplifySuggestion> suggestions;
    if (root.empty() || explicit_files.size() <= 50) {
        return suggestions;
    }

    // Group explicit files by top-level (or parent) folder
    std::map<std::string, std::set<std::string>> by_folder;
    for (const auto& f : explicit_files) {
        std::string norm = NormalizeRelPath(f);
        size_t slash = norm.find('/');
        if (slash != std::string::npos) {
            std::string top = norm.substr(0, slash);
            by_folder[top].insert(norm);
        }
    }

    for (const auto& [folder, folder_files] : by_folder) {
        if (folder_files.size() < 10) continue;

        std::string pattern = folder + "/**/*.{c,cc,cpp,cxx}";
        SourceRule rule;
        rule.include = pattern;
        RulePreviewResult prev = PreviewRule(root, rule, {}, cache, 100000);
        std::set<std::string> rule_set(prev.preview_files.begin(), prev.preview_files.end());

        if (rule_set == folder_files && !rule_set.empty()) {
            SimplifySuggestion sug;
            sug.folder_rel = folder;
            sug.suggested_include = pattern;
            sug.replaced_files.assign(folder_files.begin(), folder_files.end());
            sug.exact_match = true;
            suggestions.push_back(std::move(sug));
        }
    }
    return suggestions;
}

// -----------------------------------------------------------------------------
// FileWatcher Implementation
// -----------------------------------------------------------------------------

FileWatcher::~FileWatcher() {
    Stop();
}

void FileWatcher::Start(const std::filesystem::path& root, DirectoryCache* cache) {
    Stop();
    if (root.empty() || !cache) return;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    m_root = root;
    m_cache = cache;
    m_stop_requested.store(false);
    m_pending_changes.store(false);
#if defined(_WIN32)
    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
    m_running.store(true);
    m_thread = std::thread(&FileWatcher::WatcherLoop, this);
}

void FileWatcher::Stop() {
    if (!m_running.load() && !m_thread.joinable()) {
#if defined(_WIN32)
        if (m_stop_event) {
            CloseHandle(static_cast<HANDLE>(m_stop_event));
            m_stop_event = nullptr;
        }
#endif
        return;
    }

    m_stop_requested.store(true);
#if defined(_WIN32)
    if (m_stop_event) {
        SetEvent(static_cast<HANDLE>(m_stop_event));
    }
#endif
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
#if defined(_WIN32)
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
#endif
}

bool FileWatcher::ConsumePendingChanges() {
    return m_pending_changes.exchange(false);
}

void FileWatcher::WatcherLoop() {
#if defined(_WIN32)
    HANDLE hDir = CreateFileW(
        m_root.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );
    if (hDir == INVALID_HANDLE_VALUE) {
        m_running.store(false);
        return;
    }

    HANDLE hIoEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hIoEvent) {
        CloseHandle(hDir);
        m_running.store(false);
        return;
    }

    std::vector<BYTE> buffer(64 * 1024);
    bool debounce_active = false;
    auto last_change_time = std::chrono::steady_clock::now();

    while (!m_stop_requested.load()) {
        OVERLAPPED ov{};
        ov.hEvent = hIoEvent;
        ResetEvent(hIoEvent);

        DWORD bytes_returned = 0;
        BOOL ok = ReadDirectoryChangesW(
            hDir,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
            &bytes_returned,
            &ov,
            nullptr
        );
        if (!ok) {
            break;
        }

        HANDLE handles[2] = { static_cast<HANDLE>(m_stop_event), hIoEvent };
        while (!m_stop_requested.load()) {
            DWORD wait_ms = debounce_active ? 100 : INFINITE;
            DWORD res = WaitForMultipleObjects(2, handles, FALSE, wait_ms);

            if (res == WAIT_OBJECT_0) {
                CancelIo(hDir);
                CloseHandle(hIoEvent);
                CloseHandle(hDir);
                return;
            }

            if (res == WAIT_OBJECT_0 + 1) {
                DWORD transferred = 0;
                if (GetOverlappedResult(hDir, &ov, &transferred, FALSE) && transferred > 0) {
                    const BYTE* ptr = buffer.data();
                    while (true) {
                        const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
                        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, fni->FileName, fni->FileNameLength / sizeof(WCHAR), nullptr, 0, nullptr, nullptr);
                        if (utf8_len > 0) {
                            std::string rel(static_cast<size_t>(utf8_len), '\0');
                            WideCharToMultiByte(CP_UTF8, 0, fni->FileName, fni->FileNameLength / sizeof(WCHAR), rel.data(), utf8_len, nullptr, nullptr);
                            std::string norm = NormalizeRelPath(rel);

                            // Ignore .geniebuild and .git churn
                            if (norm.rfind(".geniebuild", 0) != 0 && norm.rfind(".git", 0) != 0) {
                                std::filesystem::path p(norm);
                                std::string parent = p.parent_path().generic_u8string();
                                m_cache->MarkDirty(parent);
                                // Also mark self if it could be a directory rename/delete
                                m_cache->MarkDirty(norm);
                                debounce_active = true;
                                last_change_time = std::chrono::steady_clock::now();
                            }
                        }
                        if (fni->NextEntryOffset == 0) break;
                        ptr += fni->NextEntryOffset;
                    }
                }
                break; // Re-arm ReadDirectoryChangesW
            }

            if (res == WAIT_TIMEOUT && debounce_active) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_change_time
                ).count();
                if (elapsed >= 300) {
                    debounce_active = false;
                    m_pending_changes.store(true);
                }
            }
        }
    }

    CancelIo(hDir);
    CloseHandle(hIoEvent);
    CloseHandle(hDir);
#else
    while (!m_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif
}

} // namespace core
