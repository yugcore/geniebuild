#include "hints.h"
#include "fs_util.h"
#include "child_process.h"
#include "build_steps.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <oleauto.h>
#endif

namespace core {

namespace {

std::string NormalizeRootKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
    std::string s = (ec ? p.lexically_normal() : c).generic_u8string();
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
    return s;
}

std::string TrimStr(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool ParseIntPrefix(const std::string& s, size_t pos, int& out_val, size_t& out_next) {
    if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
    long v = 0;
    size_t i = pos;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        if (v > 10000000) break;
        ++i;
    }
    out_val = static_cast<int>(v);
    out_next = i;
    return i > pos;
}

} // namespace

std::filesystem::path HintsManager::GetUserSettingsPath() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? std::filesystem::u8path(appdata) : std::filesystem::temp_directory_path();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = xdg ? std::filesystem::u8path(xdg)
                                     : (home ? (std::filesystem::u8path(home) / ".config") : std::filesystem::temp_directory_path());
#endif
    return base / "GenieBuild" / "user_settings.json";
}

UserSettings HintsManager::LoadUserSettings() {
    UserSettings s;
    std::filesystem::path p = GetUserSettingsPath();
    std::ifstream f(fs_util::ToWinLongPath(p));
    if (!f.is_open()) return s;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("editor") && j["editor"].is_string()) s.editorCommand = j["editor"].get<std::string>();
        if (j.contains("showPerformanceHints") && j["showPerformanceHints"].is_boolean()) {
            s.showPerformanceHints = j["showPerformanceHints"].get<bool>();
        }
        if (j.contains("defenderDismissedGlobal") && j["defenderDismissedGlobal"].is_boolean()) {
            s.defenderDismissedGlobal = j["defenderDismissedGlobal"].get<bool>();
        }
        if (j.contains("defenderDismissedProjects") && j["defenderDismissedProjects"].is_array()) {
            for (const auto& item : j["defenderDismissedProjects"]) {
                if (item.is_string()) s.defenderDismissedProjects.push_back(item.get<std::string>());
            }
        }
        if (j.contains("longPathBannerDismissed") && j["longPathBannerDismissed"].is_boolean()) {
            s.longPathBannerDismissed = j["longPathBannerDismissed"].get<bool>();
        }
    } catch (...) {
    }
    return s;
}

void HintsManager::SaveUserSettings(const UserSettings& s) {
    nlohmann::json j = nlohmann::json::object();
    j["editor"] = s.editorCommand;
    j["showPerformanceHints"] = s.showPerformanceHints;
    j["defenderDismissedGlobal"] = s.defenderDismissedGlobal;
    j["defenderDismissedProjects"] = s.defenderDismissedProjects;
    j["longPathBannerDismissed"] = s.longPathBannerDismissed;
    fs_util::WriteFileTextLong(GetUserSettingsPath(), j.dump(2) + "\n");
}

bool HintsManager::IsWindowsDefenderRealTimeActive() {
#if defined(_WIN32)
    HRESULT hr_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool should_uninit = SUCCEEDED(hr_init);
    if (hr_init == RPC_E_CHANGED_MODE) {
        should_uninit = false;
    } else if (FAILED(hr_init)) {
        return false;
    }

    bool active = false;
    IWbemLocator* pLoc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<LPVOID*>(&pLoc));
    if (SUCCEEDED(hr) && pLoc) {
        IWbemServices* pSvc = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Defender");
        hr = pLoc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
        SysFreeString(ns);
        if (SUCCEEDED(hr) && pSvc) {
            CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                              RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            BSTR wql = SysAllocString(L"WQL");
            BSTR qry = SysAllocString(L"SELECT RealTimeProtectionEnabled FROM MSFT_MpComputerStatus");
            IEnumWbemClassObject* pEnum = nullptr;
            hr = pSvc->ExecQuery(wql, qry, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
            SysFreeString(wql);
            SysFreeString(qry);
            if (SUCCEEDED(hr) && pEnum) {
                IWbemClassObject* pObj = nullptr;
                ULONG uRet = 0;
                if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &uRet) == S_OK && pObj) {
                    VARIANT vtProp;
                    VariantInit(&vtProp);
                    if (SUCCEEDED(pObj->Get(L"RealTimeProtectionEnabled", 0, &vtProp, nullptr, nullptr))) {
                        if (vtProp.vt == VT_BOOL) {
                            active = (vtProp.boolVal == VARIANT_TRUE);
                        }
                    }
                    VariantClear(&vtProp);
                    pObj->Release();
                }
                pEnum->Release();
            }
            pSvc->Release();
        }
        pLoc->Release();
    }

    if (should_uninit) {
        CoUninitialize();
    }
    return active;
#else
    return false;
#endif
}

bool HintsManager::IsPathOnDevDrive(const std::filesystem::path& path) {
#if defined(_WIN32)
    if (path.empty()) return false;
    std::wstring abs_w = fs_util::Utf8ToWide(std::filesystem::absolute(path).generic_u8string());
    wchar_t vol_root[MAX_PATH * 2]{};
    if (!GetVolumePathNameW(abs_w.c_str(), vol_root, static_cast<DWORD>(std::size(vol_root)))) {
        return false;
    }
    wchar_t fs_name[64]{};
    if (!GetVolumeInformationW(vol_root, nullptr, 0, nullptr, nullptr, nullptr, fs_name, static_cast<DWORD>(std::size(fs_name)))) {
        return false;
    }
    if (_wcsicmp(fs_name, L"ReFS") != 0) {
        return false;
    }

    // Remove trailing slash for CreateFileW volume handle
    std::wstring vol_dev = L"\\\\.\\";
    std::wstring vr(vol_root);
    while (!vr.empty() && (vr.back() == L'\\' || vr.back() == L'/')) vr.pop_back();
    if (vr.size() == 2 && vr[1] == L':') {
        vol_dev += vr;
    } else {
        return false;
    }

    HANDLE hVol = CreateFileW(vol_dev.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        return false;
    }

#ifndef FSCTL_QUERY_PERSISTENT_VOLUME_STATE
#define FSCTL_QUERY_PERSISTENT_VOLUME_STATE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 143, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef PERSISTENT_VOLUME_STATE_DEV_VOLUME
#define PERSISTENT_VOLUME_STATE_DEV_VOLUME 0x00002000
#endif
    struct FileFsPersistentVolInfo {
        ULONG VolumeFlags;
        ULONG FlagMask;
        ULONG Version;
        ULONG Reserved;
    };
    FileFsPersistentVolInfo req{};
    req.FlagMask = PERSISTENT_VOLUME_STATE_DEV_VOLUME;
    req.Version = 1;
    FileFsPersistentVolInfo resp{};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(hVol, FSCTL_QUERY_PERSISTENT_VOLUME_STATE, &req, sizeof(req), &resp, sizeof(resp), &bytes, nullptr);
    CloseHandle(hVol);
    return ok && ((resp.VolumeFlags & PERSISTENT_VOLUME_STATE_DEV_VOLUME) != 0);
#else
    (void)path;
    return false;
#endif
}

bool HintsManager::ShouldSuggestDefenderExclusion(
    const std::filesystem::path& project_root,
    size_t compiled_file_count,
    const UserSettings& settings
) {
#if !defined(_WIN32)
    (void)project_root; (void)compiled_file_count; (void)settings;
    return false;
#else
    if (!settings.showPerformanceHints || settings.defenderDismissedGlobal) return false;
    if (compiled_file_count <= 500) return false;
    std::string key = NormalizeRootKey(project_root);
    for (const auto& d : settings.defenderDismissedProjects) {
        if (d == key) return false;
    }
    if (IsPathOnDevDrive(project_root)) return false;
    return IsWindowsDefenderRealTimeActive();
#endif
}

void HintsManager::DismissDefenderForProject(const std::filesystem::path& project_root, bool forever_all_projects) {
    UserSettings s = LoadUserSettings();
    if (forever_all_projects) {
        s.defenderDismissedGlobal = true;
    } else if (!project_root.empty()) {
        std::string key = NormalizeRootKey(project_root);
        if (std::find(s.defenderDismissedProjects.begin(), s.defenderDismissedProjects.end(), key) == s.defenderDismissedProjects.end()) {
            s.defenderDismissedProjects.push_back(std::move(key));
        }
    }
    SaveUserSettings(s);
}

bool HintsManager::ParseDiagnosticLocation(
    const std::string& raw_line,
    const std::filesystem::path& project_root,
    std::filesystem::path& out_file,
    int& out_line,
    int& out_col
) {
    out_file.clear();
    out_line = 0;
    out_col = 1;
    std::string s = TrimStr(raw_line);
    if (s.empty()) return false;

    auto resolve_candidate = [&](const std::string& path_part) -> bool {
        std::string trimmed = TrimStr(path_part);
        if (trimmed.empty() || trimmed.size() > 512) return false;
        std::filesystem::path cand = std::filesystem::u8path(trimmed);
        if (!cand.is_absolute() && !project_root.empty()) {
            cand = (project_root / cand).lexically_normal();
        }
        if (fs_util::ExistsLong(cand)) {
            out_file = cand;
            return true;
        }
        return false;
    };

    // Form 1: path(line) or path(line,col) (MSVC / linker)
    for (size_t i = 2; i < s.size(); ++i) {
        if (s[i] == '(') {
            int ln = 0;
            size_t next = 0;
            if (ParseIntPrefix(s, i + 1, ln, next) && ln > 0) {
                int col = 1;
                size_t close_pos = next;
                if (next < s.size() && s[next] == ',') {
                    int c2 = 1;
                    size_t next2 = next + 1;
                    if (ParseIntPrefix(s, next2, c2, close_pos) && c2 > 0) col = c2;
                }
                if (close_pos < s.size() && s[close_pos] == ')') {
                    if (resolve_candidate(s.substr(0, i))) {
                        out_line = ln;
                        out_col = col;
                        return true;
                    }
                }
            }
        }
    }

    // Form 2: path:line or path:line:col (GCC / Clang, handling C:/... drive prefix cleanly)
    for (size_t i = 2; i < s.size(); ++i) {
        if (s[i] == ':' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            int ln = 0;
            size_t next = 0;
            if (ParseIntPrefix(s, i + 1, ln, next) && ln > 0) {
                int col = 1;
                if (next < s.size() && s[next] == ':') {
                    int c2 = 1;
                    size_t next2 = next + 1;
                    if (ParseIntPrefix(s, next2, c2, next2) && c2 > 0) col = c2;
                }
                if (resolve_candidate(s.substr(0, i))) {
                    out_line = ln;
                    out_col = col;
                    return true;
                }
            }
        }
    }
    return false;
}

bool HintsManager::LaunchEditorForLocation(
    const std::string& editor_template,
    const std::filesystem::path& file_path,
    int line,
    int col
) {
    if (file_path.empty()) return false;
    std::string fstr = file_path.generic_u8string();
    std::string tmpl = TrimStr(editor_template);
    if (tmpl.empty()) {
        OpenPathInShell(file_path);
        return true;
    }

    auto replace_all = [](std::string& target, const std::string& tok, const std::string& rep) {
        size_t pos = 0;
        while ((pos = target.find(tok, pos)) != std::string::npos) {
            target.replace(pos, tok.size(), rep);
            pos += rep.size();
        }
    };
    std::string expanded = tmpl;
    std::string quoted_f = (fstr.find(' ') != std::string::npos) ? ("\"" + fstr + "\"") : fstr;
    replace_all(expanded, "{file}", quoted_f);
    replace_all(expanded, "{line}", std::to_string(std::max(1, line)));
    replace_all(expanded, "{col}", std::to_string(std::max(1, col)));

    BuildCommand cmd = BuildStepCommand(expanded, false);
    if (cmd.program.empty()) return false;
    ChildProcess proc;
    ProcessOptions opts;
    opts.workingDir = file_path.parent_path();
    if (!proc.Start(cmd, opts)) {
        return false;
    }
    proc.Detach();
    return true;
}

void HintsManager::OpenPathInShell(const std::filesystem::path& target) {
#if defined(_WIN32)
    std::wstring wt = fs_util::ToWinLongPath(target).wstring();
    ShellExecuteW(nullptr, L"open", wt.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    BuildCommand cmd;
#if defined(__APPLE__)
    cmd.program = "open";
#else
    cmd.program = "xdg-open";
#endif
    cmd.arguments = { target.generic_u8string() };
    ChildProcess proc;
    ProcessOptions opts;
    proc.Start(cmd, opts);
#endif
}

} // namespace core
