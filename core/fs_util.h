#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace core {

struct BuildCommand;

namespace fs_util {

// Converts a path to Windows extended-length \\?\ form when normalized absolute length >= 240 chars.
// No-op on POSIX or short paths.
std::filesystem::path ToWinLongPath(const std::filesystem::path& p);

// UTF-8 <-> UTF-16 helpers for Win32 API calls
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// Long-path safe filesystem operations
bool ExistsLong(const std::filesystem::path& p);
bool CreateDirsLong(const std::filesystem::path& p, std::error_code& ec);
bool RemoveFileLong(const std::filesystem::path& p, std::error_code& ec);
bool RemoveAllLong(const std::filesystem::path& p, std::error_code& ec);
bool RenameReplaceLong(const std::filesystem::path& from_p, const std::filesystem::path& to_p, std::error_code& ec);
bool CopyFileLong(const std::filesystem::path& from_p, const std::filesystem::path& to_p, std::error_code& ec);
bool ReadFileTextLong(const std::filesystem::path& p, std::string& out_text);
bool WriteFileTextLong(const std::filesystem::path& p, const std::string& text);

// Atomic write via temp file + replace, ONLY touching target file if contents differ (V2-19).
bool AtomicWriteTextIfChanged(
    const std::filesystem::path& target_path,
    const std::string& content,
    bool* out_written = nullptr
);

// V2-22: Reads HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled (true if 1 or non-Windows).
bool IsWindowsLongPathsEnabled();

// V2-22: Computes deterministic FNV-1a 64-bit 16-hex hash
std::string Hash16Hex(const std::string& text);

// V2-22: Computes sanitized object path inside obj_dir; compacts to obj_dir/h/<hash16>_<stem><ext>
// when full path length >= 200 chars.
std::filesystem::path ComputeSafeObjectPath(
    const std::filesystem::path& obj_dir,
    const std::string& sanitized_rel_with_ext,
    const std::string& raw_rel_key
);

// Returns relative path (forward slashes) from working_dir when cleanly under working_dir, else generic path
std::string MakeRelativeArgPath(
    const std::filesystem::path& abs_or_rel,
    const std::filesystem::path& working_dir
);

// V2-22: Returns true if any argument looks like a path >= 250 chars on MinGW/GCC toolchains
bool HasMinGwLongArgRisk(const BuildCommand& cmd, std::string& out_long_arg);

} // namespace fs_util
} // namespace core
