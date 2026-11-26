#include "condition.h"
#include "project.h"
#include "toolchain.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace core {

namespace {

std::string ToLowerCond(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string TrimCond(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

size_t CondEditDistance(const std::string& a, const std::string& b) {
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

std::string SuggestConditionIdentifier(const std::string& unknown_lower) {
    static const char* kKnown[] = {
        "windows", "linux", "macos", "none",
        "msvc", "gcc", "clang", "clangcl",
        "x64", "x86", "arm64", "arm"
    };
    std::string best;
    size_t best_dist = 4;
    for (const char* k : kKnown) {
        size_t d = CondEditDistance(unknown_lower, k);
        if (d < best_dist) {
            best_dist = d;
            best = k;
        }
    }
    if (unknown_lower.rfind("cfg:", 0) == 0 || unknown_lower.rfind("conf:", 0) == 0) {
        return "config:<Name>";
    }
    if (unknown_lower.rfind("tc:", 0) == 0 || unknown_lower.rfind("tool:", 0) == 0) {
        return "toolchain:<Name>";
    }
    return best;
}

struct AstNode {
    enum class Kind {
        TrueConst,
        IdentOs,
        IdentCompiler,
        IdentArch,
        IdentConfig,
        IdentToolchain,
        Not,
        And,
        Or
    };

    Kind kind = Kind::TrueConst;
    std::string value;       // normalized lowercase value (for leaf identifiers)
    std::string rawIdent;    // original token text
    std::shared_ptr<const AstNode> left;
    std::shared_ptr<const AstNode> right;

    bool Eval(const ConditionContext& ctx) const {
        switch (kind) {
            case Kind::TrueConst: return true;
            case Kind::IdentOs: return ToLowerCond(ctx.os) == value;
            case Kind::IdentCompiler: return ToLowerCond(ctx.compilerFamily) == value;
            case Kind::IdentArch: return ToLowerCond(ctx.arch) == value;
            case Kind::IdentConfig: return ToLowerCond(ctx.config) == value;
            case Kind::IdentToolchain: return ToLowerCond(ctx.toolchain) == value;
            case Kind::Not: return left ? !left->Eval(ctx) : true;
            case Kind::And: return (left && left->Eval(ctx)) && (right && right->Eval(ctx));
            case Kind::Or: return (left && left->Eval(ctx)) || (right && right->Eval(ctx));
        }
        return false;
    }
};

struct CachedConditionEntry {
    bool valid = true;
    std::string error;
    std::shared_ptr<const AstNode> root;
};

class ConditionParser {
public:
    explicit ConditionParser(const std::string& src) : m_src(src) {}

    bool Parse(std::shared_ptr<const AstNode>& out_root, std::string& out_error) {
        m_pos = 0;
        m_error.clear();
        SkipWs();
        if (m_pos >= m_src.size()) {
            auto node = std::make_shared<AstNode>();
            node->kind = AstNode::Kind::TrueConst;
            out_root = node;
            return true;
        }
        auto root = ParseOr();
        if (!m_error.empty()) {
            out_error = m_error;
            return false;
        }
        SkipWs();
        if (m_pos < m_src.size()) {
            if (m_src[m_pos] == ')') {
                out_error = "Unexpected ')' at position " + std::to_string(m_pos);
            } else {
                out_error = "Unexpected token '" + m_src.substr(m_pos) + "' at position " + std::to_string(m_pos);
            }
            return false;
        }
        out_root = std::move(root);
        return true;
    }

private:
    const std::string& m_src;
    size_t m_pos = 0;
    std::string m_error;

    void SkipWs() {
        while (m_pos < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_pos]))) {
            ++m_pos;
        }
    }

    std::shared_ptr<AstNode> ParseOr() {
        auto lhs = ParseAnd();
        if (!lhs) return nullptr;
        while (true) {
            SkipWs();
            if (m_pos < m_src.size() && m_src[m_pos] == '|') {
                if (m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '|') {
                    m_pos += 2;
                    auto rhs = ParseAnd();
                    if (!rhs) return nullptr;
                    auto node = std::make_shared<AstNode>();
                    node->kind = AstNode::Kind::Or;
                    node->left = std::move(lhs);
                    node->right = std::move(rhs);
                    lhs = std::move(node);
                } else {
                    m_error = "Unexpected '|' at position " + std::to_string(m_pos) + " (did you mean '||'?)";
                    return nullptr;
                }
            } else {
                break;
            }
        }
        return lhs;
    }

    std::shared_ptr<AstNode> ParseAnd() {
        auto lhs = ParseUnary();
        if (!lhs) return nullptr;
        while (true) {
            SkipWs();
            if (m_pos < m_src.size() && m_src[m_pos] == '&') {
                if (m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '&') {
                    m_pos += 2;
                    auto rhs = ParseUnary();
                    if (!rhs) return nullptr;
                    auto node = std::make_shared<AstNode>();
                    node->kind = AstNode::Kind::And;
                    node->left = std::move(lhs);
                    node->right = std::move(rhs);
                    lhs = std::move(node);
                } else {
                    m_error = "Unexpected '&' at position " + std::to_string(m_pos) + " (did you mean '&&'?)";
                    return nullptr;
                }
            } else {
                break;
            }
        }
        return lhs;
    }

    std::shared_ptr<AstNode> ParseUnary() {
        SkipWs();
        if (m_pos < m_src.size() && m_src[m_pos] == '!') {
            ++m_pos;
            auto inner = ParseUnary();
            if (!inner) return nullptr;
            auto node = std::make_shared<AstNode>();
            node->kind = AstNode::Kind::Not;
            node->left = std::move(inner);
            return node;
        }
        return ParsePrimary();
    }

    static bool IsIdentChar(char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_' || c == ':' || c == '-' || c == '.' || c == '+';
    }

    std::shared_ptr<AstNode> ParsePrimary() {
        SkipWs();
        if (m_pos >= m_src.size()) {
            m_error = "Unexpected end of condition expression (expected identifier or '(')";
            return nullptr;
        }

        if (m_src[m_pos] == '(') {
            size_t open_pos = m_pos++;
            SkipWs();
            if (m_pos < m_src.size() && m_src[m_pos] == ')') {
                m_error = "Empty parentheses '()' at position " + std::to_string(open_pos);
                return nullptr;
            }
            auto inner = ParseOr();
            if (!inner) return nullptr;
            SkipWs();
            if (m_pos >= m_src.size() || m_src[m_pos] != ')') {
                m_error = "Unclosed '(' opened at position " + std::to_string(open_pos);
                return nullptr;
            }
            ++m_pos;
            return inner;
        }

        if (!IsIdentChar(m_src[m_pos])) {
            m_error = std::string("Unexpected character '") + m_src[m_pos] + "' at position " + std::to_string(m_pos);
            return nullptr;
        }

        size_t start = m_pos;
        while (m_pos < m_src.size() && IsIdentChar(m_src[m_pos])) {
            // Support quoted value right after "config:" or "toolchain:", e.g. toolchain:"MSVC 2022 x64"
            if (m_src[m_pos] == ':' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '"') {
                m_pos += 2;
                while (m_pos < m_src.size() && m_src[m_pos] != '"') {
                    ++m_pos;
                }
                if (m_pos >= m_src.size()) {
                    m_error = "Unterminated quote in condition identifier";
                    return nullptr;
                }
                ++m_pos; // consume closing '"'
                break;
            }
            ++m_pos;
        }

        std::string raw_tok = m_src.substr(start, m_pos - start);
        std::string low = ToLowerCond(raw_tok);

        auto node = std::make_shared<AstNode>();
        node->rawIdent = raw_tok;

        if (low == "windows" || low == "linux" || low == "macos" || low == "none") {
            node->kind = AstNode::Kind::IdentOs;
            node->value = low;
            return node;
        }
        if (low == "msvc" || low == "gcc" || low == "clang" || low == "clangcl") {
            node->kind = AstNode::Kind::IdentCompiler;
            node->value = low;
            return node;
        }
        if (low == "x64" || low == "x86" || low == "arm64" || low == "arm") {
            node->kind = AstNode::Kind::IdentArch;
            node->value = low;
            return node;
        }

        auto extract_prefixed = [&](const std::string& prefix, std::string& out_val) -> bool {
            if (low.rfind(prefix, 0) != 0) return false;
            std::string val = raw_tok.substr(prefix.size());
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            out_val = ToLowerCond(val);
            return true;
        };

        std::string val;
        if (extract_prefixed("config:", val)) {
            if (val.empty()) {
                m_error = "Expected configuration name after 'config:'";
                return nullptr;
            }
            node->kind = AstNode::Kind::IdentConfig;
            node->value = std::move(val);
            return node;
        }
        if (extract_prefixed("toolchain:", val)) {
            if (val.empty()) {
                m_error = "Expected toolchain name after 'toolchain:'";
                return nullptr;
            }
            node->kind = AstNode::Kind::IdentToolchain;
            node->value = std::move(val);
            return node;
        }

        std::string sug = SuggestConditionIdentifier(low);
        m_error = "Unknown condition identifier '" + raw_tok + "'";
        if (!sug.empty()) {
            m_error += " (did you mean '" + sug + "'?)";
        }
        return nullptr;
    }
};

std::mutex g_cond_cache_mutex;
std::unordered_map<std::string, CachedConditionEntry> g_cond_cache;

CachedConditionEntry GetOrParseCondition(const std::string& expr) {
    {
        std::lock_guard<std::mutex> lock(g_cond_cache_mutex);
        auto it = g_cond_cache.find(expr);
        if (it != g_cond_cache.end()) {
            return it->second;
        }
    }

    CachedConditionEntry entry;
    ConditionParser parser(expr);
    entry.valid = parser.Parse(entry.root, entry.error);

    {
        std::lock_guard<std::mutex> lock(g_cond_cache_mutex);
        if (g_cond_cache.size() > 4096) {
            g_cond_cache.clear();
        }
        g_cond_cache[expr] = entry;
    }
    return entry;
}

} // namespace

ConditionContext BuildConditionContext(const Project& project, const ToolchainProfile* active_profile) {
    ConditionContext ctx;
#if defined(_WIN32)
    ctx.os = "windows";
#elif defined(__APPLE__)
    ctx.os = "macos";
#else
    ctx.os = "linux";
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
    ctx.arch = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    ctx.arch = "x86";
#elif defined(__arm__)
    ctx.arch = "arm";
#else
    ctx.arch = "x64";
#endif

    if (active_profile) {
        if (!active_profile->os.empty()) ctx.os = active_profile->os;
        if (!active_profile->arch.empty()) ctx.arch = active_profile->arch;
        ctx.compilerFamily = ToolchainFamilyToString(active_profile->family);
        ctx.toolchain = !active_profile->name.empty() ? active_profile->name : ctx.compilerFamily;
    } else {
        std::string comp = project.GetToolchain().empty() ? project.GetCompiler() : project.GetToolchain();
        std::string comp_low = ToLowerCond(comp);
        if (comp_low == "cl" || comp_low == "cl.exe" || comp_low.rfind("msvc", 0) == 0) {
            ctx.compilerFamily = "msvc";
        } else if (comp_low.find("clang-cl") != std::string::npos || comp_low == "clangcl") {
            ctx.compilerFamily = "clangcl";
        } else if (comp_low.find("clang") != std::string::npos) {
            ctx.compilerFamily = "clang";
        } else {
            ctx.compilerFamily = "gcc";
        }
        ctx.toolchain = comp.empty() ? ctx.compilerFamily : comp;
    }

    ctx.config = project.GetConfiguration();
    return ctx;
}

bool ValidateCondition(const std::string& expr, std::string& out_error) {
    out_error.clear();
    std::string trimmed = TrimCond(expr);
    if (trimmed.empty()) return true;
    CachedConditionEntry entry = GetOrParseCondition(trimmed);
    if (!entry.valid) {
        out_error = entry.error;
        return false;
    }
    return true;
}

ConditionEvalResult EvaluateCondition(const std::string& expr, const ConditionContext& ctx) {
    ConditionEvalResult res;
    std::string trimmed = TrimCond(expr);
    if (trimmed.empty()) {
        res.active = true;
        res.valid = true;
        return res;
    }

    CachedConditionEntry entry = GetOrParseCondition(trimmed);
    if (!entry.valid) {
        res.active = false;
        res.valid = false;
        res.error = entry.error;
        res.reason = "invalid condition: " + entry.error;
        return res;
    }

    res.valid = true;
    res.active = entry.root ? entry.root->Eval(ctx) : true;
    if (!res.active) {
        std::string ctx_desc = !ctx.toolchain.empty() ? ctx.toolchain : ctx.compilerFamily;
        if (!ctx.arch.empty()) {
            ctx_desc += " " + ctx.arch;
        }
        if (entry.root && !entry.root->rawIdent.empty()) {
            res.reason = "inactive: `" + entry.root->rawIdent + "` is false for " + ctx_desc;
        } else {
            res.reason = "inactive: `" + trimmed + "` is false for " + ctx_desc;
            if (!ctx.config.empty()) {
                res.reason += " (" + ctx.config + ")";
            }
        }
    }
    return res;
}

} // namespace core
