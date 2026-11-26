#include "unity.h"
#include "project.h"
#include "target.h"
#include "settings_resolver.h"
#include "dep_log.h"
#include "pch.h"
#include "source_rules.h"
#include "tool_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>

namespace core {

namespace {

std::string Hash8Unity(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    uint32_t folded = static_cast<uint32_t>((h >> 32) ^ (h & 0xFFFFFFFFu));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", folded);
    return std::string(buf);
}

std::string ComputeSettingsGroupHash(const EffectiveFileSettings* fe, bool is_c, bool is_msvc) {
    if (!fe) return "default";
    uint64_t h = 14695981039346656037ULL;
    auto feed = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        h ^= 0xFF; h *= 1099511628211ULL;
    };
    for (const auto& inc : fe->base.includeDirs) feed("I:" + inc);
    for (const auto& sinc : fe->base.systemIncludeDirs) feed("S:" + sinc);
    for (const auto& def : fe->base.defines) feed("D:" + def);
    for (const auto& cf : fe->base.compileFlags) feed("C:" + cf);
    feed(is_c ? fe->base.cStd : fe->base.cxxStd);
    feed(fe->base.warnings);
    feed(fe->optimize);
    feed(fe->debugInfo);
    if (is_msvc) feed(fe->runtime);
    feed(fe->lto);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::filesystem::path GetHotStatePath(const Project& project, const Target& target, const std::string& config) {
    return project.GetRootPath() / ".geniebuild" / "unity" / SanitizeConfigDirName(config) / target.name / "hot_state.json";
}

std::unordered_map<std::string, int64_t> LoadHotState(const std::filesystem::path& p) {
    std::unordered_map<std::string, int64_t> m;
    std::ifstream in(p);
    if (!in.is_open()) return m;
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_number_integer()) {
                    m[it.key()] = it.value().get<int64_t>();
                }
            }
        }
    } catch (...) {}
    return m;
}

} // namespace

UnityPlan UnityManager::PlanTargetUnity(
    const Project& project,
    const Target& target,
    const std::vector<std::string>& compiled_files,
    const std::unordered_map<std::string, std::shared_ptr<const EffectiveFileSettings>>& file_settings,
    const std::unordered_map<std::string, bool>& file_uses_pch,
    const std::string& config,
    const std::string& compiler_name,
    bool is_msvc,
    const DepLog* dep_log,
    bool write_files
) {
    (void)compiler_name;
    UnityPlan plan;
    if (!target.unity.enabled) {
        plan.standaloneFiles = compiled_files;
        return plan;
    }

    size_t batch_size = target.unity.batchSize > 0 ? target.unity.batchSize : 16;

    std::vector<CompiledGlob> unity_excludes;
    for (const auto& pat : target.unity.exclude) {
        CompiledGlob cg = CompiledGlob::Compile(pat);
        if (cg.IsValid()) unity_excludes.push_back(std::move(cg));
    }

    std::filesystem::path unity_dir = project.GetRootPath() / ".geniebuild" / "unity" / SanitizeConfigDirName(config) / target.name;
    std::string unity_rel_dir = ".geniebuild/unity/" + SanitizeConfigDirName(config) + "/" + target.name;
    if (write_files) {
        std::error_code ec;
        std::filesystem::create_directories(unity_dir, ec);
    }

    // Load adaptive hot state & last batch build mtimes
    std::unordered_map<std::string, int64_t> prev_hot = target.unity.adaptive
        ? LoadHotState(GetHotStatePath(project, target, config))
        : std::unordered_map<std::string, int64_t>{};

    // Determine the most recent successful batch/target timestamp from dep_log (if available)
    int64_t last_unity_build_ns = 0;
    if (target.unity.adaptive && dep_log) {
        std::error_code ec;
        if (std::filesystem::exists(unity_dir, ec) && !ec) {
            for (const auto& ent : std::filesystem::directory_iterator(unity_dir, ec)) {
                std::string fn = ent.path().filename().generic_u8string();
                if (fn == "hot_state.json") continue;
                std::string obj_cand = (target.GetObjectDir(project.GetRootPath(), config) /
                    (Hash8Unity(unity_rel_dir + "/" + fn) + "_" + fn + (is_msvc ? ".obj" : ".o"))).generic_u8string();
                DepOutputEntry doe;
                if (dep_log->TryGetOutput(obj_cand, doe) && doe.mtimeNs > last_unity_build_ns) {
                    last_unity_build_ns = doe.mtimeNs;
                }
            }
        }
    }

    struct GroupKey {
        std::string folder;
        bool isC = false;
        bool usesPch = false;
        std::string sigHash;
        bool operator<(const GroupKey& o) const {
            if (folder != o.folder) return folder < o.folder;
            if (isC != o.isC) return isC < o.isC;
            if (usesPch != o.usesPch) return usesPch < o.usesPch;
            return sigHash < o.sigHash;
        }
    };

    struct GroupBucket {
        std::vector<std::string> files;
        std::shared_ptr<const EffectiveFileSettings> settings;
    };

    std::map<GroupKey, GroupBucket> groups;

    for (const auto& f : compiled_files) {
        SourceFileKind kind = ToolRegistry::ClassifySourceFile(f);
        if (kind != SourceFileKind::C && kind != SourceFileKind::Cxx) {
            plan.standaloneFiles.push_back(f);
            continue;
        }

        // Check unity.exclude globs
        bool excluded = false;
        for (const auto& cg : unity_excludes) {
            if (cg.Matches(f)) {
                excluded = true;
                break;
            }
        }
        if (excluded) {
            plan.standaloneFiles.push_back(f);
            continue;
        }

        // Check per-file excludeFromUnity or per-file overrides
        std::shared_ptr<const EffectiveFileSettings> fe;
        auto fit = file_settings.find(f);
        if (fit != file_settings.end()) fe = fit->second;
        if (fe && fe->excludeFromUnity) {
            plan.standaloneFiles.push_back(f);
            continue;
        }

        // Adaptive hot-file tracking:
        // If file was modified since last batch build (or modified while hot), compile standalone.
        // If file was hot on the previous build and has NOT been modified since, graduate back to batch!
        if (target.unity.adaptive) {
            int64_t f_ns = DepLog::StatFileNs(project.ResolvePath(f));
            auto hot_it = prev_hot.find(f);
            bool is_hot = false;
            if (hot_it != prev_hot.end()) {
                // Still hot only if edited again since last hot build!
                if (f_ns > hot_it->second) {
                    is_hot = true;
                }
            } else if (last_unity_build_ns > 0 && f_ns > last_unity_build_ns) {
                is_hot = true;
            }
            if (is_hot) {
                plan.hotFiles.push_back(f);
                plan.standaloneFiles.push_back(f);
                continue;
            }
        }

        bool is_c = (kind == SourceFileKind::C);
        bool uses_pch = false;
        auto pit = file_uses_pch.find(f);
        if (pit != file_uses_pch.end()) uses_pch = pit->second;

        GroupKey gk;
        gk.folder = std::filesystem::u8path(f).parent_path().generic_u8string();
        gk.isC = is_c;
        gk.usesPch = uses_pch;
        gk.sigHash = ComputeSettingsGroupHash(fe.get(), is_c, is_msvc);

        GroupBucket& bucket = groups[gk];
        bucket.files.push_back(f);
        if (!bucket.settings) bucket.settings = fe;
    }

    size_t batch_idx = 0;
    for (auto& [gk, bucket] : groups) {
        std::sort(bucket.files.begin(), bucket.files.end());
        std::string folder_hash = Hash8Unity(gk.folder + "|" + (gk.isC ? "c" : "cxx") + "|" + gk.sigHash + (gk.usesPch ? "|pch" : ""));

        for (size_t off = 0; off < bucket.files.size(); off += batch_size) {
            size_t end = std::min(bucket.files.size(), off + batch_size);
            if (end - off == 1) {
                // Single file in chunk: compile directly without unity wrapper overhead
                plan.standaloneFiles.push_back(bucket.files[off]);
                continue;
            }

            size_t chunk_k = off / batch_size;
            std::string ext = gk.isC ? ".c" : ".cpp";
            std::string fname = folder_hash + "_" + std::to_string(chunk_k) + ext;

            UnityBatch batch;
            batch.batchName = target.name + "_" + std::to_string(batch_idx++);
            batch.generatedFileRel = unity_rel_dir + "/" + fname;
            batch.generatedFileAbs = unity_dir / fname;
            batch.isC = gk.isC;
            batch.usesPch = gk.usesPch;
            batch.settings = bucket.settings;
            batch.memberFiles.assign(bucket.files.begin() + static_cast<ptrdiff_t>(off),
                                     bucket.files.begin() + static_cast<ptrdiff_t>(end));

            if (write_files) {
                std::ostringstream body;
                body << "// GenieBuild unity batch — do not edit\n";
                for (const auto& mf : batch.memberFiles) {
                    std::string abs_s = project.ResolvePath(mf).generic_u8string();
                    body << "#include \"" << abs_s << "\"\n";
                }
                PchManager::WriteFileIfDifferent(batch.generatedFileAbs, body.str());
            }

            plan.batches.push_back(std::move(batch));
        }
    }

    plan.totalBatchCount = plan.batches.size();
    return plan;
}

void UnityManager::SaveAdaptiveHotState(
    const Project& project,
    const Target& target,
    const std::string& config,
    const UnityPlan& plan
) {
    if (!target.unity.enabled || !target.unity.adaptive) return;
    std::filesystem::path hp = GetHotStatePath(project, target, config);
    std::error_code ec;
    std::filesystem::create_directories(hp.parent_path(), ec);

    nlohmann::json j = nlohmann::json::object();
    for (const auto& hf : plan.hotFiles) {
        int64_t ns = DepLog::StatFileNs(project.ResolvePath(hf));
        j[hf] = ns;
    }

    std::ofstream out(hp, std::ios::trunc);
    if (out.is_open()) {
        out << j.dump(2) << std::endl;
    }
}

} // namespace core
