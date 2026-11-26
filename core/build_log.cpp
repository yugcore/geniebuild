#include "build_log.h"
#include "fs_util.h"
#include "target.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace core {

namespace {

std::string MakeTimestampSlug() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_WIN32)
    localtime_s(&tm_val, &tt);
#else
    localtime_r(&tt, &tm_val);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                  tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return std::string(buf);
}

std::string ToLowerLog(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool ContainsCaseInsensitive(const std::string& hay, const std::string& needle_low) {
    if (needle_low.empty()) return true;
    std::string hlow = ToLowerLog(hay);
    return hlow.find(needle_low) != std::string::npos;
}

bool ExtractProgressTargetName(const std::string& text, std::string& out_target) {
    // Matches "[12/120] [target] path" or "[pch] target (C++)" or "[step] target/step"
    out_target.clear();
    if (text.rfind("[pch] ", 0) == 0) {
        size_t sp = text.find(' ', 6);
        out_target = (sp != std::string::npos) ? text.substr(6, sp - 6) : text.substr(6);
        return !out_target.empty();
    }
    if (text.rfind("[step] ", 0) == 0) {
        size_t sl = text.find('/', 7);
        out_target = (sl != std::string::npos) ? text.substr(7, sl - 7) : text.substr(7);
        return !out_target.empty();
    }
    if (!text.empty() && text[0] == '[') {
        size_t close1 = text.find(']');
        if (close1 != std::string::npos && close1 + 2 < text.size() && text[close1 + 1] == ' ' && text[close1 + 2] == '[') {
            size_t close2 = text.find(']', close1 + 3);
            if (close2 != std::string::npos) {
                out_target = text.substr(close1 + 3, close2 - (close1 + 3));
                return !out_target.empty();
            }
        }
    }
    return false;
}

} // namespace

BuildLog::BuildLog() = default;

BuildLog::~BuildLog() {
    JoinSearchThread();
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_out_stream.is_open()) {
        m_out_stream.flush();
        m_out_stream.close();
    }
}

void BuildLog::JoinSearchThread() {
    m_abort_search.store(true);
    if (m_search_thread.joinable()) {
        m_search_thread.join();
    }
    m_abort_search.store(false);
}

void BuildLog::PruneOldLogs(size_t keep_count) const {
    std::error_code ec;
    if (m_logs_dir.empty() || !fs_util::ExistsLong(m_logs_dir)) return;

    struct LogFile {
        std::filesystem::path p;
        std::filesystem::file_time_type mt;
    };
    std::vector<LogFile> files;
    for (const auto& entry : std::filesystem::directory_iterator(fs_util::ToWinLongPath(m_logs_dir), ec)) {
        if (ec) break;
        std::error_code tec;
        if (entry.is_regular_file(tec) && entry.path().extension() == ".log") {
            files.push_back({ entry.path(), entry.last_write_time(tec) });
        }
    }
    if (files.size() <= keep_count) return;
    std::sort(files.begin(), files.end(), [](const LogFile& a, const LogFile& b) {
        return a.mt > b.mt; // newest first
    });
    for (size_t i = keep_count; i < files.size(); ++i) {
        std::error_code rec;
        fs_util::RemoveFileLong(files[i].p, rec);
    }
}

bool BuildLog::BeginSession(const std::filesystem::path& project_root, const std::string& config) {
    JoinSearchThread();
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_out_stream.is_open()) {
        m_out_stream.flush();
        m_out_stream.close();
    }

    m_entries.clear();
    m_error_lines.clear();
    m_warning_lines.clear();
    m_targets.clear();
    m_target_map.clear();
    m_jobs.clear();
    m_job_idx_by_id.clear();
    m_active_job_id = 0;
    m_next_job_id = 1;
    m_write_offset = 0;

    {
        std::lock_guard<std::mutex> clk(m_cache_mutex);
        m_lru_list.clear();
        m_lru_map.clear();
        if (m_read_stream.is_open()) m_read_stream.close();
        m_read_stream_path.clear();
    }
    {
        std::lock_guard<std::mutex> slk(m_search_mutex);
        m_search_query.clear();
        m_search_matches.clear();
    }

    m_project_root = project_root;
    if (project_root.empty()) {
        m_logs_dir.clear();
        m_current_log_path.clear();
        m_last_log_path.clear();
        return false;
    }

    m_logs_dir = project_root / ".geniebuild" / "logs";
    std::error_code ec;
    fs_util::CreateDirsLong(m_logs_dir, ec);
    PruneOldLogs(9); // Keep last 10 including this new one

    std::string cfg_tok = SanitizeConfigDirName(config.empty() ? "Debug" : config);
    m_current_log_path = m_logs_dir / (MakeTimestampSlug() + "_" + cfg_tok + ".log");
    m_last_log_path = project_root / ".geniebuild" / "last_build.log";

    m_out_stream.open(fs_util::ToWinLongPath(m_current_log_path), std::ios::binary | std::ios::trunc);
    return m_out_stream.is_open();
}

void BuildLog::EndSession() {
    std::filesystem::path src_p;
    std::filesystem::path dst_p;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_out_stream.is_open()) {
            m_out_stream.flush();
            m_out_stream.close();
        }
        src_p = m_current_log_path;
        dst_p = m_last_log_path;
    }
    if (!src_p.empty() && !dst_p.empty() && fs_util::ExistsLong(src_p)) {
        std::error_code ec;
        fs_util::CopyFileLong(src_p, dst_p, ec);
    }
}

void BuildLog::Clear() {
    JoinSearchThread();
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_out_stream.is_open()) {
        m_out_stream.flush();
        m_out_stream.close();
    }
    m_entries.clear();
    m_error_lines.clear();
    m_warning_lines.clear();
    m_targets.clear();
    m_target_map.clear();
    m_jobs.clear();
    m_job_idx_by_id.clear();
    m_active_job_id = 0;
    m_next_job_id = 1;
    m_write_offset = 0;
    m_current_log_path.clear();
    {
        std::lock_guard<std::mutex> clk(m_cache_mutex);
        m_lru_list.clear();
        m_lru_map.clear();
        if (m_read_stream.is_open()) m_read_stream.close();
        m_read_stream_path.clear();
    }
    {
        std::lock_guard<std::mutex> slk(m_search_mutex);
        m_search_query.clear();
        m_search_matches.clear();
    }
}

uint16_t BuildLog::InternTargetLocked(const std::string& target_name) {
    if (target_name.empty()) return 0;
    auto it = m_target_map.find(target_name);
    if (it != m_target_map.end()) return it->second;
    if (m_targets.size() >= 65530) return 0;
    uint16_t idx = static_cast<uint16_t>(m_targets.size() + 1); // 1-based; 0 = unassigned
    m_targets.push_back(target_name);
    m_target_map[target_name] = idx;
    return idx;
}

void BuildLog::AppendLines(const std::vector<OutLine>& lines) {
    if (lines.empty()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_entries.reserve(m_entries.size() + lines.size());

    std::string write_batch;
    write_batch.reserve(lines.size() * 96);

    uint16_t cur_target_idx = 0;
    for (const auto& ol : lines) {
        size_t line_idx = m_entries.size();
        if (ol.kind == OutLineKind::Progress) {
            std::string tname;
            if (ExtractProgressTargetName(ol.text, tname)) {
                cur_target_idx = InternTargetLocked(tname);
            }
            m_active_job_id = m_next_job_id++;
            LogJobBlock jb;
            jb.jobId = m_active_job_id;
            jb.header = ol.text;
            jb.firstLine = line_idx;
            jb.lineCount = 1;
            jb.collapsed = true;
            m_job_idx_by_id[jb.jobId] = m_jobs.size();
            m_jobs.push_back(std::move(jb));
        } else if (ol.kind == OutLineKind::Status) {
            m_active_job_id = 0;
        }

        LogLineEntry meta;
        meta.offset = m_write_offset;
        meta.length = static_cast<uint32_t>(std::min<size_t>(ol.text.size(), 0xFFFFFFFFu));
        meta.kind = static_cast<uint8_t>(ol.kind);
        meta.flags = (ol.has_error ? 1 : 0) | (ol.has_warning ? 2 : 0);
        meta.targetIdx = cur_target_idx;
        meta.jobId = (ol.kind == OutLineKind::Status) ? 0 : m_active_job_id;

        if (m_active_job_id != 0 && ol.kind != OutLineKind::Progress && ol.kind != OutLineKind::Status) {
            auto jit = m_job_idx_by_id.find(m_active_job_id);
            if (jit != m_job_idx_by_id.end()) {
                LogJobBlock& jb = m_jobs[jit->second];
                jb.lineCount++;
                if (!ol.text.empty()) {
                    jb.hasOutput = true;
                    jb.collapsed = false; // Auto-expand jobs with actual compiler output (V2-20)
                }
                if (ol.has_error) { jb.hasError = true; jb.collapsed = false; }
                if (ol.has_warning) { jb.hasWarning = true; jb.collapsed = false; }
            }
        }

        if (ol.has_error) m_error_lines.push_back(static_cast<uint32_t>(line_idx));
        if (ol.has_warning) m_warning_lines.push_back(static_cast<uint32_t>(line_idx));

        m_entries.push_back(meta);
        PutLruLocked(line_idx, ol.text);

        std::string raw_rec = (ol.kind == OutLineKind::Command ? "> " : "") + ol.text + "\n";
        // Store exact offset & length matching what's written to disk for seamless seekg reloads
        m_entries.back().offset = m_write_offset + (ol.kind == OutLineKind::Command ? 2ULL : 0ULL);
        write_batch += raw_rec;
        m_write_offset += raw_rec.size();
    }

    if (m_out_stream.is_open() && !write_batch.empty()) {
        m_out_stream.write(write_batch.data(), static_cast<std::streamsize>(write_batch.size()));
        m_out_stream.flush();
    }
}

bool BuildLog::LoadFromFile(const std::filesystem::path& file_path) {
    JoinSearchThread();
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_out_stream.is_open()) {
        m_out_stream.flush();
        m_out_stream.close();
    }
    m_entries.clear();
    m_error_lines.clear();
    m_warning_lines.clear();
    m_targets.clear();
    m_target_map.clear();
    m_jobs.clear();
    m_job_idx_by_id.clear();
    m_active_job_id = 0;
    m_next_job_id = 1;
    m_current_log_path = file_path;
    m_write_offset = 0;

    {
        std::lock_guard<std::mutex> clk(m_cache_mutex);
        m_lru_list.clear();
        m_lru_map.clear();
        if (m_read_stream.is_open()) m_read_stream.close();
        m_read_stream_path.clear();
    }

    std::ifstream in(fs_util::ToWinLongPath(file_path), std::ios::binary);
    if (!in.is_open()) return false;

    in.seekg(0, std::ios::end);
    uint64_t fsize = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    m_entries.reserve(static_cast<size_t>(std::min<uint64_t>(fsize / 64 + 1024, 3000000ULL)));

    constexpr size_t BUF_SZ = 1 << 20; // 1 MB sequential scan chunk
    std::vector<char> buf(BUF_SZ);
    std::string line_buf;
    uint64_t line_start = 0;
    uint64_t pos = 0;

    auto flush_line = [&](uint64_t end_off) {
        size_t len = line_buf.size();
        bool trim_cr = (len > 0 && line_buf.back() == '\r');
        if (trim_cr) line_buf.pop_back();

        bool herr = false;
        bool hwarn = false;
        BuildRunner::ClassifyDiagnosticLine(line_buf, herr, hwarn);

        LogLineEntry e;
        e.offset = line_start;
        e.length = static_cast<uint32_t>(line_buf.size());
        e.kind = static_cast<uint8_t>(OutLineKind::Raw);
        e.flags = (herr ? 1 : 0) | (hwarn ? 2 : 0);
        if (line_buf.rfind("> ", 0) == 0) {
            e.kind = static_cast<uint8_t>(OutLineKind::Command);
            e.offset += 2;
            if (e.length >= 2) e.length -= 2;
        } else if (!line_buf.empty() && line_buf[0] == '[' && line_buf.find(']') != std::string::npos) {
            e.kind = static_cast<uint8_t>(OutLineKind::Progress);
        }

        uint32_t idx = static_cast<uint32_t>(m_entries.size());
        if (herr) m_error_lines.push_back(idx);
        if (hwarn) m_warning_lines.push_back(idx);
        m_entries.push_back(e);

        line_buf.clear();
        line_start = end_off;
    };

    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        for (std::streamsize i = 0; i < got; ++i) {
            char ch = buf[static_cast<size_t>(i)];
            ++pos;
            if (ch == '\n') {
                flush_line(pos);
            } else if (line_buf.size() < 32768) {
                line_buf.push_back(ch);
            }
        }
    }
    if (!line_buf.empty()) {
        flush_line(pos);
    }
    m_write_offset = pos;
    return true;
}

void BuildLog::PutLruLocked(size_t line_idx, const std::string& text) const {
    std::lock_guard<std::mutex> clk(m_cache_mutex);
    auto it = m_lru_map.find(line_idx);
    if (it != m_lru_map.end()) {
        it->second->second = text;
        m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second);
        return;
    }
    m_lru_list.emplace_front(line_idx, text);
    m_lru_map[line_idx] = m_lru_list.begin();
    if (m_lru_list.size() > MAX_LRU_LINES) {
        auto last = std::prev(m_lru_list.end());
        m_lru_map.erase(last->first);
        m_lru_list.pop_back();
    }
}

std::string BuildLog::ReadLineUncached(const LogLineEntry& meta) const {
    if (meta.length == 0 || m_current_log_path.empty()) return "";
    std::lock_guard<std::mutex> clk(m_cache_mutex);
    if (!m_read_stream.is_open() || m_read_stream_path != m_current_log_path) {
        if (m_read_stream.is_open()) m_read_stream.close();
        m_read_stream.clear();
        m_read_stream.open(fs_util::ToWinLongPath(m_current_log_path), std::ios::binary);
        m_read_stream_path = m_current_log_path;
    }
    if (!m_read_stream.is_open()) return "";
    m_read_stream.clear();
    m_read_stream.seekg(static_cast<std::streamoff>(meta.offset), std::ios::beg);
    std::string out(meta.length, '\0');
    m_read_stream.read(&out[0], static_cast<std::streamsize>(meta.length));
    std::streamsize got = m_read_stream.gcount();
    if (got < static_cast<std::streamsize>(meta.length)) {
        out.resize(static_cast<size_t>(std::max<std::streamsize>(0, got)));
    }
    return out;
}

bool BuildLog::GetLineMetaAndText(size_t line_idx, LogLineEntry& out_meta, std::string& out_text) const {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (line_idx >= m_entries.size()) return false;
        out_meta = m_entries[line_idx];
    }

    if (out_meta.length == 0) {
        out_text.clear();
        return true;
    }

    {
        std::lock_guard<std::mutex> clk(m_cache_mutex);
        auto it = m_lru_map.find(line_idx);
        if (it != m_lru_map.end()) {
            out_text = it->second->second;
            m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second);
            return true;
        }
    }

    out_text = ReadLineUncached(out_meta);
    PutLruLocked(line_idx, out_text);
    return true;
}

size_t BuildLog::GetTotalLines() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_entries.size();
}

size_t BuildLog::GetErrorCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_error_lines.size();
}

size_t BuildLog::GetWarningCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_warning_lines.size();
}

std::vector<std::string> BuildLog::GetTargetNames() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_targets;
}

std::vector<uint32_t> BuildLog::GetFilteredIndices(
    LogFilterMode mode,
    const std::string& target_filter,
    const std::string& file_filter
) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    uint16_t want_target = 0;
    if (!target_filter.empty()) {
        auto it = m_target_map.find(target_filter);
        if (it == m_target_map.end()) return {};
        want_target = it->second;
    }

    std::vector<uint32_t> result;
    std::string f_low = ToLowerLog(file_filter);

    auto accept_line = [&](uint32_t idx) -> bool {
        const auto& e = m_entries[idx];
        if (want_target != 0 && e.targetIdx != want_target) return false;
        if (!f_low.empty()) {
            std::string txt;
            {
                std::lock_guard<std::mutex> clk(m_cache_mutex);
                auto it = m_lru_map.find(idx);
                if (it != m_lru_map.end()) txt = it->second->second;
            }
            if (txt.empty() && e.length > 0) txt = ReadLineUncached(e);
            if (!ContainsCaseInsensitive(txt, f_low)) return false;
        }
        return true;
    };

    if (mode == LogFilterMode::Errors) {
        result.reserve(m_error_lines.size());
        for (uint32_t idx : m_error_lines) {
            if (accept_line(idx)) result.push_back(idx);
        }
    } else if (mode == LogFilterMode::Warnings) {
        result.reserve(m_warning_lines.size());
        for (uint32_t idx : m_warning_lines) {
            if (accept_line(idx)) result.push_back(idx);
        }
    } else if (want_target != 0 || !f_low.empty()) {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            uint32_t idx = static_cast<uint32_t>(i);
            if (accept_line(idx)) result.push_back(idx);
        }
    }
    return result;
}

size_t BuildLog::FindNextError(size_t current_line, bool forward) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_error_lines.empty()) return static_cast<size_t>(-1);
    if (forward) {
        auto it = std::upper_bound(m_error_lines.begin(), m_error_lines.end(), static_cast<uint32_t>(current_line));
        if (it == m_error_lines.end()) return m_error_lines.front();
        return *it;
    } else {
        auto it = std::lower_bound(m_error_lines.begin(), m_error_lines.end(), static_cast<uint32_t>(current_line));
        if (it == m_error_lines.begin()) return m_error_lines.back();
        --it;
        return *it;
    }
}

std::vector<LogJobBlock> BuildLog::GetJobBlocksSnapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_jobs;
}

void BuildLog::SetJobCollapsed(uint32_t job_id, bool collapsed) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_job_idx_by_id.find(job_id);
    if (it != m_job_idx_by_id.end()) {
        m_jobs[it->second].collapsed = collapsed;
    }
}

void BuildLog::StartSearch(const std::string& query, bool case_sensitive) {
    JoinSearchThread();
    {
        std::lock_guard<std::mutex> slk(m_search_mutex);
        m_search_query = query;
        m_search_matches.clear();
    }
    if (query.empty()) return;

    m_searching.store(true);
    std::filesystem::path log_p;
    size_t total_n = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        log_p = m_current_log_path;
        total_n = m_entries.size();
    }

    m_search_thread = std::thread([this, query, case_sensitive, log_p, total_n]() {
        std::string needle = case_sensitive ? query : ToLowerLog(query);
        std::vector<uint32_t> found;
        std::ifstream f(fs_util::ToWinLongPath(log_p), std::ios::binary);
        for (size_t i = 0; i < total_n && !m_abort_search.load(); ++i) {
            LogLineEntry meta;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (i >= m_entries.size()) break;
                meta = m_entries[i];
            }
            if (meta.length == 0) continue;
            std::string line;
            if (f.is_open()) {
                f.seekg(static_cast<std::streamoff>(meta.offset), std::ios::beg);
                line.resize(meta.length);
                f.read(&line[0], static_cast<std::streamsize>(meta.length));
            } else {
                LogLineEntry dm;
                GetLineMetaAndText(i, dm, line);
            }
            bool match = case_sensitive
                ? (line.find(needle) != std::string::npos)
                : (ToLowerLog(line).find(needle) != std::string::npos);
            if (match) {
                found.push_back(static_cast<uint32_t>(i));
                if (found.size() % 512 == 0) {
                    std::lock_guard<std::mutex> slk(m_search_mutex);
                    m_search_matches = found;
                }
            }
        }
        {
            std::lock_guard<std::mutex> slk(m_search_mutex);
            m_search_matches = std::move(found);
        }
        m_searching.store(false);
    });
}

std::vector<uint32_t> BuildLog::GetSearchMatches() const {
    std::lock_guard<std::mutex> slk(m_search_mutex);
    return m_search_matches;
}

std::string BuildLog::GetActiveSearchQuery() const {
    std::lock_guard<std::mutex> slk(m_search_mutex);
    return m_search_query;
}

std::filesystem::path BuildLog::GetCurrentLogFile() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_current_log_path;
}

std::filesystem::path BuildLog::GetLogsDirectory() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_logs_dir;
}

} // namespace core
