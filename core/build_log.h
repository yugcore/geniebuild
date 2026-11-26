#pragma once

#include "build_runner.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace core {

struct LogLineEntry {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint8_t kind = 0;     // OutLineKind
    uint8_t flags = 0;    // bit 0: has_error, bit 1: has_warning
    uint16_t targetIdx = 0;
    uint32_t jobId = 0;
};

struct LogJobBlock {
    uint32_t jobId = 0;
    std::string header;
    size_t firstLine = 0;
    size_t lineCount = 0;
    bool hasOutput = false;
    bool hasError = false;
    bool hasWarning = false;
    bool collapsed = true;
};

enum class LogFilterMode {
    All = 0,
    Errors = 1,
    Warnings = 2
};

// V2-21 Disk-backed streaming build log with compact line index, LRU line cache,
// incremental error/warning filters, target filter, background search, and collapsible job blocks.
class BuildLog {
public:
    BuildLog();
    ~BuildLog();

    BuildLog(const BuildLog&) = delete;
    BuildLog& operator=(const BuildLog&) = delete;

    // Begins a new build log session under <root>/.geniebuild/logs/<timestamp>_<config>.log (keeps last 10)
    bool BeginSession(const std::filesystem::path& project_root, const std::string& config);

    // Finalizes the session and copies to <root>/.geniebuild/last_build.log
    void EndSession();

    // Clears in-memory index and resets display state
    void Clear();

    // Loads an existing log file (e.g., synthetic 2M-line benchmark log) directly by indexing byte offsets
    bool LoadFromFile(const std::filesystem::path& file_path);

    // Appends lines (used by UI when draining BuildRunner or tests). Assigns target/job metadata cleanly.
    void AppendLines(const std::vector<OutLine>& lines);

    size_t GetTotalLines() const;
    size_t GetErrorCount() const;
    size_t GetWarningCount() const;
    std::vector<std::string> GetTargetNames() const;

    // Reads a line using small LRU cache + disk random-access fallback
    bool GetLineMetaAndText(size_t line_idx, LogLineEntry& out_meta, std::string& out_text) const;

    // Builds / retrieves filtered index list for the active filter parameters
    std::vector<uint32_t> GetFilteredIndices(
        LogFilterMode mode,
        const std::string& target_filter,
        const std::string& file_filter
    ) const;

    // Jump to next/previous error relative to current_line (returns invalid npos if none)
    size_t FindNextError(size_t current_line, bool forward) const;

    // Job block helpers for V2-20 collapsible groups
    std::vector<LogJobBlock> GetJobBlocksSnapshot() const;
    void SetJobCollapsed(uint32_t job_id, bool collapsed);

    // Background search (V2-21)
    void StartSearch(const std::string& query, bool case_sensitive = false);
    bool IsSearching() const { return m_searching.load(); }
    std::vector<uint32_t> GetSearchMatches() const;
    std::string GetActiveSearchQuery() const;

    std::filesystem::path GetCurrentLogFile() const;
    std::filesystem::path GetLogsDirectory() const;

private:
    mutable std::mutex m_mutex;
    std::filesystem::path m_project_root;
    std::filesystem::path m_logs_dir;
    std::filesystem::path m_current_log_path;
    std::filesystem::path m_last_log_path;
    std::ofstream m_out_stream;
    uint64_t m_write_offset = 0;

    std::vector<LogLineEntry> m_entries;
    std::vector<uint32_t> m_error_lines;
    std::vector<uint32_t> m_warning_lines;
    std::vector<std::string> m_targets;
    std::unordered_map<std::string, uint16_t> m_target_map;

    std::vector<LogJobBlock> m_jobs;
    std::unordered_map<uint32_t, size_t> m_job_idx_by_id;
    uint32_t m_active_job_id = 0;
    uint32_t m_next_job_id = 1;

    // Small in-memory LRU cache of decoded raw lines (keeps memory tiny even at 2M+ lines)
    static constexpr size_t MAX_LRU_LINES = 4096;
    mutable std::mutex m_cache_mutex;
    mutable std::list<std::pair<size_t, std::string>> m_lru_list;
    mutable std::unordered_map<size_t, std::list<std::pair<size_t, std::string>>::iterator> m_lru_map;
    mutable std::ifstream m_read_stream;
    mutable std::filesystem::path m_read_stream_path;

    // Background search thread state
    std::atomic<bool> m_searching{false};
    std::atomic<bool> m_abort_search{false};
    std::thread m_search_thread;
    mutable std::mutex m_search_mutex;
    std::string m_search_query;
    std::vector<uint32_t> m_search_matches;

    void PutLruLocked(size_t line_idx, const std::string& text) const;
    std::string ReadLineUncached(const LogLineEntry& meta) const;
    void PruneOldLogs(size_t keep_count) const;
    uint16_t InternTargetLocked(const std::string& target_name);
    void JoinSearchThread();
};

} // namespace core
