#include "toolchain.h"
#include "project.h"

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace core {

namespace {

std::string ToLowerTc(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string ReplaceRootVar(std::string s, const std::string& root_fwd) {
    if (root_fwd.empty()) return s;
    const std::string token = "${root}";
    size_t pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
        s.replace(pos, token.size(), root_fwd);
        pos += root_fwd.size();
    }
    return s;
}

std::string Hash8Tc(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    uint32_t f = static_cast<uint32_t>((h >> 32) ^ (h & 0xFFFFFFFFu));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", f);
    return std::string(buf);
}

std::string QueryExeOutput(
    const std::string& exe,
    const std::vector<std::string>& args,
    const std::unordered_map<std::string, std::string>* extra_env = nullptr,
    int timeout_ms = 2500
) {
    if (exe.empty()) return "";
    reproc::process proc;
    reproc::options opts;
    opts.redirect.out.type = reproc::redirect::pipe;
    opts.redirect.err.type = reproc::redirect::stdout_;
    opts.deadline = reproc::milliseconds(timeout_ms);
    if (extra_env && !extra_env->empty()) {
        opts.env.behavior = reproc::env::extend;
        opts.env.extra = *extra_env;
    }

    std::vector<std::string> argv;
    argv.push_back(exe);
    for (const auto& a : args) argv.push_back(a);
    if (proc.start(argv, opts)) return "";

    std::string out;
    uint8_t buf[2048];
    while (true) {
        auto [n, ec] = proc.read(reproc::stream::out, buf, sizeof(buf));
        if (ec || n == 0) break;
        out.append(reinterpret_cast<const char*>(buf), n);
        if (out.size() > 8192) break;
    }
    proc.wait(reproc::milliseconds(500));
    return out;
}

std::string FirstNonEmptyLine(const std::string& text) {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) return line;
    }
    return "";
}

void InferOsArchFromTriple(const std::string& triple_raw, std::string& out_os, std::string& out_arch) {
    std::string t = ToLowerTc(triple_raw);
    if (t.find("aarch64") != std::string::npos || t.find("arm64") != std::string::npos) {
        out_arch = "arm64";
    } else if (t.find("x86_64") != std::string::npos || t.find("amd64") != std::string::npos) {
        out_arch = "x64";
    } else if (t.find("i686") != std::string::npos || t.find("i386") != std::string::npos || t.find("i586") != std::string::npos) {
        out_arch = "x86";
    } else if (t.find("arm") != std::string::npos) {
        out_arch = "arm";
    }

    if (t.find("mingw") != std::string::npos || t.find("windows") != std::string::npos || t.find("msvc") != std::string::npos) {
        out_os = "windows";
    } else if (t.find("linux") != std::string::npos) {
        out_os = "linux";
    } else if (t.find("apple") != std::string::npos || t.find("darwin") != std::string::npos) {
        out_os = "macos";
    } else if (t.find("-elf") != std::string::npos || t.find("eabi") != std::string::npos || t.find("-none") != std::string::npos) {
        out_os = "none";
    }
}

std::string HostOsName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string HostArchName() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(__arm__)
    return "arm";
#else
    return "x64";
#endif
}

std::string FindSiblingExe(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
#if defined(_WIN32)
    for (const char* ext : { ".exe", "" }) {
        std::filesystem::path p = dir / (name + ext);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
            return p.generic_u8string();
        }
    }
#else
    std::filesystem::path p = dir / name;
    if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
        return p.generic_u8string();
    }
#endif
    return "";
}

std::string ShortenVersionDigits(const std::string& banner) {
    // Extract e.g. "16.1.0" or "19.1.0"
    for (size_t i = 0; i < banner.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(banner[i]))) {
            size_t j = i;
            int dots = 0;
            while (j < banner.size() && (std::isdigit(static_cast<unsigned char>(banner[j])) || banner[j] == '.')) {
                if (banner[j] == '.') dots++;
                j++;
            }
            if (dots >= 1 && j > i + 2) {
                std::string ver = banner.substr(i, j - i);
                while (!ver.empty() && ver.back() == '.') ver.pop_back();
                return ver;
            }
            i = j;
        }
    }
    return "";
}

} // namespace

std::string ToolchainFamilyToString(ToolchainFamily fam) {
    switch (fam) {
        case ToolchainFamily::Gcc:     return "gcc";
        case ToolchainFamily::Clang:   return "clang";
        case ToolchainFamily::Msvc:    return "msvc";
        case ToolchainFamily::ClangCl: return "clangcl";
    }
    return "gcc";
}

ToolchainFamily ToolchainFamilyFromString(const std::string& str) {
    std::string low = ToLowerTc(str);
    if (low == "clang") return ToolchainFamily::Clang;
    if (low == "msvc" || low == "cl") return ToolchainFamily::Msvc;
    if (low == "clangcl" || low == "clang-cl") return ToolchainFamily::ClangCl;
    return ToolchainFamily::Gcc;
}

void ToolchainProfile::ComputeIdHash() {
    std::ostringstream ss;
    ss << name << "|" << ToolchainFamilyToString(family) << "|"
       << cxx << "|" << cc << "|" << ar << "|" << ld << "|"
       << targetTriple << "|" << sysroot << "|" << linkWith << "|" << version;
    idHash8 = Hash8Tc(ss.str());
}

std::filesystem::path ToolchainManager::GetUserToolchainsPath() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? std::filesystem::u8path(appdata) : std::filesystem::current_path();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = xdg ? std::filesystem::u8path(xdg)
                                     : (home ? (std::filesystem::u8path(home) / ".config") : std::filesystem::current_path());
#endif
    return base / "GenieBuild" / "toolchains.json";
}

ToolchainProfile ToolchainManager::ProfileFromJson(const nlohmann::json& j, bool is_project_local) {
    ToolchainProfile p;
    p.isProjectLocal = is_project_local;
    if (!j.is_object()) return p;
    p.name = j.value("name", "");
    p.family = ToolchainFamilyFromString(j.value("family", "gcc"));
    p.cc = j.value("cc", "");
    p.cxx = j.value("cxx", "");
    p.ar = j.value("ar", "");
    p.ld = j.value("ld", "");
    p.objcopy = j.value("objcopy", "");
    p.targetTriple = j.value("targetTriple", "");
    p.sysroot = j.value("sysroot", "");
    p.os = j.value("os", "");
    p.arch = j.value("arch", "");
    p.linkWith = j.value("linkWith", "driver");
    if (p.linkWith != "ld") p.linkWith = "driver";
    p.launcher = j.value("launcher", "auto");
    p.version = j.value("version", "");

    if (j.contains("env") && j["env"].is_object()) {
        for (auto it = j["env"].begin(); it != j["env"].end(); ++it) {
            if (it.value().is_string()) {
                p.env[it.key()] = it.value().get<std::string>();
            }
        }
    }
    if (j.contains("extraCompileFlags") && j["extraCompileFlags"].is_array()) {
        for (const auto& x : j["extraCompileFlags"]) {
            if (x.is_string()) p.extraCompileFlags.push_back(x.get<std::string>());
        }
    }
    if (j.contains("extraLinkFlags") && j["extraLinkFlags"].is_array()) {
        for (const auto& x : j["extraLinkFlags"]) {
            if (x.is_string()) p.extraLinkFlags.push_back(x.get<std::string>());
        }
    }
    if (j.contains("customTools") && j["customTools"].is_array()) {
        for (const auto& item : j["customTools"]) {
            if (!item.is_object()) continue;
            CustomToolSpec ct;
            ct.ext = item.value("ext", "");
            ct.program = item.value("program", "");
            ct.args = item.value("args", "");
            ct.output = item.value("output", "");
            ct.link = item.value("link", false);
            if (!ct.ext.empty() && !ct.program.empty()) {
                p.customTools.push_back(std::move(ct));
            }
        }
    }
    p.ComputeIdHash();
    return p;
}

nlohmann::json ToolchainManager::ProfileToJson(const ToolchainProfile& p) {
    nlohmann::json j = nlohmann::json::object();
    j["name"] = p.name;
    j["family"] = ToolchainFamilyToString(p.family);
    if (!p.cc.empty()) j["cc"] = p.cc;
    if (!p.cxx.empty()) j["cxx"] = p.cxx;
    if (!p.ar.empty()) j["ar"] = p.ar;
    if (!p.ld.empty()) j["ld"] = p.ld;
    if (!p.objcopy.empty()) j["objcopy"] = p.objcopy;
    if (!p.targetTriple.empty()) j["targetTriple"] = p.targetTriple;
    if (!p.sysroot.empty()) j["sysroot"] = p.sysroot;
    if (!p.os.empty()) j["os"] = p.os;
    if (!p.arch.empty()) j["arch"] = p.arch;
    if (p.linkWith != "driver") j["linkWith"] = p.linkWith;
    if (!p.env.empty()) j["env"] = p.env;
    if (!p.extraCompileFlags.empty()) j["extraCompileFlags"] = p.extraCompileFlags;
    if (!p.extraLinkFlags.empty()) j["extraLinkFlags"] = p.extraLinkFlags;
    if (!p.launcher.empty() && p.launcher != "auto") j["launcher"] = p.launcher;
    if (!p.customTools.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ct : p.customTools) {
            nlohmann::json c = nlohmann::json::object();
            c["ext"] = ct.ext;
            c["program"] = ct.program;
            c["args"] = ct.args;
            if (!ct.output.empty()) c["output"] = ct.output;
            if (ct.link) c["link"] = true;
            arr.push_back(std::move(c));
        }
        j["customTools"] = std::move(arr);
    }
    return j;
}

std::vector<ToolchainProfile> ToolchainManager::LoadUserProfiles() {
    std::vector<ToolchainProfile> out;
    std::ifstream in(GetUserToolchainsPath());
    if (!in.is_open()) return out;
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_array()) {
            for (const auto& item : j) {
                ToolchainProfile p = ProfileFromJson(item, false);
                if (!p.name.empty()) out.push_back(std::move(p));
            }
        }
    } catch (...) {}
    return out;
}

void ToolchainManager::SaveUserProfiles(const std::vector<ToolchainProfile>& profiles) {
    std::filesystem::path path = GetUserToolchainsPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : profiles) {
        if (!p.isProjectLocal && !p.detected) {
            arr.push_back(ProfileToJson(p));
        }
    }

    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return;
        out << arr.dump(2) << std::endl;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

std::vector<ToolchainProfile> ToolchainManager::DetectAllToolchains(
    bool force_vs_refresh,
    MsvcEnvCacheState* out_msvc_state
) {
    std::vector<ToolchainProfile> profiles;
    std::unordered_set<std::string> dedup_keys;
    std::unordered_set<std::string> used_names;
    std::unordered_set<std::string> seen_dirs;
    std::vector<std::filesystem::path> scan_dirs;
    std::error_code ec;

    auto add_dir = [&](const std::filesystem::path& d) {
        if (d.empty()) return;
        if (!std::filesystem::exists(d, ec) || !std::filesystem::is_directory(d, ec)) return;
        std::string key = ToLowerTc(d.lexically_normal().generic_u8string());
        if (seen_dirs.insert(key).second) {
            scan_dirs.push_back(d);
        }
    };

    if (const char* path_env = std::getenv("PATH")) {
#if defined(_WIN32)
        const char delim = ';';
#else
        const char delim = ':';
#endif
        std::stringstream ss(path_env);
        std::string tok;
        while (std::getline(ss, tok, delim)) {
            if (!tok.empty()) add_dir(std::filesystem::u8path(tok));
        }
    }

#if defined(_WIN32)
    add_dir("C:/msys64/ucrt64/bin");
    add_dir("C:/msys64/mingw64/bin");
    add_dir("C:/msys64/clang64/bin");
    add_dir("C:/Program Files/LLVM/bin");
    if (const char* local_app = std::getenv("LOCALAPPDATA")) {
        std::filesystem::path progs = std::filesystem::u8path(local_app) / "Programs";
        add_dir(progs / "LLVM" / "bin");
    }
#else
    add_dir("/usr/bin");
    add_dir("/usr/local/bin");
#endif

    auto try_register_driver = [&](const std::filesystem::path& dir, const std::string& driver_exe, const std::string& prefix, ToolchainFamily fam) {
        std::string driver_path = FindSiblingExe(dir, driver_exe);
        if (driver_path.empty()) return;

        std::string ver_banner = FirstNonEmptyLine(QueryExeOutput(driver_path, { "--version" }));
        std::string triple = FirstNonEmptyLine(QueryExeOutput(driver_path, { "-dumpmachine" }));
        std::string short_ver = ShortenVersionDigits(ver_banner);

        ToolchainProfile p;
        p.detected = true;
        p.family = fam;
        p.version = ver_banner;
        p.os = HostOsName();
        p.arch = HostArchName();
        InferOsArchFromTriple(triple, p.os, p.arch);

        bool is_cxx_driver = (driver_exe.find("++") != std::string::npos || driver_exe == "clang++");
        if (is_cxx_driver) {
            p.cxx = driver_path;
            std::string cc_name = (fam == ToolchainFamily::Clang) ? (prefix + "clang") : (prefix + "gcc");
            p.cc = FindSiblingExe(dir, cc_name);
            if (p.cc.empty()) p.cc = driver_path;
        } else {
            p.cc = driver_path;
            std::string cxx_name = (fam == ToolchainFamily::Clang) ? (prefix + "clang++") : (prefix + "g++");
            p.cxx = FindSiblingExe(dir, cxx_name);
            if (p.cxx.empty()) p.cxx = driver_path;
        }

        // Deduplicate paired cc/cxx drivers (e.g. g++ + gcc or x86_64-w64-mingw32-g++ + -gcc)
        std::string dedup = ToolchainFamilyToString(fam) + "|" + ToLowerTc(p.cxx) + "|" + ToLowerTc(p.cc) + "|" + triple;
        if (!dedup_keys.insert(dedup).second) return;

        if (fam == ToolchainFamily::Clang) {
            p.ar = FindSiblingExe(dir, prefix + "llvm-ar");
            if (p.ar.empty()) p.ar = FindSiblingExe(dir, prefix + "ar");
            p.ld = FindSiblingExe(dir, prefix + "ld.lld");
            if (p.ld.empty()) p.ld = FindSiblingExe(dir, prefix + "ld");
            p.objcopy = FindSiblingExe(dir, prefix + "llvm-objcopy");
            if (p.objcopy.empty()) p.objcopy = FindSiblingExe(dir, prefix + "objcopy");
        } else {
            p.ar = FindSiblingExe(dir, prefix + "gcc-ar");
            if (p.ar.empty()) p.ar = FindSiblingExe(dir, prefix + "ar");
            p.ld = FindSiblingExe(dir, prefix + "ld");
            p.objcopy = FindSiblingExe(dir, prefix + "objcopy");
        }

        std::string env_tag;
        {
            std::string low_dir = ToLowerTc(dir.generic_u8string());
            if (low_dir.find("ucrt64") != std::string::npos) env_tag = "MSYS2 UCRT64";
            else if (low_dir.find("mingw64") != std::string::npos) env_tag = "MSYS2 MinGW64";
            else if (low_dir.find("clang64") != std::string::npos) env_tag = "MSYS2 CLANG64";
            else if (low_dir.find("llvm") != std::string::npos) env_tag = "LLVM";
        }

        if (!prefix.empty()) {
            std::string clean_pref = prefix;
            if (clean_pref.back() == '-') clean_pref.pop_back();
            std::string fam_lbl = (fam == ToolchainFamily::Clang ? "Clang" : "GCC");
            std::string details = fam_lbl + (short_ver.empty() ? "" : (" " + short_ver));
            if (!env_tag.empty()) details += ", " + env_tag;
            p.name = clean_pref + " cross (" + details + ")";
            if (p.os == "none") {
                p.linkWith = "driver";
            }
        } else {
            std::string fam_lbl = (fam == ToolchainFamily::Clang) ? "Clang" : "GCC";
            p.name = fam_lbl + (short_ver.empty() ? "" : (" " + short_ver));
            if (!env_tag.empty()) p.name += " (" + env_tag + ")";
        }

        if (!used_names.insert(p.name).second) {
            std::string base_name = p.name;
            int idx = 2;
            while (!used_names.insert(base_name + " (" + std::to_string(idx) + ")").second) {
                ++idx;
            }
            p.name = base_name + " (" + std::to_string(idx) + ")";
        }

        p.ComputeIdHash();
        profiles.push_back(std::move(p));
    };

    const char* kCrossPrefixes[] = {
        "x86_64-elf-", "i686-elf-", "aarch64-none-elf-", "arm-none-eabi-", "x86_64-w64-mingw32-"
    };

    for (const auto& dir : scan_dirs) {
        try_register_driver(dir, "g++", "", ToolchainFamily::Gcc);
        try_register_driver(dir, "clang++", "", ToolchainFamily::Clang);
        for (const char* pref : kCrossPrefixes) {
            try_register_driver(dir, std::string(pref) + "g++", pref, ToolchainFamily::Gcc);
            try_register_driver(dir, std::string(pref) + "gcc", pref, ToolchainFamily::Gcc);
        }
        // Fallback if only C gcc/clang is installed without g++/clang++
        try_register_driver(dir, "gcc", "", ToolchainFamily::Gcc);
        try_register_driver(dir, "clang", "", ToolchainFamily::Clang);
    }

    // Also scan for arbitrary *-gcc / *-g++ in scan dirs beyond the explicit prefix list
    for (const auto& dir : scan_dirs) {
        std::filesystem::directory_iterator dit(dir, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; dit != std::filesystem::directory_iterator() && !ec; dit.increment(ec)) {
            if (!dit->is_regular_file(ec)) continue;
            std::string fname = dit->path().filename().generic_u8string();
            std::string low = ToLowerTc(fname);
#if defined(_WIN32)
            if (low.size() > 4 && low.rfind(".exe") == low.size() - 4) {
                fname.resize(fname.size() - 4);
                low.resize(low.size() - 4);
            }
#endif
            if (low.size() > 4 && low.rfind("-g++") == low.size() - 4) {
                std::string pref = fname.substr(0, fname.size() - 3);
                try_register_driver(dir, fname, pref, ToolchainFamily::Gcc);
            } else if (low.size() > 4 && low.rfind("-gcc") == low.size() - 4) {
                std::string pref = fname.substr(0, fname.size() - 3);
                try_register_driver(dir, fname, pref, ToolchainFamily::Gcc);
            }
        }
    }

    // V2-17: Detect MSVC installs + captured vcvarsall environments
    MsvcEnvCacheState msvc_state = MsvcEnvDetector::DetectAndCaptureAll(force_vs_refresh);
    if (out_msvc_state) *out_msvc_state = msvc_state;

    for (const auto& m : msvc_state.installs) {
        std::string dedup = "msvc|" + m.installationVersion + "|" + m.arch;
        if (dedup_keys.insert(dedup).second) {
            ToolchainProfile p;
            p.detected = true;
            p.name = m.profileName;
            p.family = ToolchainFamily::Msvc;
            p.cc = m.clPath;
            p.cxx = m.clPath;
            p.ar = m.libPath;
            p.ld = m.linkPath;
            p.os = "windows";
            p.arch = m.arch;
            p.linkWith = "driver";
            p.env = m.env;
            p.version = m.installationVersion;
            p.ComputeIdHash();
            profiles.push_back(std::move(p));
        }

        // Also offer clang-cl paired with this MSVC SDK/CRT if LLVM clang-cl is installed
        std::string clang_cl = m.clangClPath;
        if (clang_cl.empty()) {
            clang_cl = FindSiblingExe("C:/Program Files/LLVM/bin", "clang-cl");
        }
        if (!clang_cl.empty()) {
            std::string cc_dedup = "clangcl|" + clang_cl + "|" + m.arch;
            if (dedup_keys.insert(cc_dedup).second) {
                std::string cver = FirstNonEmptyLine(QueryExeOutput(clang_cl, { "--version" }));
                std::string sver = ShortenVersionDigits(cver);
                ToolchainProfile ccp;
                ccp.detected = true;
                ccp.name = "Clang-CL " + (sver.empty() ? "" : (sver + " ")) + m.arch +
                           (m.shortVersion.empty() ? "" : (" (VS " + m.shortVersion + ")"));
                ccp.family = ToolchainFamily::ClangCl;
                ccp.cc = clang_cl;
                ccp.cxx = clang_cl;
                ccp.ar = m.libPath;
                ccp.ld = m.linkPath;
                ccp.os = "windows";
                ccp.arch = m.arch;
                ccp.linkWith = "driver";
                ccp.env = m.env;
                ccp.version = cver;
                ccp.ComputeIdHash();
                profiles.push_back(std::move(ccp));
            }
        }
    }

    return profiles;
}

ToolchainProfile ToolchainManager::ResolveProfileVariables(
    const ToolchainProfile& prof,
    const std::filesystem::path& project_root
) {
    ToolchainProfile p = prof;
    std::string root_fwd = project_root.generic_u8string();
    p.cc = ReplaceRootVar(p.cc, root_fwd);
    p.cxx = ReplaceRootVar(p.cxx, root_fwd);
    p.ar = ReplaceRootVar(p.ar, root_fwd);
    p.ld = ReplaceRootVar(p.ld, root_fwd);
    p.objcopy = ReplaceRootVar(p.objcopy, root_fwd);
    p.sysroot = ReplaceRootVar(p.sysroot, root_fwd);
    for (auto& [k, v] : p.env) v = ReplaceRootVar(v, root_fwd);
    for (auto& f : p.extraCompileFlags) f = ReplaceRootVar(f, root_fwd);
    for (auto& f : p.extraLinkFlags) f = ReplaceRootVar(f, root_fwd);
    p.ComputeIdHash();
    return p;
}

std::vector<ToolchainProfile> ToolchainManager::MergeProfiles(
    const std::vector<ToolchainProfile>& detected,
    const std::vector<ToolchainProfile>& user_profiles,
    const std::vector<ToolchainProfile>& project_profiles,
    const std::filesystem::path& project_root
) {
    std::vector<ToolchainProfile> merged = detected;

    auto upsert = [&](const ToolchainProfile& prof) {
        ToolchainProfile resolved = ResolveProfileVariables(prof, project_root);
        for (auto& existing : merged) {
            if (existing.name == resolved.name) {
                existing = resolved;
                return;
            }
        }
        merged.push_back(std::move(resolved));
    };

    for (const auto& u : user_profiles) upsert(u);
    for (const auto& pl : project_profiles) upsert(pl);
    return merged;
}

ToolchainResolution ToolchainManager::ResolveForProject(
    const Project& project,
    const std::vector<ToolchainProfile>& available_profiles
) {
    ToolchainResolution res;
    std::string requested = project.GetToolchain();
    if (requested.empty()) {
        requested = project.GetCompiler();
    }
    res.requestedName = requested;

    if (available_profiles.empty()) {
        return res;
    }

    // 1. Exact profile name match
    for (const auto& p : available_profiles) {
        if (p.name == requested) {
            res.profile = p;
            res.found = true;
            res.isFallback = false;
            return res;
        }
    }

    // 2. Direct legacy executable/name match ("g++", "clang++", "cl")
    std::string req_low = ToLowerTc(requested);
    ToolchainFamily wanted_fam = ToolchainFamily::Gcc;
    if (req_low.find("clang-cl") != std::string::npos || req_low == "clangcl") wanted_fam = ToolchainFamily::ClangCl;
    else if (req_low.find("clang") != std::string::npos) wanted_fam = ToolchainFamily::Clang;
    else if (req_low.find("msvc") != std::string::npos || req_low == "cl" || req_low == "cl.exe") wanted_fam = ToolchainFamily::Msvc;

    for (const auto& p : available_profiles) {
        if (p.family == wanted_fam && (req_low == "g++" || req_low == "gcc" || req_low == "clang++" ||
                                       req_low == "clang" || req_low == "cl" || req_low == "clang-cl")) {
            res.profile = p;
            res.found = true;
            res.isFallback = false;
            return res;
        }
    }

    // 3. Fallback rule: same family (+ arch if known) -> any detected toolchain
    std::string wanted_arch = HostArchName();
    if (req_low.find("arm64") != std::string::npos) wanted_arch = "arm64";
    else if (req_low.find("x86") != std::string::npos && req_low.find("x86_64") == std::string::npos) wanted_arch = "x86";

    const ToolchainProfile* fam_arch_match = nullptr;
    const ToolchainProfile* fam_match = nullptr;
    for (const auto& p : available_profiles) {
        if (p.family == wanted_fam) {
            if (!fam_match) fam_match = &p;
            if (p.arch == wanted_arch && !fam_arch_match) fam_arch_match = &p;
        }
    }

    const ToolchainProfile* chosen = fam_arch_match ? fam_arch_match : (fam_match ? fam_match : &available_profiles.front());
    res.profile = *chosen;
    res.found = true;
    if (!requested.empty()) {
        res.isFallback = true;
        res.bannerMessage = "Toolchain '" + requested + "' not found; using fallback '" + res.profile.name + "'.";
    }
    return res;
}

ToolchainTestResult ToolchainManager::TestProfile(const ToolchainProfile& profile) {
    ToolchainTestResult res;
    auto t0 = std::chrono::high_resolution_clock::now();

    std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("geniebuild_tc_test_" + (profile.idHash8.empty() ? "default" : profile.idHash8));
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);

    std::filesystem::path src = tmp / "hello.cpp";
    std::filesystem::path obj = tmp / (profile.IsMsvcSyntax() ? "hello.obj" : "hello.o");
    std::filesystem::path exe = tmp / "hello.exe";

    bool is_freestanding_none = (ToLowerTc(profile.os) == "none");
    {
        std::ofstream f(src);
        if (is_freestanding_none) {
            f << "extern \"C\" int tc_ok() { return 42; }\n";
        } else {
            f << "#include <iostream>\nint main() { std::cout << \"Hello from GenieBuild toolchain!\\n\"; return 0; }\n";
        }
    }

    std::unordered_map<std::string, std::string> extra_env = MsvcEnvDetector::BuildMergedProcessEnv(profile.env);
    std::string compiler_exe = !profile.cxx.empty() ? profile.cxx : profile.cc;
    if (compiler_exe.empty()) {
        res.output = "No C++ compiler path configured in profile.";
        return res;
    }

    std::ostringstream log;
    std::vector<std::string> args;
    if (profile.IsMsvcSyntax()) {
        args = { "/nologo", "/EHsc" };
        if (is_freestanding_none) args.push_back("/c");
        args.push_back(src.generic_u8string());
        if (is_freestanding_none) {
            args.push_back("/Fo" + obj.generic_u8string());
        } else {
            args.push_back("/Fe" + exe.generic_u8string());
            args.push_back("/Fo" + obj.generic_u8string());
        }
    } else {
        if (!profile.targetTriple.empty()) args.push_back("--target=" + profile.targetTriple);
        if (!profile.sysroot.empty()) args.push_back("--sysroot=" + profile.sysroot);
        if (is_freestanding_none) {
            args.push_back("-ffreestanding");
            args.push_back("-c");
            args.push_back(src.generic_u8string());
            args.push_back("-o");
            args.push_back(obj.generic_u8string());
        } else {
            args.push_back(src.generic_u8string());
            args.push_back("-o");
            args.push_back(exe.generic_u8string());
        }
    }

    reproc::process proc;
    reproc::options opts;
    opts.redirect.out.type = reproc::redirect::pipe;
    opts.redirect.err.type = reproc::redirect::stdout_;
    opts.deadline = reproc::milliseconds(15000);
    if (!extra_env.empty()) {
        opts.env.behavior = reproc::env::extend;
        opts.env.extra = extra_env;
    }
    std::vector<std::string> argv = { compiler_exe };
    argv.insert(argv.end(), args.begin(), args.end());

    log << "> " << compiler_exe;
    for (const auto& a : args) log << " " << a;
    log << "\n";

    std::error_code sec = proc.start(argv, opts);
    if (sec) {
        log << "Failed to start compiler: " << sec.message() << "\n";
        res.output = log.str();
        std::filesystem::remove_all(tmp, ec);
        return res;
    }
    uint8_t buf[2048];
    while (true) {
        auto [n, rec] = proc.read(reproc::stream::out, buf, sizeof(buf));
        if (rec || n == 0) break;
        log.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(n));
    }
    auto [exit_code, wec] = proc.wait(reproc::milliseconds(5000));
    if (wec || exit_code != 0) {
        log << "\nCompile/link failed (exit " << exit_code << ").\n";
        res.output = log.str();
        std::filesystem::remove_all(tmp, ec);
        return res;
    }

    if (!is_freestanding_none && std::filesystem::exists(exe, ec)) {
        std::string run_out = QueryExeOutput(exe.generic_u8string(), {}, &extra_env, 5000);
        log << run_out;
    } else if (is_freestanding_none) {
        log << "Freestanding cross-compile OK (" << obj.filename().generic_u8string() << ").\n";
    }

    res.success = true;
    res.durationSeconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
    res.output = log.str();
    std::filesystem::remove_all(tmp, ec);
    return res;
}

std::string ToolchainManager::QueryLibgccPath(const ToolchainProfile& profile) {
    std::string cc = !profile.cc.empty() ? profile.cc : profile.cxx;
    if (cc.empty() || profile.IsMsvcSyntax()) return "";
    auto env = MsvcEnvDetector::BuildMergedProcessEnv(profile.env);
    std::vector<std::string> args = { "-print-libgcc-file-name" };
    if (!profile.targetTriple.empty()) args.push_back("--target=" + profile.targetTriple);
    return FirstNonEmptyLine(QueryExeOutput(cc, args, env.empty() ? nullptr : &env, 2500));
}

} // namespace core
