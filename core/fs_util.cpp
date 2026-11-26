#include "fs_util.h"
#include "compiler.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace core {
namespace fs_util {

std::wstring Utf8ToWide(const std::string& utf8) {
#if defined(_WIN32)
    if (utf8.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], needed);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string WideToUtf8(const std::wstring& wide) {
#if defined(_WIN32)
    if (wide.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], needed, nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

std::filesystem::path ToWinLongPath(const std::filesystem::path& p) {
#if defined(_WIN32)
    if (p.empty()) return p;
    std::error_code ec;
    std::filesystem::path abs_p = p.is_absolute() ? p : std::filesystem::absolute(p, ec);
    if (ec) abs_p = p;
    abs_p = abs_p.lexically_normal();

    std::wstring ws = abs_p.wstring();
    for (wchar_t& ch : ws) {
        if (ch == L'/') ch = L'\\';
    }
    if (ws.rfind(L"\\\\?\\", 0) == 0) {
        return std::filesystem::path(ws);
    }
    if (ws.size() < 240) {
        return std::filesystem::path(ws);
    }
    if (ws.rfind(L"\\\\", 0) == 0) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + ws.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + ws);
#else
    return p;
#endif
}

bool ExistsLong(const std::filesystem::path& p) {
    if (p.empty()) return false;
#if defined(_WIN32)
    DWORD attrs = GetFileAttributesW(ToWinLongPath(p).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    std::error_code ec;
    return std::filesystem::exists(p, ec) && !ec;
#endif
}

bool CreateDirsLong(const std::filesystem::path& p, std::error_code& ec) {
    ec.clear();
    if (p.empty()) return true;
#if defined(_WIN32)
    std::filesystem::path abs_p = p.is_absolute() ? p : std::filesystem::absolute(p, ec);
    if (ec) {
        ec.clear();
        abs_p = p;
    }
    abs_p = abs_p.lexically_normal();
    std::wstring w = abs_p.wstring();
    for (wchar_t& ch : w) {
        if (ch == L'/') ch = L'\\';
    }
    while (w.size() > 3 && w.back() == L'\\') {
        w.pop_back();
    }
    auto to_long = [](const std::wstring& s) -> std::wstring {
        if (s.rfind(L"\\\\?\\", 0) == 0) return s;
        if (s.size() < 240) return s;
        if (s.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + s.substr(2);
        return L"\\\\?\\" + s;
    };
    std::wstring full_long = to_long(w);
    DWORD full_attrs = GetFileAttributesW(full_long.c_str());
    if (full_attrs != INVALID_FILE_ATTRIBUTES) {
        if (full_attrs & FILE_ATTRIBUTE_DIRECTORY) return true;
        ec = std::make_error_code(std::errc::file_exists);
        return false;
    }

    size_t start = 0;
    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        size_t s1 = w.find(L'\\', 8);
        size_t s2 = (s1 != std::wstring::npos) ? w.find(L'\\', s1 + 1) : std::wstring::npos;
        start = (s2 != std::wstring::npos) ? (s2 + 1) : w.size();
    } else if (w.rfind(L"\\\\?\\", 0) == 0) {
        start = (w.size() >= 7 && w[5] == L':' && w[6] == L'\\') ? 7 : 4;
    } else if (w.rfind(L"\\\\", 0) == 0) {
        size_t s1 = w.find(L'\\', 2);
        size_t s2 = (s1 != std::wstring::npos) ? w.find(L'\\', s1 + 1) : std::wstring::npos;
        start = (s2 != std::wstring::npos) ? (s2 + 1) : w.size();
    } else if (w.size() >= 3 && w[1] == L':' && w[2] == L'\\') {
        start = 3;
    }

    for (size_t pos = start; pos <= w.size(); ++pos) {
        if (pos == w.size() || w[pos] == L'\\') {
            if (pos > start) {
                std::wstring sub_long = to_long(w.substr(0, pos));
                DWORD sa = GetFileAttributesW(sub_long.c_str());
                if (sa == INVALID_FILE_ATTRIBUTES) {
                    if (!CreateDirectoryW(sub_long.c_str(), nullptr)) {
                        DWORD err = GetLastError();
                        if (err != ERROR_ALREADY_EXISTS) {
                            ec = std::error_code(static_cast<int>(err), std::system_category());
                            return false;
                        }
                    }
                } else if (!(sa & FILE_ATTRIBUTE_DIRECTORY)) {
                    ec = std::make_error_code(std::errc::file_exists);
                    return false;
                }
            }
        }
    }
    return true;
#else
    return std::filesystem::create_directories(p, ec);
#endif
}

bool RemoveFileLong(const std::filesystem::path& p, std::error_code& ec) {
    ec.clear();
    if (p.empty()) return false;
#if defined(_WIN32)
    std::wstring lp = ToWinLongPath(p).wstring();
    DWORD attrs = GetFileAttributesW(lp.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (RemoveDirectoryW(lp.c_str())) return true;
    } else {
        if (DeleteFileW(lp.c_str())) return true;
    }
    DWORD err = GetLastError();
    ec = std::error_code(static_cast<int>(err), std::system_category());
    return false;
#else
    return std::filesystem::remove(p, ec);
#endif
}

bool RemoveAllLong(const std::filesystem::path& p, std::error_code& ec) {
    ec.clear();
    if (p.empty()) return true;
#if defined(_WIN32)
    std::wstring lp = ToWinLongPath(p).wstring();
    DWORD attrs = GetFileAttributesW(lp.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(lp.c_str()) != 0;
        return DeleteFileW(lp.c_str()) != 0;
    }
    WIN32_FIND_DATAW ffd;
    std::wstring glob = lp;
    if (!glob.empty() && glob.back() != L'\\') glob += L'\\';
    glob += L'*';
    HANDLE h = FindFirstFileExW(glob.c_str(), FindExInfoBasic, &ffd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
            std::filesystem::path child = p / ffd.cFileName;
            RemoveAllLong(child, ec);
        } while (FindNextFileW(h, &ffd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(lp.c_str())) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            ec = std::error_code(static_cast<int>(err), std::system_category());
            return false;
        }
    }
    return true;
#else
    std::filesystem::remove_all(p, ec);
    return !ec;
#endif
}

bool RenameReplaceLong(const std::filesystem::path& from_p, const std::filesystem::path& to_p, std::error_code& ec) {
    ec.clear();
#if defined(_WIN32)
    std::filesystem::path src = ToWinLongPath(from_p);
    std::filesystem::path dst = ToWinLongPath(to_p);
    if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    std::filesystem::rename(src, dst, ec);
    if (ec) {
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code rem_ec;
        std::filesystem::remove(src, rem_ec);
    }
    return !ec;
#else
    std::filesystem::rename(from_p, to_p, ec);
    return !ec;
#endif
}

bool CopyFileLong(const std::filesystem::path& from_p, const std::filesystem::path& to_p, std::error_code& ec) {
    ec.clear();
    return std::filesystem::copy_file(
        ToWinLongPath(from_p),
        ToWinLongPath(to_p),
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
}

bool ReadFileTextLong(const std::filesystem::path& p, std::string& out_text) {
    out_text.clear();
    std::ifstream f(ToWinLongPath(p), std::ios::binary);
    if (!f.is_open()) return false;
    out_text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileTextLong(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    if (!p.parent_path().empty()) {
        CreateDirsLong(p.parent_path(), ec);
    }
    std::ofstream f(ToWinLongPath(p), std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

bool AtomicWriteTextIfChanged(
    const std::filesystem::path& target_path,
    const std::string& content,
    bool* out_written
) {
    if (out_written) *out_written = false;
    if (target_path.empty()) return false;

    std::string existing;
    if (ExistsLong(target_path) && ReadFileTextLong(target_path, existing)) {
        if (existing == content) {
            return true; // Unchanged: do not touch mtime
        }
    }

    std::error_code ec;
    if (!target_path.parent_path().empty()) {
        CreateDirsLong(target_path.parent_path(), ec);
    }
    std::filesystem::path tmp_path = target_path;
    tmp_path += ".tmp";

    if (!WriteFileTextLong(tmp_path, content)) {
        return false;
    }
    if (!RenameReplaceLong(tmp_path, target_path, ec)) {
        std::error_code rec;
        RemoveFileLong(tmp_path, rec);
        return false;
    }
    if (out_written) *out_written = true;
    return true;
}

bool IsWindowsLongPathsEnabled() {
#if defined(_WIN32)
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
            0,
            KEY_READ,
            &hKey) != ERROR_SUCCESS) {
        return true; // Do not warn falsely if registry key cannot be queried
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    LONG rc = RegQueryValueExW(hKey, L"LongPathsEnabled", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    return value == 1;
#else
    return true;
#endif
}

std::string Hash16Hex(const std::string& text) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::filesystem::path ComputeSafeObjectPath(
    const std::filesystem::path& obj_dir,
    const std::string& sanitized_rel_with_ext,
    const std::string& raw_rel_key
) {
    std::filesystem::path normal_obj = obj_dir / sanitized_rel_with_ext;
    std::string full_s = normal_obj.generic_u8string();
    // Reserve headroom for ".d" / ".json" and long working dirs (spec V2-22: >= 200 chars)
    if (full_s.size() < 200) {
        return normal_obj;
    }

    std::filesystem::path rel_p(sanitized_rel_with_ext);
    std::string ext = rel_p.extension().generic_u8string();
    std::string stem = rel_p.stem().generic_u8string();
    if (stem.size() > 40) stem = stem.substr(0, 40);
    std::string h16 = Hash16Hex(raw_rel_key.empty() ? sanitized_rel_with_ext : raw_rel_key);
    return obj_dir / "h" / (h16 + "_" + stem + ext);
}

std::string MakeRelativeArgPath(
    const std::filesystem::path& abs_or_rel,
    const std::filesystem::path& working_dir
) {
    if (abs_or_rel.empty() || working_dir.empty()) {
        return abs_or_rel.generic_u8string();
    }
    std::error_code ec;
    std::filesystem::path abs_p = abs_or_rel.is_absolute() ? abs_or_rel : (working_dir / abs_or_rel);
    std::filesystem::path rel = std::filesystem::relative(abs_p.lexically_normal(), working_dir.lexically_normal(), ec);
    if (!ec) {
        std::string r = rel.generic_u8string();
        if (!r.empty() && r.rfind("..", 0) != 0) {
            return r;
        }
    }
    return abs_or_rel.generic_u8string();
}

bool HasMinGwLongArgRisk(const BuildCommand& cmd, std::string& out_long_arg) {
    out_long_arg.clear();
    if (cmd.program.size() >= 250) {
        out_long_arg = cmd.program;
        return true;
    }
    for (const auto& a : cmd.arguments) {
        if (a.size() >= 250) {
            // Strip common -I / -MF / -o / -D prefixes? Even total flag+path >= 250 on MinGW risks CreateProcess/stat limits.
            out_long_arg = a;
            return true;
        }
    }
    return false;
}

} // namespace fs_util
} // namespace core
