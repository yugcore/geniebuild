#include "dep_log.h"
#include "fs_util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace core {

namespace {

constexpr char kMagic[4] = { 'G', 'B', 'D', 'L' };
constexpr uint32_t kFormatVersion = 1;

std::string ToLowerDep(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(p[i]) << (8 * i));
    }
    return v;
}

int64_t ReadI64(const uint8_t* p) {
    uint64_t u = ReadU64(p);
    int64_t s = 0;
    std::memcpy(&s, &u, sizeof(s));
    return s;
}

} // namespace

DepLog::~DepLog() {
    Close();
}

std::string DepLog::NormalizeDepPath(const std::string& path_str) {
    if (path_str.empty()) return "";
    // Fast lexical path avoids std::filesystem::path parsing on already-normalized clean paths
    if (path_str.back() != '/' && path_str.find('\\') == std::string::npos &&
        path_str.find("//") == std::string::npos && path_str.find("/.") == std::string::npos &&
        path_str.rfind("./", 0) != 0 && path_str != "." && path_str != "..") {
#if defined(_WIN32)
        return ToLowerDep(path_str);
#else
        return path_str;
#endif
    }
    std::filesystem::path p = std::filesystem::u8path(path_str).lexically_normal();
    std::string norm = p.generic_u8string();
    while (norm.rfind("./", 0) == 0) {
        norm = norm.substr(2);
    }
#if defined(_WIN32)
    norm = ToLowerDep(norm);
#endif
    return norm;
}

int64_t DepLog::FileTimeToNs(std::filesystem::file_time_type ft) {
    if (ft == std::filesystem::file_time_type::min()) return 0;
#if defined(_WIN32)
    // Convert std::filesystem::file_time_type to Unix epoch nanoseconds cleanly without int64 overflow
    static const int64_t kFileClockToUnixNs = []() -> int64_t {
        auto sys_now = std::chrono::system_clock::now();
        auto fc_now = std::filesystem::file_time_type::clock::now();
        int64_t sys_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys_now.time_since_epoch()).count();
        int64_t fc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(fc_now.time_since_epoch()).count();
        int64_t diff = sys_ns - fc_ns;
        constexpr int64_t kSec = 1000000000LL;
        return (diff >= 0) ? ((diff + kSec / 2) / kSec) * kSec : ((diff - kSec / 2) / kSec) * kSec;
    }();
    int64_t raw_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch()).count();
    return raw_ns + kFileClockToUnixNs;
#else
    auto dur = ft.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
#endif
}

int64_t DepLog::StatFileNs(const std::filesystem::path& path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    std::filesystem::path lp = fs_util::ToWinLongPath(path);
    if (!GetFileAttributesExW(lp.c_str(), GetFileExInfoStandard, &fad)) {
        return 0;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return 0;
    }
    ULARGE_INTEGER ui;
    ui.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    ui.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    constexpr uint64_t kWinToUnix100Ns = 116444736000000000ULL;
    return (ui.QuadPart >= kWinToUnix100Ns)
        ? static_cast<int64_t>(ui.QuadPart - kWinToUnix100Ns) * 100LL
        : -static_cast<int64_t>(kWinToUnix100Ns - ui.QuadPart) * 100LL;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
}

uint64_t DepLog::ParseHashU64(const std::string& hex_or_str) {
    if (hex_or_str.size() <= 16 && !hex_or_str.empty()) {
        bool all_hex = true;
        for (unsigned char c : hex_or_str) {
            if (!std::isxdigit(c)) {
                all_hex = false;
                break;
            }
        }
        if (all_hex) {
            try {
                return std::stoull(hex_or_str, nullptr, 16);
            } catch (...) {}
        }
    }
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : hex_or_str) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

bool DepLog::IsSystemHeader(const std::string& norm_path, const std::vector<std::string>& extra_sys_dirs) {
    std::string low = ToLowerDep(norm_path);
    for (const auto& dir : extra_sys_dirs) {
        std::string nd = ToLowerDep(NormalizeDepPath(dir));
        if (!nd.empty() && low.rfind(nd, 0) == 0) return true;
    }
#if defined(_WIN32)
    if (low.find("/program files/") != std::string::npos ||
        low.find("/program files (x86)/") != std::string::npos ||
        low.find("/windows kits/") != std::string::npos ||
        low.find("/msys64/") != std::string::npos ||
        low.find("/mingw64/") != std::string::npos ||
        low.find("/ucrt64/") != std::string::npos ||
        low.find("/lib/gcc/") != std::string::npos ||
        low.find("/lib/clang/") != std::string::npos) {
        return true;
    }
#else
    if (low.rfind("/usr/include/", 0) == 0 ||
        low.rfind("/usr/local/include/", 0) == 0 ||
        low.rfind("/usr/lib/gcc/", 0) == 0 ||
        low.rfind("/usr/lib/clang/", 0) == 0 ||
        low.rfind("/library/developer/", 0) == 0) {
        return true;
    }
#endif
    return false;
}

void DepLog::AppendU8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

void DepLog::AppendU16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void DepLog::AppendU32(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void DepLog::AppendU64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void DepLog::AppendI64(std::vector<uint8_t>& buf, int64_t v) {
    uint64_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    AppendU64(buf, u);
}

bool DepLog::Open(
    const std::filesystem::path& deps_bin_path,
    const std::vector<std::filesystem::path>& legacy_state_json_paths
) {
    Close();

    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_path = deps_bin_path;
    m_id_to_path.clear();
    m_path_to_id.clear();
    m_outputs.clear();
    m_steps.clear();
    m_legacy_hashes.clear();

    std::error_code ec;
    fs_util::CreateDirsLong(m_path.parent_path(), ec);

    bool loaded_bin = false;
    size_t total_records = 0;
    size_t valid_end_offset = 8;
    bool needs_truncate = false;

    if (fs_util::ExistsLong(m_path)) {
        std::ifstream in(fs_util::ToWinLongPath(m_path), std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            std::streamsize sz = in.tellg();
            in.seekg(0, std::ios::beg);
            if (sz >= 8) {
                std::vector<uint8_t> data(static_cast<size_t>(sz));
                if (in.read(reinterpret_cast<char*>(data.data()), sz)) {
                    if (std::memcmp(data.data(), kMagic, 4) == 0 && ReadU32(data.data() + 4) == kFormatVersion) {
                        loaded_bin = true;
                        size_t off = 8;
                        while (off < data.size()) {
                            uint8_t tag = data[off];
                            if (tag == static_cast<uint8_t>(RecordTag::Path)) {
                                if (off + 7 > data.size()) { needs_truncate = true; break; }
                                uint32_t id = ReadU32(data.data() + off + 1);
                                uint16_t len = ReadU16(data.data() + off + 5);
                                if (off + 7 + len > data.size() || id == 0) { needs_truncate = true; break; }
                                std::string s(reinterpret_cast<const char*>(data.data() + off + 7), len);
                                if (id > m_id_to_path.size()) m_id_to_path.resize(id);
                                m_id_to_path[id - 1] = s;
                                m_path_to_id[s] = id;
                                off += 7 + len;
                                valid_end_offset = off;
                                total_records++;
                            } else if (tag == static_cast<uint8_t>(RecordTag::Output)) {
                                if (off + 25 > data.size()) { needs_truncate = true; break; }
                                DepOutputEntry e;
                                e.outputId = ReadU32(data.data() + off + 1);
                                e.mtimeNs = ReadI64(data.data() + off + 5);
                                e.cmdHash = ReadU64(data.data() + off + 13);
                                e.durationMs = ReadU32(data.data() + off + 21);
                                if (off + 29 > data.size()) { needs_truncate = true; break; }
                                uint32_t dep_cnt = ReadU32(data.data() + off + 25);
                                if (dep_cnt > 1000000u || off + 29 + static_cast<size_t>(dep_cnt) * 4 > data.size()) {
                                    needs_truncate = true;
                                    break;
                                }
                                e.depIds.resize(dep_cnt);
                                for (uint32_t i = 0; i < dep_cnt; ++i) {
                                    e.depIds[i] = ReadU32(data.data() + off + 29 + i * 4);
                                }
                                m_outputs[e.outputId] = std::move(e);
                                off += 29 + static_cast<size_t>(dep_cnt) * 4;
                                valid_end_offset = off;
                                total_records++;
                            } else if (tag == static_cast<uint8_t>(RecordTag::Step)) {
                                if (off + 25 > data.size()) { needs_truncate = true; break; }
                                DepStepEntry s;
                                s.stepKeyId = ReadU32(data.data() + off + 1);
                                s.cmdHash = ReadU64(data.data() + off + 5);
                                s.outputsMtimeNs = ReadI64(data.data() + off + 13);
                                uint32_t in_cnt = ReadU32(data.data() + off + 21);
                                if (in_cnt > 1000000u || off + 25 + static_cast<size_t>(in_cnt) * 4 > data.size()) {
                                    needs_truncate = true;
                                    break;
                                }
                                s.inputIds.resize(in_cnt);
                                for (uint32_t i = 0; i < in_cnt; ++i) {
                                    s.inputIds[i] = ReadU32(data.data() + off + 25 + i * 4);
                                }
                                m_steps[s.stepKeyId] = std::move(s);
                                off += 25 + static_cast<size_t>(in_cnt) * 4;
                                valid_end_offset = off;
                                total_records++;
                            } else {
                                needs_truncate = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!loaded_bin) {
        std::ofstream out(fs_util::ToWinLongPath(m_path), std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            std::vector<uint8_t> hdr;
            hdr.insert(hdr.end(), kMagic, kMagic + 4);
            AppendU32(hdr, kFormatVersion);
            out.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        }
        // Migrate legacy state.json command hashes on first build
        for (const auto& sp : legacy_state_json_paths) {
            std::ifstream sf(sp);
            if (!sf.is_open()) continue;
            try {
                nlohmann::json j;
                sf >> j;
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        if (it.value().is_string()) {
                            m_legacy_hashes[it.key()] = ParseHashU64(it.value().get<std::string>());
                        }
                    }
                }
            } catch (...) {}
        }
    } else {
        size_t live_records = m_id_to_path.size() + m_outputs.size() + m_steps.size();
        bool dead_over_half = (total_records > 200) && (live_records * 2 < total_records);
        if (dead_over_half) {
            RewriteCompactedLocked();
        } else if (needs_truncate) {
            std::filesystem::resize_file(m_path, valid_end_offset, ec);
        }
    }

    StartWriterThread();
    return true;
}

bool DepLog::RewriteCompactedLocked() {
    std::filesystem::path tmp = m_path;
    tmp += ".tmp";
    std::vector<uint8_t> buf;
    buf.reserve(65536);
    buf.insert(buf.end(), kMagic, kMagic + 4);
    AppendU32(buf, kFormatVersion);

    for (size_t i = 0; i < m_id_to_path.size(); ++i) {
        const std::string& p = m_id_to_path[i];
        if (p.empty() || p.size() > 65535) continue;
        AppendU8(buf, static_cast<uint8_t>(RecordTag::Path));
        AppendU32(buf, static_cast<uint32_t>(i + 1));
        AppendU16(buf, static_cast<uint16_t>(p.size()));
        buf.insert(buf.end(), p.begin(), p.end());
    }
    for (const auto& [oid, e] : m_outputs) {
        AppendU8(buf, static_cast<uint8_t>(RecordTag::Output));
        AppendU32(buf, e.outputId);
        AppendI64(buf, e.mtimeNs);
        AppendU64(buf, e.cmdHash);
        AppendU32(buf, e.durationMs);
        AppendU32(buf, static_cast<uint32_t>(e.depIds.size()));
        for (uint32_t d : e.depIds) AppendU32(buf, d);
    }
    for (const auto& [sid, s] : m_steps) {
        AppendU8(buf, static_cast<uint8_t>(RecordTag::Step));
        AppendU32(buf, s.stepKeyId);
        AppendU64(buf, s.cmdHash);
        AppendI64(buf, s.outputsMtimeNs);
        AppendU32(buf, static_cast<uint32_t>(s.inputIds.size()));
        for (uint32_t in_id : s.inputIds) AppendU32(buf, in_id);
    }

    {
        std::ofstream out(fs_util::ToWinLongPath(tmp), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (!out.good()) return false;
    }

    std::error_code ec;
    return fs_util::RenameReplaceLong(tmp, m_path, ec);
}

void DepLog::StartWriterThread() {
    m_stop_requested = false;
    m_running = true;
    m_writer_thread = std::thread(&DepLog::WriterLoop, this);
}

void DepLog::Close() {
    if (!m_running) return;
    Flush();
    {
        std::lock_guard<std::mutex> ql(m_queue_mutex);
        m_stop_requested = true;
        m_queue_cv.notify_all();
    }
    if (m_writer_thread.joinable()) {
        m_writer_thread.join();
    }
    m_running = false;
}

void DepLog::Flush() {
    if (!m_running) return;
    std::unique_lock<std::mutex> ql(m_queue_mutex);
    m_flush_cv.wait(ql, [&]() { return m_write_queue.empty() && m_pending_flush_count == 0; });
}

void DepLog::EnqueueBytes(std::vector<uint8_t> bytes) {
    if (!m_running || bytes.empty()) return;
    std::lock_guard<std::mutex> ql(m_queue_mutex);
    m_write_queue.push_back(QueuedWrite{ std::move(bytes) });
    m_queue_cv.notify_one();
}

void DepLog::WriterLoop() {
    std::ofstream out(fs_util::ToWinLongPath(m_path), std::ios::binary | std::ios::app);
    while (true) {
        std::deque<QueuedWrite> batch;
        {
            std::unique_lock<std::mutex> ql(m_queue_mutex);
            m_queue_cv.wait(ql, [&]() { return m_stop_requested || !m_write_queue.empty(); });
            if (m_write_queue.empty() && m_stop_requested) {
                break;
            }
            batch.swap(m_write_queue);
            m_pending_flush_count = batch.size();
        }

        if (out.is_open()) {
            for (const auto& item : batch) {
                out.write(reinterpret_cast<const char*>(item.bytes.data()), static_cast<std::streamsize>(item.bytes.size()));
            }
            out.flush();
        }

        {
            std::lock_guard<std::mutex> ql(m_queue_mutex);
            m_pending_flush_count = 0;
            m_flush_cv.notify_all();
        }
    }
}

uint32_t DepLog::InternPath(const std::string& raw_or_norm_path) {
    std::string norm = NormalizeDepPath(raw_or_norm_path);
    if (norm.empty() || norm.size() > 65535) return 0;

    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto it = m_path_to_id.find(norm);
    if (it != m_path_to_id.end()) {
        return it->second;
    }

    uint32_t id = static_cast<uint32_t>(m_id_to_path.size() + 1);
    m_id_to_path.push_back(norm);
    m_path_to_id[norm] = id;

    std::vector<uint8_t> rec;
    rec.reserve(7 + norm.size());
    AppendU8(rec, static_cast<uint8_t>(RecordTag::Path));
    AppendU32(rec, id);
    AppendU16(rec, static_cast<uint16_t>(norm.size()));
    rec.insert(rec.end(), norm.begin(), norm.end());
    EnqueueBytes(std::move(rec));
    return id;
}

std::string DepLog::GetPath(uint32_t id) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (id == 0 || id > m_id_to_path.size()) return "";
    return m_id_to_path[id - 1];
}

uint32_t DepLog::FindPathId(const std::string& raw_or_norm_path) const {
    std::string norm = NormalizeDepPath(raw_or_norm_path);
    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto it = m_path_to_id.find(norm);
    return (it == m_path_to_id.end()) ? 0 : it->second;
}

bool DepLog::TryGetOutput(const std::string& output_path, DepOutputEntry& out_entry) const {
    std::string norm = NormalizeDepPath(output_path);
    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto pid_it = m_path_to_id.find(norm);
    if (pid_it == m_path_to_id.end()) return false;
    auto it = m_outputs.find(pid_it->second);
    if (it == m_outputs.end()) return false;
    out_entry = it->second;
    return true;
}

bool DepLog::TryGetStep(const std::string& step_key, DepStepEntry& out_entry) const {
    std::string norm = NormalizeDepPath(step_key);
    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto pid_it = m_path_to_id.find(norm);
    if (pid_it == m_path_to_id.end()) return false;
    auto it = m_steps.find(pid_it->second);
    if (it == m_steps.end()) return false;
    out_entry = it->second;
    return true;
}

uint32_t DepLog::GetDurationMs(const std::string& output_path) const {
    DepOutputEntry e;
    if (TryGetOutput(output_path, e)) return e.durationMs;
    return 0;
}

bool DepLog::TryGetLegacyHash(const std::string& key, uint64_t& out_hash) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto it = m_legacy_hashes.find(key);
    if (it == m_legacy_hashes.end()) return false;
    out_hash = it->second;
    return true;
}

void DepLog::RecordOutput(
    const std::string& output_path,
    int64_t mtime_ns,
    uint64_t cmd_hash,
    uint32_t duration_ms,
    const std::vector<std::string>& dep_paths
) {
    uint32_t out_id = InternPath(output_path);
    if (out_id == 0) return;

    std::vector<uint32_t> dep_ids;
    dep_ids.reserve(dep_paths.size());
    for (const auto& dp : dep_paths) {
        uint32_t did = InternPath(dp);
        if (did != 0) dep_ids.push_back(did);
    }

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        DepOutputEntry& e = m_outputs[out_id];
        e.outputId = out_id;
        e.mtimeNs = mtime_ns;
        e.cmdHash = cmd_hash;
        e.durationMs = duration_ms;
        e.depIds = dep_ids;
    }

    std::vector<uint8_t> rec;
    rec.reserve(29 + dep_ids.size() * 4);
    AppendU8(rec, static_cast<uint8_t>(RecordTag::Output));
    AppendU32(rec, out_id);
    AppendI64(rec, mtime_ns);
    AppendU64(rec, cmd_hash);
    AppendU32(rec, duration_ms);
    AppendU32(rec, static_cast<uint32_t>(dep_ids.size()));
    for (uint32_t d : dep_ids) AppendU32(rec, d);
    EnqueueBytes(std::move(rec));
}

void DepLog::RecordStep(
    const std::string& step_key,
    uint64_t cmd_hash,
    int64_t outputs_mtime_ns,
    const std::vector<std::string>& input_paths
) {
    uint32_t key_id = InternPath(step_key);
    if (key_id == 0) return;

    std::vector<uint32_t> in_ids;
    in_ids.reserve(input_paths.size());
    for (const auto& ip : input_paths) {
        uint32_t iid = InternPath(ip);
        if (iid != 0) in_ids.push_back(iid);
    }

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        DepStepEntry& s = m_steps[key_id];
        s.stepKeyId = key_id;
        s.cmdHash = cmd_hash;
        s.outputsMtimeNs = outputs_mtime_ns;
        s.inputIds = in_ids;
    }

    std::vector<uint8_t> rec;
    rec.reserve(25 + in_ids.size() * 4);
    AppendU8(rec, static_cast<uint8_t>(RecordTag::Step));
    AppendU32(rec, key_id);
    AppendU64(rec, cmd_hash);
    AppendI64(rec, outputs_mtime_ns);
    AppendU32(rec, static_cast<uint32_t>(in_ids.size()));
    for (uint32_t iid : in_ids) AppendU32(rec, iid);
    EnqueueBytes(std::move(rec));
}

size_t DepLog::GetTrackedOutputCount() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_outputs.size();
}

void DepLog::CollectTrackedStatPaths(
    const std::vector<std::string>& norm_output_paths,
    std::vector<std::string>& out_paths
) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (m_id_to_path.empty() || m_outputs.empty()) return;
    std::vector<uint8_t> seen(m_id_to_path.size() + 1, 0);
    for (const auto& norm : norm_output_paths) {
        auto pid_it = m_path_to_id.find(norm);
        if (pid_it == m_path_to_id.end()) continue;
        auto oit = m_outputs.find(pid_it->second);
        if (oit == m_outputs.end()) continue;
        for (uint32_t did : oit->second.depIds) {
            if (did > 0 && did <= m_id_to_path.size() && !seen[did]) {
                seen[did] = 1;
                out_paths.push_back(m_id_to_path[did - 1]);
            }
        }
    }
}

std::vector<int64_t> DepLog::BuildIdMtimeTable(
    const std::unordered_map<std::string, int64_t>& stat_map
) const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::vector<int64_t> table(m_id_to_path.size() + 1, 0);
    for (size_t idx = 0; idx < m_id_to_path.size(); ++idx) {
        auto it = stat_map.find(m_id_to_path[idx]);
        if (it != stat_map.end()) {
            table[idx + 1] = it->second;
        }
    }
    return table;
}

void DepLog::LookupOutputsBatch(
    const std::vector<std::string>& norm_output_paths,
    std::vector<DepOutputEntry>& out_entries
) const {
    out_entries.clear();
    out_entries.resize(norm_output_paths.size());
    std::lock_guard<std::mutex> lock(m_data_mutex);
    for (size_t i = 0; i < norm_output_paths.size(); ++i) {
        auto pid_it = m_path_to_id.find(norm_output_paths[i]);
        if (pid_it == m_path_to_id.end()) continue;
        auto oit = m_outputs.find(pid_it->second);
        if (oit != m_outputs.end()) {
            out_entries[i] = oit->second;
        }
    }
}

std::unordered_map<std::string, int64_t> DepLog::BatchStatFiles(
    const std::vector<std::string>& raw_or_norm_paths,
    int num_threads
) {
    struct TargetItem {
        std::string norm_path;
        std::string filename_key;
    };

    std::unordered_map<std::string, std::vector<TargetItem>> by_dir;
    std::unordered_set<std::string> seen_norm;
    seen_norm.reserve(raw_or_norm_paths.size());
    for (const auto& raw : raw_or_norm_paths) {
        std::string norm = NormalizeDepPath(raw);
        if (norm.empty() || !seen_norm.insert(norm).second) continue;
        std::filesystem::path p = std::filesystem::u8path(norm);
        std::string dir = p.parent_path().generic_u8string();
        std::string fname = p.filename().generic_u8string();
#if defined(_WIN32)
        fname = ToLowerDep(fname);
#endif
        by_dir[dir].push_back(TargetItem{ std::move(norm), std::move(fname) });
    }

    std::vector<std::string> dirs;
    dirs.reserve(by_dir.size());
    for (const auto& kv : by_dir) dirs.push_back(kv.first);

    std::unordered_map<std::string, int64_t> result;
    result.reserve(raw_or_norm_paths.size());
    if (dirs.empty()) return result;

    if (num_threads <= 0) {
        num_threads = static_cast<int>(std::thread::hardware_concurrency());
    }
    if (num_threads <= 0) num_threads = 4;
    if (num_threads > static_cast<int>(dirs.size())) {
        num_threads = static_cast<int>(dirs.size());
    }

    std::atomic<size_t> next_idx{0};
    std::mutex out_mutex;

    auto worker_fn = [&]() {
        std::vector<std::pair<std::string, int64_t>> local_results;
        while (true) {
            size_t idx = next_idx.fetch_add(1);
            if (idx >= dirs.size()) break;
            const std::string& dir_str = dirs[idx];
            const auto& items = by_dir[dir_str];

            std::unordered_map<std::string, int64_t> dir_mtimes;
#if defined(_WIN32)
            std::filesystem::path dir_p = dir_str.empty() ? std::filesystem::current_path() : std::filesystem::u8path(dir_str);
            std::wstring wglob = (fs_util::ToWinLongPath(dir_p) / L"*").wstring();
            WIN32_FIND_DATAW ffd;
            HANDLE hFind = FindFirstFileExW(
                wglob.c_str(),
                FindExInfoBasic,
                &ffd,
                FindExSearchNameMatch,
                nullptr,
                FIND_FIRST_EX_LARGE_FETCH
            );
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    // Convert UTF-16 filename to UTF-8 lowercase
                    int needed = WideCharToMultiByte(CP_UTF8, 0, ffd.cFileName, -1, nullptr, 0, nullptr, nullptr);
                    if (needed > 1) {
                        std::string fname(static_cast<size_t>(needed - 1), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, ffd.cFileName, -1, fname.data(), needed, nullptr, nullptr);
                        fname = ToLowerDep(fname);
                        ULARGE_INTEGER ui;
                        ui.LowPart = ffd.ftLastWriteTime.dwLowDateTime;
                        ui.HighPart = ffd.ftLastWriteTime.dwHighDateTime;
                        // Windows FILETIME (100ns since 1601-01-01) -> signed Unix epoch nanoseconds (1970-01-01)
                        // Avoid uint64 * 100 overflow by subtracting 116444736000000000ULL first.
                        constexpr uint64_t kWinToUnix100Ns = 116444736000000000ULL;
                        int64_t mtime_ns = (ui.QuadPart >= kWinToUnix100Ns)
                            ? static_cast<int64_t>(ui.QuadPart - kWinToUnix100Ns) * 100LL
                            : -static_cast<int64_t>(kWinToUnix100Ns - ui.QuadPart) * 100LL;
                        dir_mtimes[fname] = mtime_ns;
                    }
                } while (FindNextFileW(hFind, &ffd));
                FindClose(hFind);
            }
#else
            std::string dir_open = dir_str.empty() ? "." : dir_str;
            DIR* d = opendir(dir_open.c_str());
            if (d) {
                int dfd = dirfd(d);
                struct dirent* ent;
                while ((ent = readdir(d)) != nullptr) {
                    if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
                        continue;
                    }
                    struct stat st;
                    if (fstatat(dfd, ent->d_name, &st, 0) == 0 && S_ISREG(st.st_mode)) {
                        int64_t ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + static_cast<int64_t>(st.st_mtim.tv_nsec);
                        dir_mtimes[ent->d_name] = ns;
                    }
                }
                closedir(d);
            }
#endif
            for (const auto& item : items) {
                auto mit = dir_mtimes.find(item.filename_key);
                if (mit != dir_mtimes.end()) {
                    local_results.emplace_back(item.norm_path, mit->second);
                }
            }
        }

        std::lock_guard<std::mutex> lock(out_mutex);
        for (auto& kv : local_results) {
            result[std::move(kv.first)] = kv.second;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_threads));
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker_fn);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    return result;
}

std::vector<PchHeaderCandidate> DepLog::SuggestPchHeaders(
    const std::vector<std::string>& target_output_paths,
    const std::filesystem::path& project_root,
    double min_tu_ratio,
    size_t max_results
) const {
    std::unordered_map<uint32_t, size_t> header_counts;
    size_t total_tus = 0;

    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        for (const auto& out_p : target_output_paths) {
            std::string norm = NormalizeDepPath(out_p);
            auto pid_it = m_path_to_id.find(norm);
            if (pid_it == m_path_to_id.end()) continue;
            auto oit = m_outputs.find(pid_it->second);
            if (oit == m_outputs.end()) continue;
            total_tus++;
            std::unordered_set<uint32_t> seen_in_tu;
            for (uint32_t did : oit->second.depIds) {
                if (seen_in_tu.insert(did).second) {
                    header_counts[did]++;
                }
            }
        }
    }

    if (total_tus == 0) return {};

    size_t min_count = static_cast<size_t>(std::ceil(static_cast<double>(total_tus) * min_tu_ratio));
    if (min_count < 1) min_count = 1;

    std::string norm_root = NormalizeDepPath(project_root.generic_u8string());

    std::vector<PchHeaderCandidate> candidates;
    for (const auto& [did, cnt] : header_counts) {
        if (cnt < min_count) continue;
        std::string hpath = GetPath(did);
        if (hpath.empty()) continue;

        bool is_sys = IsSystemHeader(hpath);
        bool is_external = is_sys || (!norm_root.empty() && hpath.rfind(norm_root, 0) != 0);

        // Return relative path when inside project_root for cleaner UX
        std::string disp_path = hpath;
        if (!norm_root.empty() && hpath.rfind(norm_root + "/", 0) == 0) {
            disp_path = hpath.substr(norm_root.size() + 1);
        }

        PchHeaderCandidate c;
        c.path = disp_path;
        c.tuCount = cnt;
        c.isSystemOrExternal = is_external;
        candidates.push_back(std::move(c));
    }

    std::sort(candidates.begin(), candidates.end(), [](const PchHeaderCandidate& a, const PchHeaderCandidate& b) {
        if (a.tuCount != b.tuCount) return a.tuCount > b.tuCount;
        return a.path < b.path;
    });
    if (candidates.size() > max_results) {
        candidates.resize(max_results);
    }
    return candidates;
}

} // namespace core
