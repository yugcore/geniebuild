#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include "project.h"
#include "compiler.h"
#include "compiler_cache.h"

namespace core {

class BuildLog;
class ChildProcess;

enum class BuildStatus {
    Idle,
    Running,
    Finished
};

enum class OutLineKind {
    Command,
    Raw,
    Status,
    Guard,
    Progress
};

struct OutLine {
    OutLineKind kind = OutLineKind::Raw;
    std::string text;
    bool has_error = false;
    bool has_warning = false;
};

struct ActiveJobInfo {
    std::string label;
    double elapsedSeconds = 0.0;
};

struct SlowestFileInfo {
    std::string path;
    uint32_t durationMs = 0;
};

struct BuildProgress {
    size_t completedJobs = 0;
    size_t totalJobs = 0;
    size_t compiledCount = 0;
    size_t upToDateCount = 0;
    size_t failedCount = 0;
    double elapsedSeconds = 0.0;
    double etaSeconds = -1.0;
    std::vector<ActiveJobInfo> activeJobs;
};

struct BuildResult {
    int exit_code = 0;
    double duration_seconds = 0.0;
    bool success = false;
    bool cancelled = false;
    size_t checked_files_count = 0;
    size_t compiled_count = 0;
    size_t up_to_date_count = 0;
    size_t failed_count = 0;
    size_t skipped_count = 0;
    int active_worker_count = 0;
    bool ram_capped = false;
    BuildCacheSummary cache_summary;

    // V2-20 Phase timings & slowest files summary
    double steps_seconds = 0.0;
    double pch_seconds = 0.0;
    double compile_seconds = 0.0;
    double link_seconds = 0.0;
    std::vector<SlowestFileInfo> slowest_files;

    // V2-19 compile_commands.json status
    bool compile_commands_ok = false;
    bool compile_commands_modified = false;
    size_t compile_commands_count = 0;

    // V2-23 Windows Defender suggestion
    bool defender_hint_recommended = false;
};

// Executes the compiler on a background thread using ChildProcess (Job Objects / PGID),
// capturing merged stdout/stderr, progress/ETA, build log, and compile_commands.json.
class BuildRunner {
public:
    BuildRunner();
    ~BuildRunner();

    // Validates guards (empty list, missing files, missing compiler)
    // and launches the incremental compiler process on a background thread.
    // If target_name is empty, builds all active targets; otherwise builds target_name + its transitive deps.
    // Returns true if build started; returns false with error diagnostic in out_error.
    bool StartBuild(
        Project& project,
        const CompilerManager& compiler_manager,
        std::string& out_error,
        const std::string& target_name = ""
    );

    // Cleans the active configuration and starts a full rebuild (Spec §5.1.5)
    bool StartRebuild(
        Project& project,
        const CompilerManager& compiler_manager,
        std::string& out_error,
        const std::string& target_name = ""
    );

    // Cleans object files and output binary for active configuration (Spec §5.1.5)
    bool Clean(
        const Project& project,
        std::string& out_message,
        const std::string& target_name = ""
    );

    // Queries current execution status
    BuildStatus GetStatus() const { return m_status.load(); }
    bool IsRunning() const { return m_status.load() == BuildStatus::Running; }

    // V2-18 Cancels the running build and terminates all active child process trees (< 1s)
    void CancelBuild();
    bool IsCancelRequested() const { return m_cancel_requested.load(); }

    // V2-20 Live progress, ETA & running jobs snapshot
    BuildProgress GetProgress() const;

    // V2-21 Disk-backed streaming build log (supports 2M+ lines)
    std::shared_ptr<BuildLog> GetBuildLog() const { return m_build_log; }

    // Drains any newly arrived compiler output lines. Called by UI thread every frame.
    std::vector<OutLine> DrainOutput();

    // Returns the result of the completed build
    BuildResult GetLastResult() const;

    // Returns elapsed seconds while building, or final duration when completed
    double GetElapsedSeconds() const;

    // Returns the full command line string executed
    std::string GetExecutedCommand() const;

    // V2-10 / V2-13 live status bar queries
    int GetActiveWorkerCount() const { return m_active_workers.load(); }
    bool IsRamCapped() const { return m_ram_capped.load(); }
    bool IsThrottledLowMemory() const { return m_throttled_low_memory.load(); }
    BuildCacheSummary GetLastCacheSummary() const;

    // Formulates the full compiler command including -I flags and output path
    static BuildCommand FormulateBuildCommand(
        const CompilerInfo& compiler,
        const Project& project,
        std::filesystem::path& out_binary_path
    );

    // Maps a source path to a sanitized object path strictly inside obj_dir (E3)
    static std::filesystem::path ComputeObjectPath(
        const Project& project,
        const std::filesystem::path& obj_dir,
        const std::string& file_rel,
        bool is_msvc,
        std::string& out_obj_key
    );

    // Classifies a compiler/linker output line as error and/or warning without modifying text (E5)
    static void ClassifyDiagnosticLine(
        const std::string& line,
        bool& out_has_error,
        bool& out_has_warning
    );

private:
    std::atomic<BuildStatus> m_status{BuildStatus::Idle};
    std::atomic<bool> m_cancel_requested{false};
    std::atomic<int> m_active_workers{0};
    std::atomic<bool> m_ram_capped{false};
    std::atomic<bool> m_throttled_low_memory{false};
    std::thread m_worker;

    mutable std::mutex m_active_procs_mutex;
    std::unordered_set<ChildProcess*> m_active_procs;
    std::atomic<bool>* m_worker_abort_flag = nullptr;
    std::condition_variable* m_worker_dag_cv = nullptr;
    std::mutex* m_worker_dag_mutex = nullptr;

    struct ActiveJobSlot {
        std::string label;
        std::chrono::steady_clock::time_point startTime;
    };
    mutable std::mutex m_progress_mutex;
    std::unordered_map<uint64_t, ActiveJobSlot> m_running_slots;
    std::atomic<uint64_t> m_next_slot_id{1};
    std::atomic<size_t> m_prog_completed{0};
    std::atomic<size_t> m_prog_total{0};
    std::atomic<size_t> m_prog_compiled{0};
    std::atomic<size_t> m_prog_uptodate{0};
    std::atomic<size_t> m_prog_failed{0};
    std::atomic<int64_t> m_remaining_est_ms{0};
    mutable double m_smoothed_eta_s = -1.0;

    std::shared_ptr<BuildLog> m_build_log;

    mutable std::mutex m_queue_mutex;
    std::vector<OutLine> m_output_queue;

    mutable std::mutex m_result_mutex;
    BuildResult m_last_result;
    std::string m_executed_command;
    std::chrono::high_resolution_clock::time_point m_start_time;

    void WorkerRun(
        const Project* project,
        const CompilerManager* compiler_manager,
        std::filesystem::path log_path,
        std::vector<Target> targets_snapshot,
        std::string target_filter
    );
};

} // namespace core
