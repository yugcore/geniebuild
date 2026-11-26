#include "run_runner.h"

namespace core {

RunRunner::RunRunner() = default;

RunRunner::~RunRunner() {
    Stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool RunRunner::Start(
    const BuildCommand& cmd,
    const std::filesystem::path& working_dir,
    const std::unordered_map<std::string, std::string>* extra_env,
    std::string* out_error
) {
    if (IsRunning()) {
        if (out_error) *out_error = "A target process is already running.";
        return false;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (cmd.program.empty()) {
        if (out_error) *out_error = "Run command is empty.";
        return false;
    }

    auto proc = std::make_unique<ChildProcess>();
    ProcessOptions opts;
    opts.workingDir = working_dir;
    opts.extraEnv = extra_env;
    opts.pipeStdin = true;
    opts.belowNormalPriority = false;

    if (!proc->Start(cmd, opts, out_error)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_proc_mutex);
        m_process = std::move(proc);
        m_running_program = cmd.program;
        m_start_time = std::chrono::high_resolution_clock::now();
        m_final_duration = 0.0;
    }
    m_stop_requested.store(false);
    m_exit_code.store(0);
    m_has_started.store(true);
    m_running.store(true);

    {
        std::lock_guard<std::mutex> ql(m_queue_mutex);
        OutLine hdr;
        hdr.kind = OutLineKind::Command;
        hdr.text = cmd.display_command.empty() ? cmd.program : cmd.display_command;
        m_output_queue.push_back(std::move(hdr));
    }

    m_worker = std::thread([this]() {
        std::string leftover;
        uint8_t buf[4096];
        auto push_line = [&](const std::string& s) {
            std::lock_guard<std::mutex> ql(m_queue_mutex);
            OutLine ol;
            ol.kind = OutLineKind::Raw;
            ol.text = s;
            m_output_queue.push_back(std::move(ol));
        };

        while (true) {
            ChildProcess* p = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_proc_mutex);
                p = m_process.get();
            }
            if (!p) break;

            bool eof = false;
            std::ptrdiff_t n = p->ReadStdout(buf, sizeof(buf), eof);
            if (n <= 0 && eof) break;
            if (n > 0) {
                leftover.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
                size_t pos = 0;
                while ((pos = leftover.find('\n')) != std::string::npos) {
                    std::string line = leftover.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    push_line(line);
                    leftover.erase(0, pos + 1);
                }
            }
        }
        if (!leftover.empty()) {
            if (leftover.back() == '\r') leftover.pop_back();
            push_line(leftover);
        }

        int status = 0;
        {
            std::lock_guard<std::mutex> lk(m_proc_mutex);
            if (m_process) {
                status = m_process->WaitExit(500);
                m_process.reset();
            }
            m_final_duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - m_start_time).count();
        }
        m_exit_code.store(status);
        m_running.store(false);

        std::lock_guard<std::mutex> ql(m_queue_mutex);
        OutLine done;
        done.kind = OutLineKind::Status;
        done.text = m_stop_requested.load()
            ? "■ Process stopped by user"
            : ("Process exited with code " + std::to_string(status));
        done.has_error = (!m_stop_requested.load() && status != 0);
        m_output_queue.push_back(std::move(done));
    });

    return true;
}

bool RunRunner::SendStdin(const std::string& line) {
    std::lock_guard<std::mutex> lk(m_proc_mutex);
    if (!m_process || !m_running.load()) return false;
    std::string payload = line + "\n";
    if (m_process->WriteStdin(payload.data(), payload.size())) {
        std::lock_guard<std::mutex> ql(m_queue_mutex);
        OutLine echo;
        echo.kind = OutLineKind::Command;
        echo.text = line;
        m_output_queue.push_back(std::move(echo));
        return true;
    }
    return false;
}

void RunRunner::Stop() {
    m_stop_requested.store(true);
    std::lock_guard<std::mutex> lk(m_proc_mutex);
    if (m_process) {
        m_process->TerminateTree();
    }
}

double RunRunner::GetElapsedSeconds() const {
    if (m_running.load()) {
        return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - m_start_time).count();
    }
    return m_final_duration;
}

std::string RunRunner::GetRunningProgram() const {
    if (!m_running.load()) return "";
    std::lock_guard<std::mutex> lk(m_proc_mutex);
    return m_running_program;
}

std::vector<OutLine> RunRunner::DrainOutput() {
    std::lock_guard<std::mutex> ql(m_queue_mutex);
    std::vector<OutLine> out;
    out.swap(m_output_queue);
    return out;
}

} // namespace core
