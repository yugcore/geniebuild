#if defined(_WIN32)
#include "child_process.h"
#include "fs_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

namespace core {

namespace {

std::wstring QuoteWin32Arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t num_bs = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++num_bs;
        }
        if (it == arg.end()) {
            out.append(num_bs * 2, L'\\');
            break;
        } else if (*it == L'"') {
            out.append(num_bs * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(num_bs, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring BuildWin32CommandLine(const BuildCommand& cmd) {
    std::wstring cl = QuoteWin32Arg(fs_util::Utf8ToWide(cmd.program));
    for (const auto& a : cmd.arguments) {
        cl.push_back(L' ');
        cl += QuoteWin32Arg(fs_util::Utf8ToWide(a));
    }
    return cl;
}

std::wstring BuildMergedUnicodeEnvBlock(const std::unordered_map<std::string, std::string>* extra_env) {
    if (!extra_env || extra_env->empty()) {
        return L""; // Inherit parent env directly
    }

    struct CiLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        }
    };
    std::map<std::wstring, std::wstring, CiLess> merged;

    LPWCH env_strs = GetEnvironmentStringsW();
    if (env_strs) {
        const wchar_t* p = env_strs;
        while (*p) {
            std::wstring entry(p);
            size_t eq = entry.find(L'=');
            if (eq != std::wstring::npos && eq > 0) {
                merged[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(env_strs);
    }

    for (const auto& [k, v] : *extra_env) {
        if (k.empty()) continue;
        std::wstring wk = fs_util::Utf8ToWide(k);
        std::wstring wv = fs_util::Utf8ToWide(v);
        merged[wk] = wv;
    }

    std::wstring block;
    for (const auto& [k, v] : merged) {
        block += k;
        block.push_back(L'=');
        block += v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::string ResolveExecutableForWin32(const std::string& prog, const std::filesystem::path& working_dir) {
    std::filesystem::path p = fs_util::ToWinLongPath(std::filesystem::u8path(prog));
    if (prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos) {
        std::error_code ec;
        if (!std::filesystem::u8path(prog).is_absolute() && !working_dir.empty()) {
            std::filesystem::path rel_cand = working_dir / std::filesystem::u8path(prog);
            if (fs_util::ExistsLong(rel_cand)) {
                return rel_cand.lexically_normal().generic_u8string();
            }
        }
        return prog;
    }

    std::wstring wprog = fs_util::Utf8ToWide(prog);
    wchar_t found[MAX_PATH * 4];
    DWORD len = SearchPathW(nullptr, wprog.c_str(), L".exe", static_cast<DWORD>(std::size(found)), found, nullptr);
    if (len > 0 && len < std::size(found)) {
        return fs_util::WideToUtf8(std::wstring(found, len));
    }
    len = SearchPathW(nullptr, wprog.c_str(), L".bat", static_cast<DWORD>(std::size(found)), found, nullptr);
    if (len > 0 && len < std::size(found)) {
        return fs_util::WideToUtf8(std::wstring(found, len));
    }
    return prog;
}

std::string FormatLastWinError(DWORD err) {
    LPWSTR msg = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msg),
        0,
        nullptr
    );
    std::string s = "Win32 error " + std::to_string(err);
    if (len > 0 && msg) {
        std::wstring ws(msg, len);
        while (!ws.empty() && (ws.back() == L'\r' || ws.back() == L'\n' || ws.back() == L' ')) {
            ws.pop_back();
        }
        s = fs_util::WideToUtf8(ws);
    }
    if (msg) LocalFree(msg);
    return s;
}

} // namespace

struct ChildProcess::Impl {
    std::mutex mu;
    HANDLE hJob = nullptr;
    HANDLE hProcess = nullptr;
    HANDLE hThread = nullptr;
    HANDLE hOutRead = nullptr;
    HANDLE hOutWrite = nullptr;
    HANDLE hInRead = nullptr;
    HANDLE hInWrite = nullptr;
    DWORD pid = 0;
    std::atomic<bool> terminated{false};

    ~Impl() {
        CloseAll();
    }

    void ClosePipeHandles() {
        if (hOutRead) { CloseHandle(hOutRead); hOutRead = nullptr; }
        if (hOutWrite) { CloseHandle(hOutWrite); hOutWrite = nullptr; }
        if (hInRead) { CloseHandle(hInRead); hInRead = nullptr; }
        if (hInWrite) { CloseHandle(hInWrite); hInWrite = nullptr; }
    }

    void CloseAll() {
        std::lock_guard<std::mutex> lk(mu);
        ClosePipeHandles();
        if (hThread) { CloseHandle(hThread); hThread = nullptr; }
        if (hProcess) { CloseHandle(hProcess); hProcess = nullptr; }
        if (hJob) { CloseHandle(hJob); hJob = nullptr; }
    }
};

ChildProcess::ChildProcess() : m_impl(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() {
    TerminateTree();
}

bool ChildProcess::Start(const BuildCommand& cmd, const ProcessOptions& opts, std::string* out_error) {
    if (cmd.program.empty()) {
        if (out_error) *out_error = "Empty process program.";
        return false;
    }

    std::string resolved_prog = ResolveExecutableForWin32(cmd.program, opts.workingDir);
    BuildCommand run_cmd = cmd;
    run_cmd.program = resolved_prog;
    std::wstring cmdline = BuildWin32CommandLine(run_cmd);
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        if (out_error) *out_error = "CreatePipe(stdout) failed: " + FormatLastWinError(GetLastError());
        return false;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE err_write_dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), out_write, GetCurrentProcess(), &err_write_dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        err_write_dup = out_write;
    }

    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    if (opts.pipeStdin) {
        if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
            CloseHandle(out_read);
            CloseHandle(out_write);
            if (err_write_dup && err_write_dup != out_write) CloseHandle(err_write_dup);
            if (out_error) *out_error = "CreatePipe(stdin) failed: " + FormatLastWinError(GetLastError());
            return false;
        }
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE nul_in = nullptr;
    HANDLE stdin_handle = nullptr;
    if (in_read) {
        stdin_handle = in_read;
    } else {
        nul_in = CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (nul_in != INVALID_HANDLE_VALUE) {
            stdin_handle = nul_in;
        } else {
            nul_in = nullptr;
            stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
        }
    }

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    std::wstring env_block = BuildMergedUnicodeEnvBlock(opts.extraEnv);
    std::wstring cwd_w;
    if (!opts.workingDir.empty()) {
        cwd_w = fs_util::ToWinLongPath(opts.workingDir).wstring();
    }

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    if (opts.belowNormalPriority) {
        flags |= BELOW_NORMAL_PRIORITY_CLASS;
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdOutput = out_write;
    siex.StartupInfo.hStdError = err_write_dup;
    siex.StartupInfo.hStdInput = stdin_handle;

    // Explicitly restrict handle inheritance to ONLY this child's stdio handles so
    // concurrent worker threads never inherit sibling process pipe handles and stall EOF.
    std::vector<HANDLE> inherit_handles;
    auto push_unique_handle = [&](HANDLE h) {
        if (!h || h == INVALID_HANDLE_VALUE) return;
        for (HANDLE ex : inherit_handles) {
            if (ex == h) return;
        }
        inherit_handles.push_back(h);
    };
    push_unique_handle(out_write);
    push_unique_handle(err_write_dup);
    if (nul_in || in_read) {
        push_unique_handle(stdin_handle);
    }

    std::vector<uint8_t> attr_buf;
    bool have_attr_list = false;
    DWORD attr_count = (!inherit_handles.empty() ? 1 : 0) + (hJob ? 1 : 0);
    if (attr_count > 0) {
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_size);
        if (attr_size > 0) {
            attr_buf.resize(attr_size);
            siex.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
            if (InitializeProcThreadAttributeList(siex.lpAttributeList, attr_count, 0, &attr_size)) {
                bool attr_ok = true;
                if (!inherit_handles.empty()) {
                    attr_ok = attr_ok && UpdateProcThreadAttribute(
                        siex.lpAttributeList,
                        0,
                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                        inherit_handles.data(),
                        inherit_handles.size() * sizeof(HANDLE),
                        nullptr,
                        nullptr
                    );
                }
                if (hJob) {
                    attr_ok = attr_ok && UpdateProcThreadAttribute(
                        siex.lpAttributeList,
                        0,
                        PROC_THREAD_ATTRIBUTE_JOB_LIST,
                        &hJob,
                        sizeof(HANDLE),
                        nullptr,
                        nullptr
                    );
                }
                if (attr_ok) {
                    have_attr_list = true;
                    flags |= EXTENDED_STARTUPINFO_PRESENT;
                } else {
                    DeleteProcThreadAttributeList(siex.lpAttributeList);
                    siex.lpAttributeList = nullptr;
                }
            }
        }
    }

    static std::mutex s_fallback_spawn_mutex;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr,
        cmdline_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        flags,
        env_block.empty() ? nullptr : reinterpret_cast<LPVOID>(&env_block[0]),
        cwd_w.empty() ? nullptr : cwd_w.c_str(),
        have_attr_list ? reinterpret_cast<LPSTARTUPINFOW>(&siex) : &siex.StartupInfo,
        &pi
    );

    if (!ok && have_attr_list) {
        // Fallback for environments where PROC_THREAD_ATTRIBUTE_* fails (nested job / older Win):
        // CREATE_SUSPENDED -> AssignProcessToJobObject -> ResumeThread
        DeleteProcThreadAttributeList(siex.lpAttributeList);
        have_attr_list = false;
        siex.lpAttributeList = nullptr;
        siex.StartupInfo.cb = sizeof(STARTUPINFOW);
        DWORD fb_flags = (flags & ~EXTENDED_STARTUPINFO_PRESENT) | CREATE_SUSPENDED;
        cmdline_buf.assign(cmdline.begin(), cmdline.end());
        cmdline_buf.push_back(L'\0');
        std::lock_guard<std::mutex> fblk(s_fallback_spawn_mutex);
        ok = CreateProcessW(
            nullptr,
            cmdline_buf.data(),
            nullptr,
            nullptr,
            TRUE,
            fb_flags,
            env_block.empty() ? nullptr : reinterpret_cast<LPVOID>(&env_block[0]),
            cwd_w.empty() ? nullptr : cwd_w.c_str(),
            &siex.StartupInfo,
            &pi
        );
        if (ok) {
            if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
            ResumeThread(pi.hThread);
        }
    }

    if (have_attr_list) {
        DeleteProcThreadAttributeList(siex.lpAttributeList);
    }

    // Close child-side pipe ends in parent immediately so EOF works reliably
    if (err_write_dup && err_write_dup != out_write) CloseHandle(err_write_dup);
    CloseHandle(out_write);
    if (in_read) CloseHandle(in_read);
    if (nul_in) CloseHandle(nul_in);

    if (!ok) {
        DWORD err = GetLastError();
        if (out_read) CloseHandle(out_read);
        if (in_write) CloseHandle(in_write);
        if (hJob) CloseHandle(hJob);
        if (out_error) *out_error = "Failed to launch '" + cmd.program + "': " + FormatLastWinError(err);
        return false;
    }

    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->hJob = hJob;
    m_impl->hProcess = pi.hProcess;
    m_impl->hThread = pi.hThread;
    m_impl->hOutRead = out_read;
    m_impl->hInWrite = in_write;
    m_impl->pid = pi.dwProcessId;
    m_impl->terminated.store(false);
    return true;
}

std::ptrdiff_t ChildProcess::ReadStdout(uint8_t* buf, size_t capacity, bool& out_eof) {
    out_eof = false;
    HANDLE hr = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        hr = m_impl->hOutRead;
    }
    if (!hr) {
        out_eof = true;
        return 0;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(hr, buf, static_cast<DWORD>(capacity), &got, nullptr);
    if (!ok || got == 0) {
        out_eof = true;
        return 0;
    }
    return static_cast<std::ptrdiff_t>(got);
}

bool ChildProcess::WriteStdin(const char* data, size_t len) {
    HANDLE hw = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        hw = m_impl->hInWrite;
    }
    if (!hw) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hw, data, static_cast<DWORD>(len), &written, nullptr);
    return ok && written == static_cast<DWORD>(len);
}

int ChildProcess::WaitExit(int timeout_ms) {
    HANDLE hp = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        hp = m_impl->hProcess;
    }
    if (!hp) return -1;
    DWORD wait_ms = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    DWORD wr = WaitForSingleObject(hp, wait_ms);
    if (wr != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    if (!GetExitCodeProcess(hp, &code)) return -1;
    return static_cast<int>(code);
}

void ChildProcess::TerminateTree() {
    if (m_impl->terminated.exchange(true)) return;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->hInWrite) {
        CloseHandle(m_impl->hInWrite);
        m_impl->hInWrite = nullptr;
    }
    if (m_impl->hJob) {
        TerminateJobObject(m_impl->hJob, 1);
    } else if (m_impl->hProcess) {
        TerminateProcess(m_impl->hProcess, 1);
    }
}

void ChildProcess::Detach() {
    m_impl->terminated.store(true);
    m_impl->CloseAll();
}

uint64_t ChildProcess::GetPid() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return static_cast<uint64_t>(m_impl->pid);
}

bool ChildProcess::IsRunning() const {
    HANDLE hp = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        hp = m_impl->hProcess;
    }
    if (!hp) return false;
    return WaitForSingleObject(hp, 0) == WAIT_TIMEOUT;
}

} // namespace core
#endif
