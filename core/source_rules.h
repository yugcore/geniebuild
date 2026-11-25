#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

namespace core {

struct SourceRule {
    std::string include;              // e.g. "src/core/**/*.{c,cpp}"
    std::vector<std::string> exclude; // e.g. ["src/legacy/**"]
    std::string condition;            // optional "if" expression, e.g. "windows"
    bool follow_links = false;        // avoid symlink/junction cycles by default
};

// Precompiled glob matcher supporting *, **, ?, and {a,b} brace alternatives.
class CompiledGlob {
public:
    CompiledGlob() = default;
    static CompiledGlob Compile(const std::string& pattern);

    bool IsValid() const { return !m_variants.empty(); }
    const std::string& Pattern() const { return m_pattern; }

    // Tests a normalized relative path (forward slashes, no leading "./")
    bool Matches(const std::string& rel_path) const;

    // Returns longest literal directory prefix before any wildcard segment (e.g. "src/core/**" -> "src/core")
    const std::string& LongestLiteralPrefix() const { return m_longest_literal_prefix; }

    // Returns true if the literal prefix or an explicit literal segment names dir_name
    bool ExplicitlyNamesDir(const std::string& dir_name) const;

    // Splits pattern into brace-expanded raw variants (public for unit testing)
    static std::vector<std::string> ExpandBraces(const std::string& pattern);

    // Matches a single path segment against a wildcard segment (* and ?)
    static bool MatchSegment(const std::string& pattern_seg, const std::string& text_seg, bool case_insensitive);

private:
    enum class SegKind {
        Literal,
        Wildcard,
        GlobStar // "**"
    };

    struct Segment {
        SegKind kind = SegKind::Literal;
        std::string text;
        std::string text_lower;
    };

    std::string m_pattern;
    std::vector<std::vector<Segment>> m_variants;
    std::string m_longest_literal_prefix;
    std::unordered_set<std::string> m_literal_dir_names; // lowercase dir segments named literally
    bool m_case_insensitive = false;
};

// Cached directory listing entry
struct CachedDirEntry {
    std::string name;
    bool is_directory = false;
    bool is_symlink = false;
};

struct CachedDirectory {
    std::filesystem::file_time_type mtime{};
    bool dirty = true;
    std::vector<CachedDirEntry> entries;
};

// Thread-safe in-memory directory cache (uses FindFirstFileExW on Windows for speed)
class DirectoryCache {
public:
    // Marks a relative directory (e.g. "src/core") and optionally root dirty
    void MarkDirty(const std::string& rel_dir);
    void MarkAllDirty();
    void Clear();

    // Returns true if any cached directory is marked dirty
    bool HasDirty() const;

    // Reads entries of rel_dir ("" for root), returning cached entries if valid & clean
    const std::vector<CachedDirEntry>& GetEntries(
        const std::filesystem::path& root,
        const std::string& rel_dir,
        bool follow_links = false
    );

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, CachedDirectory> m_dirs;
};

// Input descriptor per target for ExpandRules
struct TargetRuleInput {
    std::string name;
    std::vector<SourceRule> rules;
    std::vector<std::string> explicit_files;
    std::vector<std::string> target_excludes;
    std::string condition_os;       // "windows" | "linux" | "macos"
    std::string condition_compiler; // "msvc" | "gcc" | "clang"
};

struct TargetRuleOutput {
    std::vector<std::string> source_files;   // effective compilable files: (rules - excludes) U explicit_files
    std::vector<std::string> header_files;   // headers matched by rules (context only)
    std::vector<size_t> rule_match_counts;   // compilable matches per rule
};

struct RuleExpansionResult {
    std::unordered_map<std::string, TargetRuleOutput> per_target;
    std::unordered_map<std::string, std::vector<std::string>> multi_target_files; // file -> targets (>1)
};

struct RulePreviewResult {
    size_t total_count = 0;
    std::vector<std::string> preview_files;
};

struct SimplifySuggestion {
    std::string folder_rel;                  // e.g. "src/core"
    std::string suggested_include;           // e.g. "src/core/**/*.{c,cc,cpp,cxx}"
    std::vector<std::string> replaced_files; // explicit files covered by this rule
    bool exact_match = false;                // true if rule matches the exact same set in that folder
};

// Evaluates simple V2-1/V2-5 condition expressions (e.g. "windows", "!linux", "windows && msvc")
bool EvaluateSimpleCondition(const std::string& expr, const std::string& os_name, const std::string& compiler_family);

// Returns true if extension is a compilable C/C++ source file
bool IsCompilableSourceExtension(const std::string& ext_lower);
bool IsHeaderExtension(const std::string& ext_lower);

// Single-walk rule expansion across all targets using DirectoryCache
RuleExpansionResult ExpandRules(
    const std::filesystem::path& root,
    const std::vector<TargetRuleInput>& targets,
    DirectoryCache& cache
);

// Fast live preview for Rule Editor popover (returns first max_preview matches + total count)
RulePreviewResult PreviewRule(
    const std::filesystem::path& root,
    const SourceRule& rule,
    const std::vector<std::string>& target_excludes,
    DirectoryCache& cache,
    size_t max_preview = 200
);

// Checks if a target with >50 explicit files can simplify any folder to a glob rule
std::vector<SimplifySuggestion> ComputeSimplifySuggestions(
    const std::filesystem::path& root,
    const std::vector<std::string>& explicit_files,
    DirectoryCache& cache
);

// Background recursive file watcher (ReadDirectoryChangesW on Windows, polling fallback elsewhere)
// Debounces bursts by 300ms and marks affected directories dirty in DirectoryCache.
class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    void Start(const std::filesystem::path& root, DirectoryCache* cache);
    void Stop();

    // Returns true (and clears flag) if debounced filesystem changes are ready to re-expand
    bool ConsumePendingChanges();

private:
    std::filesystem::path m_root;
    DirectoryCache* m_cache = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_pending_changes{false};
    std::thread m_thread;
#if defined(_WIN32)
    void* m_stop_event = nullptr; // HANDLE
#endif

    void WatcherLoop();
};

} // namespace core
