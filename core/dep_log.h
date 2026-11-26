#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace core {

struct DepOutputEntry {
    uint32_t outputId = 0;
    int64_t mtimeNs = 0;
    uint64_t cmdHash = 0;
    uint32_t durationMs = 0;
    std::vector<uint32_t> depIds;
};

struct DepStepEntry {
    uint32_t stepKeyId = 0;
    uint64_t cmdHash = 0;
    int64_t outputsMtimeNs = 0;
    std::vector<uint32_t> inputIds;
};

struct PchHeaderCandidate {
    std::string path;
    size_t tuCount = 0;
    bool isSystemOrExternal = false;
};

// V2-11 Binary dependency & duration log (.geniebuild/obj/<Config>/deps.bin)
// Thread-safe reader/appender with single background writer thread, compaction,
// corrupt-tail truncation, and directory-batched parallel stat.
class DepLog {
public:
    DepLog() = default;
    ~DepLog();

    DepLog(const DepLog&) = delete;
    DepLog& operator=(const DepLog&) = delete;

    // Normalizes path for dep log (forward slashes, collapsed '..', lowercase on Windows)
    static std::string NormalizeDepPath(const std::string& path_str);

    // Converts std::filesystem::file_time_type or disk mtime to int64 nanoseconds since epoch
    static int64_t FileTimeToNs(std::filesystem::file_time_type ft);

    // Exact single-file mtime stat in Unix nanoseconds (matches BatchStatFiles 100ns precision on Windows)
    static int64_t StatFileNs(const std::filesystem::path& path);

    // Converts a 64-bit hex command hash string to uint64_t (or FNV-1a fallback)
    static uint64_t ParseHashU64(const std::string& hex_or_str);

    // Returns true if path is inside known system / toolchain include directories
    static bool IsSystemHeader(const std::string& norm_path, const std::vector<std::string>& extra_sys_dirs = {});

    // Opens (and loads/recovers/compacts) deps.bin; migrates legacy state.json if deps.bin absent
    bool Open(
        const std::filesystem::path& deps_bin_path,
        const std::vector<std::filesystem::path>& legacy_state_json_paths = {}
    );

    // Flushes pending queued records and stops the writer thread
    void Close();

    // Interns or looks up a normalized path ID (thread-safe; queues PathRecord if new)
    uint32_t InternPath(const std::string& raw_or_norm_path);
    std::string GetPath(uint32_t id) const;
    uint32_t FindPathId(const std::string& raw_or_norm_path) const;

    // Queries latest loaded records (thread-safe read)
    bool TryGetOutput(const std::string& output_path, DepOutputEntry& out_entry) const;
    bool TryGetStep(const std::string& step_key, DepStepEntry& out_entry) const;
    uint32_t GetDurationMs(const std::string& output_path) const;

    // Enqueues an OutputRecord / StepRecord for asynchronous append & in-memory update
    void RecordOutput(
        const std::string& output_path,
        int64_t mtime_ns,
        uint64_t cmd_hash,
        uint32_t duration_ms,
        const std::vector<std::string>& dep_paths
    );

    void RecordStep(
        const std::string& step_key,
        uint64_t cmd_hash,
        int64_t outputs_mtime_ns,
        const std::vector<std::string>& input_paths
    );

    // Migrates a single key->hash pair (used during state.json fallback)
    bool TryGetLegacyHash(const std::string& key, uint64_t& out_hash) const;

    // Flushes writer queue to disk
    void Flush();

    // Directory-batched parallel stat across unique paths (returns norm_path -> mtime_ns)
    static std::unordered_map<std::string, int64_t> BatchStatFiles(
        const std::vector<std::string>& raw_or_norm_paths,
        int num_threads = 0
    );

    // Analyzes recorded dependencies for given TU outputs to suggest PCH headers (V2-8)
    std::vector<PchHeaderCandidate> SuggestPchHeaders(
        const std::vector<std::string>& target_output_paths,
        const std::filesystem::path& project_root,
        double min_tu_ratio = 0.5,
        size_t max_results = 20
    ) const;

    size_t GetTrackedOutputCount() const;

private:
    enum class RecordTag : uint8_t {
        Path = 1,
        Output = 2,
        Step = 3
    };

    struct QueuedWrite {
        std::vector<uint8_t> bytes;
    };

    std::filesystem::path m_path;
    mutable std::mutex m_data_mutex;
    std::vector<std::string> m_id_to_path; // 1-based id -> index = id - 1
    std::unordered_map<std::string, uint32_t> m_path_to_id;
    std::unordered_map<uint32_t, DepOutputEntry> m_outputs;
    std::unordered_map<uint32_t, DepStepEntry> m_steps;
    std::unordered_map<std::string, uint64_t> m_legacy_hashes;

    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::deque<QueuedWrite> m_write_queue;
    std::thread m_writer_thread;
    bool m_running = false;
    bool m_stop_requested = false;
    size_t m_pending_flush_count = 0;
    std::condition_variable m_flush_cv;

    void StartWriterThread();
    void WriterLoop();
    void EnqueueBytes(std::vector<uint8_t> bytes);
    bool RewriteCompactedLocked();

    static void AppendU8(std::vector<uint8_t>& buf, uint8_t v);
    static void AppendU16(std::vector<uint8_t>& buf, uint16_t v);
    static void AppendU32(std::vector<uint8_t>& buf, uint32_t v);
    static void AppendI64(std::vector<uint8_t>& buf, int64_t v);
    static void AppendU64(std::vector<uint8_t>& buf, uint64_t v);
};

} // namespace core
