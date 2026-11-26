#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

struct MsvcInstallEnv {
    std::string displayName;        // e.g. "Visual Studio Community 2022"
    std::string installationPath;
    std::string installationVersion;// e.g. "17.12.35527.113"
    std::string shortVersion;       // e.g. "17.12"
    std::string arch;               // "x64", "x86", "arm64"
    std::string profileName;        // e.g. "MSVC 2022 x64 (17.12)"
    std::string vcvarsPath;
    int64_t vcvarsMtimeNs = 0;
    std::string clPath;
    std::string linkPath;
    std::string libPath;
    std::string rcPath;
    std::string ml64Path;
    std::string clangClPath;
    std::map<std::string, std::string> env; // PATH, INCLUDE, LIB, LIBPATH, VCTools*, WindowsSdk*
};

struct MsvcEnvCacheState {
    bool bannerDismissed = false;
    std::string lastBannerText;
    std::vector<MsvcInstallEnv> installs;
};

// V2-17 Automatic MSVC environment setup (vswhere + vcvarsall capture cached in %APPDATA%/GenieBuild/msvc_env.json)
class MsvcEnvDetector {
public:
    static std::filesystem::path GetCacheFilePath();
    static MsvcEnvCacheState LoadCache();
    static void SaveCache(const MsvcEnvCacheState& state);

    // Finds vswhere.exe in standard Program Files (x86) locations
    static std::filesystem::path FindVsWhere();

    // Detects all VS/BuildTools installs x {x64, x86, arm64}, reusing valid cached captures (<100ms)
    // unless force_refresh is true.
    static MsvcEnvCacheState DetectAndCaptureAll(bool force_refresh = false);

    // Returns user banner string if VS installs exist and banner wasn't dismissed
    static std::string GetReadyBannerText(const MsvcEnvCacheState& state);
    static void DismissBanner();

    // Merges MSVC captured env vars + optional extra env overrides (including PATH+ prepending)
    // into an env map suitable for reproc::env::extend.
    static std::unordered_map<std::string, std::string> BuildMergedProcessEnv(
        const std::map<std::string, std::string>& toolchain_env
    );
};

} // namespace core
