#include "scheduler.h"
#include "project.h"
#include "target.h"
#include "dep_log.h"
#include "build_runner.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace core {

SystemMemoryInfo Scheduler::QuerySystemMemory() {
    SystemMemoryInfo info;
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info.valid = true;
        info.totalBytes = static_cast<uint64_t>(ms.ullTotalPhys);
        info.availBytes = static_cast<uint64_t>(ms.ullAvailPhys);
    }
#elif defined(__linux__)
    std::ifstream f("/proc/meminfo");
    if (f.is_open()) {
        std::string key;
        uint64_t val_kb = 0;
        std::string unit;
        uint64_t total_kb = 0, avail_kb = 0;
        while (f >> key >> val_kb >> unit) {
            if (key == "MemTotal:") total_kb = val_kb;
            else if (key == "MemAvailable:") avail_kb = val_kb;
        }
        if (total_kb > 0) {
            info.valid = true;
            info.totalBytes = total_kb * 1024ULL;
            info.availBytes = (avail_kb > 0 ? avail_kb : total_kb) * 1024ULL;
        }
    }
#endif
    return info;
}

WorkerBudget Scheduler::ComputeWorkerBudget(int requested_jobs, int mem_per_job_mb) {
    WorkerBudget b;
    int cores = static_cast<int>(std::thread::hardware_concurrency());
    if (cores <= 0) cores = 4;
    b.cores = cores;

    SystemMemoryInfo mem = QuerySystemMemory();
    if (mem.valid) {
        b.availRamMb = mem.availBytes / (1024ULL * 1024ULL);
        b.totalRamMb = mem.totalBytes / (1024ULL * 1024ULL);
    }

    if (requested_jobs > 0) {
        b.workers = requested_jobs;
        b.ramCapped = false;
        return b;
    }

    if (mem.valid && mem_per_job_mb > 0) {
        uint64_t per_job_bytes = static_cast<uint64_t>(mem_per_job_mb) * 1024ULL * 1024ULL;
        int ram_cap = static_cast<int>(mem.availBytes / per_job_bytes);
        if (ram_cap < 1) ram_cap = 1;
        if (ram_cap < cores) {
            b.workers = ram_cap;
            b.ramCapped = true;
            return b;
        }
    }

    b.workers = cores;
    b.ramCapped = false;
    return b;
}

bool Scheduler::IsSystemMemoryLow(double min_free_ratio) {
    SystemMemoryInfo mem = QuerySystemMemory();
    if (!mem.valid || mem.totalBytes == 0) return false;
    double ratio = static_cast<double>(mem.availBytes) / static_cast<double>(mem.totalBytes);
    return ratio < min_free_ratio;
}

void Scheduler::ApplyLowProcessPriority(int pid) {
    if (pid <= 0) return;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h) {
        SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
        CloseHandle(h);
    }
#else
    setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 10);
#endif
}

double Scheduler::ComputeTargetMsPerByte(
    const Target& target,
    const Project& project,
    const DepLog& dep_log,
    const std::filesystem::path& obj_dir,
    bool is_msvc
) {
    double total_ms = 0.0;
    double total_bytes = 0.0;
    std::error_code ec;

    for (const auto& f : target.GetCompiledFiles()) {
        std::string obj_key;
        std::filesystem::path obj_p = BuildRunner::ComputeObjectPath(project, obj_dir, f, is_msvc, obj_key);
        uint32_t dur = dep_log.GetDurationMs(obj_p.generic_u8string());
        if (dur > 0) {
            uint64_t fsz = std::filesystem::file_size(project.ResolvePath(f), ec);
            if (!ec && fsz > 0) {
                total_ms += static_cast<double>(dur);
                total_bytes += static_cast<double>(fsz);
            }
        }
    }

    if (total_bytes > 0.0 && total_ms > 0.0) {
        return total_ms / total_bytes;
    }
    return 0.05; // sensible default (~50ms per KB)
}

uint32_t Scheduler::EstimateCompileDurationMs(
    const std::filesystem::path& obj_path,
    const std::filesystem::path& src_path,
    const DepLog& dep_log,
    double target_ms_per_byte
) {
    uint32_t recorded = dep_log.GetDurationMs(obj_path.generic_u8string());
    if (recorded > 0) return recorded;

    std::error_code ec;
    uint64_t fsz = std::filesystem::file_size(src_path, ec);
    if (ec || fsz == 0) return 10;
    double est = static_cast<double>(fsz) * target_ms_per_byte;
    if (est < 1.0) est = 1.0;
    if (est > 600000.0) est = 600000.0;
    return static_cast<uint32_t>(est);
}

} // namespace core
