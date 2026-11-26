#pragma once

#include "compiler.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace core {

struct ProcessOptions {
    std::filesystem::path workingDir;
    const std::unordered_map<std::string, std::string>* extraEnv = nullptr;
    bool pipeStdin = false;
    bool belowNormalPriority = false;
};

// V2-18 Cross-platform subprocess with process-tree isolation (Job Object on Win32, PGID on POSIX).
// Supports merged stdout/stderr reading, optional stdin writing, priority control, and < 1s tree kill.
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool Start(const BuildCommand& cmd, const ProcessOptions& opts, std::string* out_error = nullptr);

    // Reads merged stdout+stderr bytes. Sets out_eof = true on EOF or error.
    // Returns bytes read (0 on EOF).
    std::ptrdiff_t ReadStdout(uint8_t* buf, size_t capacity, bool& out_eof);

    // Writes raw bytes to stdin (only valid when pipeStdin = true).
    bool WriteStdin(const char* data, size_t len);

    // Waits for process exit (-1 = infinite). Returns exit code or -1 on error/timeout.
    int WaitExit(int timeout_ms = -1);

    // Immediately terminates the entire process tree (Job Object / process group). Thread-safe.
    void TerminateTree();

    // Detaches process handles so ~ChildProcess does not terminate a deliberately background-launched tool.
    void Detach();

    uint64_t GetPid() const;
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core
