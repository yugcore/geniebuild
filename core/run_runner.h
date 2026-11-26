#pragma once

#include "target.h"
#include "build_runner.h"
#include "child_process.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace core {

// V2-16 Live target runner with merged stdout/stderr and interactive stdin pipe
class RunRunner {
public:
    RunRunner();
    ~RunRunner();

    RunRunner(const RunRunner&) = delete;
    RunRunner& operator=(const RunRunner&) = delete;

    bool Start(
        const BuildCommand& cmd,
        const std::filesystem::path& working_dir,
        const std::unordered_map<std::string, std::string>* extra_env = nullptr,
        std::string* out_error = nullptr
    );

    bool SendStdin(const std::string& line);
    void Stop();

    bool IsRunning() const { return m_running.load(); }
    bool HasEverStarted() const { return m_has_started.load(); }
    int GetLastExitCode() const { return m_exit_code.load(); }
    double GetElapsedSeconds() const;
    std::string GetRunningProgram() const;
    std::vector<OutLine> DrainOutput();

private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_has_started{false};
    std::atomic<bool> m_stop_requested{false};
    std::atomic<int> m_exit_code{0};

    std::chrono::high_resolution_clock::time_point m_start_time;
    double m_final_duration = 0.0;

    mutable std::mutex m_proc_mutex;
    std::unique_ptr<ChildProcess> m_process;
    std::string m_running_program;

    std::mutex m_queue_mutex;
    std::vector<OutLine> m_output_queue;
    std::thread m_worker;
};

} // namespace core
