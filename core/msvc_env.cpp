#include "msvc_env.h"
#include "dep_log.h"

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core {

namespace {

std::string ToLowerMsvc(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string ExtractShortVersion(const std::string& full_ver) {
    size_t dot1 = full_ver.find('.');
    if (dot1 == std::string::npos) return full_ver;
    size_t dot2 = full_ver.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return full_ver;
    return full_ver.substr(0, dot2);
}

std::string ExtractVsYearOrShortLabel(const std::string& display_name, const std::string& full_ver) {
    if (display_name.find("2022") != std::string::npos || full_ver.rfind("17.", 0) == 0) return "2022";
    if (display_name.find("2019") != std::string::npos || full_ver.rfind("16.", 0) == 0) return "2019";
    if (display_name.find("2017") != std::string::npos || full_ver.rfind("15.", 0) == 0) return "2017";
    if (full_ver.rfind("18.", 0) == 0) return "2026";
    return ExtractShortVersion(full_ver);
}

std::string FindExeInDelimitedPath(const std::string& path_var, const std::string& exe_name) {
    if (path_var.empty() || exe_name.empty()) return "";
#if defined(_WIN32)
    const char delim = ';';
#else
    const char delim = ':';
#endif
    std::stringstream ss(path_var);
    std::string dir;
    std::error_code ec;
    while (std::getline(ss, dir, delim)) {
        if (dir.empty()) continue;
        std::filesystem::path p = std::filesystem::u8path(dir) / exe_name;
        if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
            return p.generic_u8string();
        }
    }
    return "";
}

std::string RunAndCaptureStdout(const std::vector<std::string>& args, int timeout_ms) {
    reproc::process proc;
    reproc::options opts;
    opts.redirect.out.type = reproc::redirect::pipe;
    opts.redirect.err.type = reproc::redirect::discard;
    opts.deadline = reproc::milliseconds(timeout_ms);
    if (proc.start(args, opts)) return "";

    std::string out;
    uint8_t buf[4096];
    while (true) {
        auto [n, ec] = proc.read(reproc::stream::out, buf, sizeof(buf));
        if (ec || n == 0) break;
        out.append(reinterpret_cast<const char*>(buf), n);
    }
    proc.wait(reproc::milliseconds(500));
    return out;
}

bool CaptureVcVarsForArch(
    const std::filesystem::path& vcvars_path,
    const std::string& arch,
    std::map<std::string, std::string>& out_env
) {
#if defined(_WIN32)
    std::string vc_str = vcvars_path.string();
    // Double-quoted outer command line for cmd.exe /s /c
    std::string inner = "\"\"" + vc_str + "\" " + arch + " >nul 2>&1 && set\"";
    std::vector<std::string> argv = { "cmd.exe", "/s", "/c", inner };
    std::string raw = RunAndCaptureStdout(argv, 12000);
    if (raw.empty()) return false;

    static const char* kWantedKeys[] = {
        "PATH", "INCLUDE", "LIB", "LIBPATH",
        "VCTOOLSVERSION", "VCTOOLSINSTALLDIR",
        "WINDOWSSDKDIR", "WINDOWSSDKVERSION",
        "UCRTVERSION", "VSINSTALLDIR"
    };
    static const char* kCanonicalNames[] = {
        "PATH", "INCLUDE", "LIB", "LIBPATH",
        "VCToolsVersion", "VCToolsInstallDir",
        "WindowsSdkDir", "WindowsSDKVersion",
        "UCRTVersion", "VSINSTALLDIR"
    };

    std::istringstream iss(raw);
    std::string line;
    bool got_include = false;
    while (std::getline(iss, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        std::string upper_key = key;
        std::transform(upper_key.begin(), upper_key.end(), upper_key.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        for (size_t i = 0; i < sizeof(kWantedKeys) / sizeof(kWantedKeys[0]); ++i) {
            if (upper_key == kWantedKeys[i]) {
                out_env[kCanonicalNames[i]] = val;
                if (std::string(kWantedKeys[i]) == "INCLUDE" && !val.empty()) {
                    got_include = true;
                }
                break;
            }
        }
    }
    return got_include && out_env.count("PATH") && out_env.count("LIB");
#else
    (void)vcvars_path;
    (void)arch;
    (void)out_env;
    return false;
#endif
}

} // namespace

std::filesystem::path MsvcEnvDetector::GetCacheFilePath() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? std::filesystem::u8path(appdata) : std::filesystem::current_path();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = xdg ? std::filesystem::u8path(xdg)
                                     : (home ? (std::filesystem::u8path(home) / ".config") : std::filesystem::current_path());
#endif
    return base / "GenieBuild" / "msvc_env.json";
}

MsvcEnvCacheState MsvcEnvDetector::LoadCache() {
    MsvcEnvCacheState state;
    std::filesystem::path p = GetCacheFilePath();
    std::ifstream in(p);
    if (!in.is_open()) return state;
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object()) return state;
        if (j.contains("bannerDismissed") && j["bannerDismissed"].is_boolean()) {
            state.bannerDismissed = j["bannerDismissed"].get<bool>();
        }
        if (j.contains("lastBannerText") && j["lastBannerText"].is_string()) {
            state.lastBannerText = j["lastBannerText"].get<std::string>();
        }
        if (j.contains("installs") && j["installs"].is_array()) {
            for (const auto& item : j["installs"]) {
                if (!item.is_object()) continue;
                MsvcInstallEnv m;
                m.displayName = item.value("displayName", "");
                m.installationPath = item.value("installationPath", "");
                m.installationVersion = item.value("installationVersion", "");
                m.shortVersion = item.value("shortVersion", "");
                m.arch = item.value("arch", "");
                m.profileName = item.value("profileName", "");
                m.vcvarsPath = item.value("vcvarsPath", "");
                m.vcvarsMtimeNs = item.value("vcvarsMtimeNs", static_cast<int64_t>(0));
                m.clPath = item.value("clPath", "");
                m.linkPath = item.value("linkPath", "");
                m.libPath = item.value("libPath", "");
                m.rcPath = item.value("rcPath", "");
                m.ml64Path = item.value("ml64Path", "");
                m.clangClPath = item.value("clangClPath", "");
                if (item.contains("env") && item["env"].is_object()) {
                    for (auto it = item["env"].begin(); it != item["env"].end(); ++it) {
                        if (it.value().is_string()) {
                            m.env[it.key()] = it.value().get<std::string>();
                        }
                    }
                }
                if (!m.profileName.empty() && !m.clPath.empty()) {
                    state.installs.push_back(std::move(m));
                }
            }
        }
    } catch (...) {}
    return state;
}

void MsvcEnvDetector::SaveCache(const MsvcEnvCacheState& state) {
    std::filesystem::path p = GetCacheFilePath();
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    nlohmann::json j = nlohmann::json::object();
    j["bannerDismissed"] = state.bannerDismissed;
    j["lastBannerText"] = state.lastBannerText;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : state.installs) {
        nlohmann::json item = nlohmann::json::object();
        item["displayName"] = m.displayName;
        item["installationPath"] = m.installationPath;
        item["installationVersion"] = m.installationVersion;
        item["shortVersion"] = m.shortVersion;
        item["arch"] = m.arch;
        item["profileName"] = m.profileName;
        item["vcvarsPath"] = m.vcvarsPath;
        item["vcvarsMtimeNs"] = m.vcvarsMtimeNs;
        item["clPath"] = m.clPath;
        item["linkPath"] = m.linkPath;
        item["libPath"] = m.libPath;
        item["rcPath"] = m.rcPath;
        item["ml64Path"] = m.ml64Path;
        item["clangClPath"] = m.clangClPath;
        item["env"] = m.env;
        arr.push_back(std::move(item));
    }
    j["installs"] = std::move(arr);

    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return;
        out << j.dump(2) << std::endl;
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

std::filesystem::path MsvcEnvDetector::FindVsWhere() {
#if defined(_WIN32)
    std::error_code ec;
    const char* pf86 = std::getenv("ProgramFiles(x86)");
    const char* pf = std::getenv("ProgramFiles");
    std::vector<std::filesystem::path> cands;
    if (pf86) {
        cands.push_back(std::filesystem::u8path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe");
    }
    cands.push_back(std::filesystem::path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"));
    if (pf) {
        cands.push_back(std::filesystem::u8path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe");
    }
    for (const auto& c : cands) {
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec)) {
            return c;
        }
    }
#endif
    return {};
}

MsvcEnvCacheState MsvcEnvDetector::DetectAndCaptureAll(bool force_refresh) {
    MsvcEnvCacheState cache = LoadCache();
#if !defined(_WIN32)
    (void)force_refresh;
    return cache;
#else
    std::filesystem::path vswhere = FindVsWhere();
    if (vswhere.empty()) {
        return cache;
    }

    // Check fast cache validity when !force_refresh (zero vcvarsall invocations if every cached vcvars matches)
    if (!force_refresh && !cache.installs.empty()) {
        bool all_valid = true;
        std::error_code ec;
        for (const auto& m : cache.installs) {
            std::filesystem::path vp = std::filesystem::u8path(m.vcvarsPath);
            if (!std::filesystem::exists(vp, ec) || !std::filesystem::exists(std::filesystem::u8path(m.clPath), ec)) {
                all_valid = false;
                break;
            }
            int64_t cur_ns = DepLog::StatFileNs(vp);
            if (cur_ns != m.vcvarsMtimeNs) {
                all_valid = false;
                break;
            }
        }
        if (all_valid) {
            return cache;
        }
    }

    std::vector<std::string> vswhere_args = {
        vswhere.string(),
        "-all",
        "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-format", "json",
        "-utf8"
    };
    std::string raw_json = RunAndCaptureStdout(vswhere_args, 5000);
    if (raw_json.empty()) {
        // Fallback query in case ARM64-only build tools are installed without x86.x64 component
        raw_json = RunAndCaptureStdout({
            vswhere.string(), "-all", "-products", "*", "-format", "json", "-utf8"
        }, 5000);
    }

    if (raw_json.empty()) {
        return cache;
    }

    nlohmann::json installs_json;
    try {
        installs_json = nlohmann::json::parse(raw_json);
    } catch (...) {
        return cache;
    }
    if (!installs_json.is_array()) {
        return cache;
    }

    std::vector<MsvcInstallEnv> updated;
    const char* kArches[] = { "x64", "x86", "arm64" };
    std::error_code ec;

    for (const auto& inst : installs_json) {
        if (!inst.is_object()) continue;
        std::string inst_path = inst.value("installationPath", "");
        std::string disp_name = inst.value("displayName", "Visual Studio");
        std::string inst_ver = inst.value("installationVersion", "");
        if (inst_path.empty()) continue;

        std::filesystem::path vcvars_p = std::filesystem::u8path(inst_path) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
        if (!std::filesystem::exists(vcvars_p, ec)) continue;
        int64_t mtime_ns = DepLog::StatFileNs(vcvars_p);
        std::string short_ver = ExtractShortVersion(inst_ver);
        std::string year_lbl = ExtractVsYearOrShortLabel(disp_name, inst_ver);

        for (const char* arch : kArches) {
            const MsvcInstallEnv* reused = nullptr;
            if (!force_refresh) {
                for (const auto& prev : cache.installs) {
                    if (ToLowerMsvc(prev.installationPath) == ToLowerMsvc(inst_path) &&
                        prev.installationVersion == inst_ver &&
                        prev.arch == arch &&
                        prev.vcvarsMtimeNs == mtime_ns &&
                        std::filesystem::exists(std::filesystem::u8path(prev.clPath), ec)) {
                        reused = &prev;
                        break;
                    }
                }
            }
            if (reused) {
                updated.push_back(*reused);
                continue;
            }

            std::map<std::string, std::string> captured_env;
            if (!CaptureVcVarsForArch(vcvars_p, arch, captured_env)) {
                continue;
            }

            std::string env_path = captured_env["PATH"];
            std::string cl_exe = FindExeInDelimitedPath(env_path, "cl.exe");
            if (cl_exe.empty()) continue;

            MsvcInstallEnv m;
            m.displayName = disp_name;
            m.installationPath = inst_path;
            m.installationVersion = inst_ver;
            m.shortVersion = short_ver;
            m.arch = arch;
            m.profileName = "MSVC " + year_lbl + " " + arch + (short_ver.empty() ? "" : (" (" + short_ver + ")"));
            m.vcvarsPath = vcvars_p.generic_u8string();
            m.vcvarsMtimeNs = mtime_ns;
            m.clPath = cl_exe;
            m.linkPath = FindExeInDelimitedPath(env_path, "link.exe");
            m.libPath = FindExeInDelimitedPath(env_path, "lib.exe");
            m.rcPath = FindExeInDelimitedPath(env_path, "rc.exe");
            m.ml64Path = FindExeInDelimitedPath(env_path, "ml64.exe");
            m.clangClPath = FindExeInDelimitedPath(env_path, "clang-cl.exe");
            m.env = std::move(captured_env);
            updated.push_back(std::move(m));
        }
    }

    if (!updated.empty()) {
        std::string btxt = "Found " + updated[0].displayName +
            (updated[0].shortVersion.empty() ? "" : (" (" + updated[0].shortVersion + ")")) +
            ". MSVC toolchains are ready.";
        if (btxt != cache.lastBannerText && !cache.bannerDismissed) {
            cache.lastBannerText = btxt;
        } else if (cache.lastBannerText.empty()) {
            cache.lastBannerText = btxt;
        }
    }
    cache.installs = std::move(updated);
    SaveCache(cache);
    return cache;
#endif
}

std::string MsvcEnvDetector::GetReadyBannerText(const MsvcEnvCacheState& state) {
    if (state.bannerDismissed || state.installs.empty()) return "";
    if (!state.lastBannerText.empty()) return state.lastBannerText;
    const auto& first = state.installs.front();
    return "Found " + first.displayName +
           (first.shortVersion.empty() ? "" : (" (" + first.shortVersion + ")")) +
           ". MSVC toolchains are ready.";
}

void MsvcEnvDetector::DismissBanner() {
    MsvcEnvCacheState st = LoadCache();
    st.bannerDismissed = true;
    SaveCache(st);
}

std::unordered_map<std::string, std::string> MsvcEnvDetector::BuildMergedProcessEnv(
    const std::map<std::string, std::string>& toolchain_env
) {
    std::unordered_map<std::string, std::string> out;
    if (toolchain_env.empty()) return out;

    const char* cur_path_raw = std::getenv("PATH");
    std::string cur_path = cur_path_raw ? cur_path_raw : "";
    std::string path_override;
    std::string path_prepend;

    for (const auto& [k, v] : toolchain_env) {
        if (k == "PATH+") {
            if (!v.empty()) {
                if (!path_prepend.empty()) path_prepend += ";";
                path_prepend += v;
            }
        } else if (ToLowerMsvc(k) == "path") {
            path_override = v;
        } else {
            out[k] = v;
        }
    }

#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    if (!path_prepend.empty() || !path_override.empty()) {
        std::string base = !path_override.empty() ? path_override : cur_path;
        if (!path_prepend.empty()) {
            out["PATH"] = base.empty() ? path_prepend : (path_prepend + sep + base);
        } else {
            out["PATH"] = base;
        }
    }
    return out;
}

} // namespace core
