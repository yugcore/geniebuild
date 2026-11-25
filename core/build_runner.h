#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include "project.h"
#include "compiler.h"

namespace core {

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

struct BuildResult {
    int exit_code = 0;
    double duration_seconds = 0.0;
    bool success = false;
};

// Executes the compiler on a background thread using reproc++, capturing merged stdout/stderr.
// Core component: strictly independent of UI and windowing libraries.
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

    // Drains any newly arrived compiler output lines. Called by UI thread every frame.
    std::vector<OutLine> DrainOutput();

    // Returns the result of the completed build
    BuildResult GetLastResult() const;

    // Returns elapsed seconds while building, or final duration when completed
    double GetElapsedSeconds() const;

    // Returns the full command line string executed
    std::string GetExecutedCommand() const;

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
    std::thread m_worker;

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
