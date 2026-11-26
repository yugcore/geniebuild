#if !defined(_WIN32)
#include "child_process.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace core {

struct ChildProcess::Impl {
    std::mutex mu;
    pid_t pid = -1;
    int out_read_fd = -1;
    int in_write_fd = -1;
    bool exited = false;
    int last_code = -1;

    ~Impl() {
        if (out_read_fd >= 0) { close(out_read_fd); out_read_fd = -1; }
        if (in_write_fd >= 0) { close(in_write_fd); in_write_fd = -1; }
    }
};

ChildProcess::ChildProcess() : m_impl(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() {
    TerminateTree();
    WaitExit(200);
}

bool ChildProcess::Start(const BuildCommand& cmd, const ProcessOptions& opts, std::string* out_error) {
    if (cmd.program.empty()) {
        if (out_error) *out_error = "Empty process program.";
        return false;
    }

    int out_pipe[2] = { -1, -1 };
    int in_pipe[2] = { -1, -1 };
    if (pipe(out_pipe) != 0) {
        if (out_error) *out_error = std::strerror(errno);
        return false;
    }
    if (opts.pipeStdin && pipe(in_pipe) != 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        if (out_error) *out_error = std::strerror(errno);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    if (opts.pipeStdin) {
        posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    }
#if defined(__GLIBC__) || defined(__APPLE__)
    if (!opts.workingDir.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, opts.workingDir.c_str());
    }
#endif

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    std::vector<std::string> arg_storage;
    arg_storage.push_back(cmd.program);
    for (const auto& a : cmd.arguments) arg_storage.push_back(a);
    std::vector<char*> argv;
    for (auto& s : arg_storage) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    std::vector<char*> envp;
    if (opts.extraEnv && !opts.extraEnv->empty()) {
        std::unordered_map<std::string, std::string> merged;
        for (char** e = environ; e && *e; ++e) {
            std::string entry(*e);
            size_t eq = entry.find('=');
            if (eq != std::string::npos) merged[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        for (const auto& [k, v] : *opts.extraEnv) merged[k] = v;
        for (const auto& [k, v] : merged) env_storage.push_back(k + "=" + v);
        for (auto& s : env_storage) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);
    }

    pid_t pid = -1;
    int rc = posix_spawnp(
        &pid,
        cmd.program.c_str(),
        &actions,
        &attr,
        argv.data(),
        envp.empty() ? environ : envp.data()
    );

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    if (opts.pipeStdin) close(in_pipe[0]);

    if (rc != 0) {
        close(out_pipe[0]);
        if (opts.pipeStdin) close(in_pipe[1]);
        if (out_error) *out_error = std::strerror(rc);
        return false;
    }

    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->pid = pid;
    m_impl->out_read_fd = out_pipe[0];
    m_impl->in_write_fd = opts.pipeStdin ? in_pipe[1] : -1;
    m_impl->exited = false;
    return true;
}

std::ptrdiff_t ChildProcess::ReadStdout(uint8_t* buf, size_t capacity, bool& out_eof) {
    out_eof = false;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        fd = m_impl->out_read_fd;
    }
    if (fd < 0) { out_eof = true; return 0; }
    ssize_t n = read(fd, buf, capacity);
    if (n <= 0) { out_eof = true; return 0; }
    return n;
}

bool ChildProcess::WriteStdin(const char* data, size_t len) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        fd = m_impl->in_write_fd;
    }
    if (fd < 0) return false;
    return write(fd, data, len) == static_cast<ssize_t>(len);
}

int ChildProcess::WaitExit(int timeout_ms) {
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->exited) return m_impl->last_code;
        pid = m_impl->pid;
    }
    if (pid <= 0) return -1;
    (void)timeout_ms;
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->exited = true;
    m_impl->last_code = code;
    return code;
}

void ChildProcess::TerminateTree() {
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        pid = m_impl->pid;
        if (m_impl->exited) return;
    }
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    usleep(200000);
    kill(-pid, SIGKILL);
}

void ChildProcess::Detach() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->pid = -1;
    m_impl->exited = true;
}

uint64_t ChildProcess::GetPid() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->pid > 0 ? static_cast<uint64_t>(m_impl->pid) : 0;
}

bool ChildProcess::IsRunning() const {
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->exited) return false;
        pid = m_impl->pid;
    }
    if (pid <= 0) return false;
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
    return r == 0;
}

} // namespace core
#endif
