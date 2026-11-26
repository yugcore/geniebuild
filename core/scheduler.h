#pragma once

#include <cstdint>
#include <filesystem>

namespace core {

class Project;
class DepLog;
struct Target;

struct SystemMemoryInfo {
    bool valid = false;
    uint64_t totalBytes = 0;
    uint64_t availBytes = 0;
};

struct WorkerBudget {
    int workers = 4;
    int cores = 4;
    bool ramCapped = false;
    uint64_t availRamMb = 0;
    uint64_t totalRamMb = 0;
};

// V2-13 Smarter job scheduling helpers
class Scheduler {
public:
    static SystemMemoryInfo QuerySystemMemory();
    static WorkerBudget ComputeWorkerBudget(int requested_jobs, int mem_per_job_mb);
    static bool IsSystemMemoryLow(double min_free_ratio = 0.10);
    static void ApplyLowProcessPriority(int pid);

    // Learns average ms/byte for known compiled files in target from DepLog
    static double ComputeTargetMsPerByte(
        const Target& target,
        const Project& project,
        const DepLog& dep_log,
        const std::filesystem::path& obj_dir,
        bool is_msvc
    );

    // Predicts compile duration in ms (recorded duration in DepLog, or size_bytes * factor)
    static uint32_t EstimateCompileDurationMs(
        const std::filesystem::path& obj_path,
        const std::filesystem::path& src_path,
        const DepLog& dep_log,
        double target_ms_per_byte
    );
};

} // namespace core
