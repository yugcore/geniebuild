#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "build_runner.h"
#include "build_steps.h"
#include "compiler.h"
#include "condition.h"
#include "msvc_env.h"
#include "project.h"
#include "run_runner.h"
#include "settings_resolver.h"
#include "target.h"
#include "toolchain.h"

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED at line " << __LINE__ << ": " << #cond << "\n"; \
        std::abort(); \
    } \
} while (0)

static fs::path MakeTempDir(const std::string& suffix) {
    fs::path p = fs::temp_directory_path() / ("geniebuild_test_" + suffix);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static void WriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    CHECK(f.is_open());
    f << text;
}

static std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    CHECK(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static core::EffectiveTargetSettings ResolveOneTarget(const core::Project& proj, const core::Target& t) {
    std::unordered_map<std::string, const core::Target*> tmap;
    tmap[t.name] = &t;
    return core::ResolveEffectiveTargetSettings(proj, t, tmap, proj.GetConfiguration(), false);
}

static bool ContainsArg(const std::vector<std::string>& args, const std::string& needle) {
    for (const auto& a : args) {
        if (a == needle) return true;
    }
    return false;
}

static void TestMsvcEnvCacheAndEnvMerge() {
    std::cout << "[TEST] TestMsvcEnvCacheAndEnvMerge...\n";
    core::MsvcInstallEnv env_inst;
    env_inst.installationPath = "C:/Program Files/Microsoft Visual Studio/2022/Community";
    env_inst.installationVersion = "17.10.0.1";
    env_inst.shortVersion = "17.10";
    env_inst.displayName = "Visual Studio Community 2022";
    env_inst.vcvarsMtimeNs = 123456789LL;
    env_inst.arch = "x64";
    env_inst.profileName = "MSVC 2022 x64 (17.10)";
    env_inst.clPath = "C:/VS/VC/Tools/MSVC/14.40/bin/Hostx64/x64/cl.exe";
    env_inst.env["INCLUDE"] = "C:/VS/include";
    env_inst.env["LIB"] = "C:/VS/lib";
    env_inst.env["PATH+"] = "C:/VS/VC/Tools/MSVC/14.40/bin/Hostx64/x64";

    core::MsvcEnvCacheState st;
    st.installs.push_back(env_inst);
    st.bannerDismissed = false;

    std::string banner = core::MsvcEnvDetector::GetReadyBannerText(st);
    CHECK(banner.find("Visual Studio") != std::string::npos);

    std::map<std::string, std::string> combined = env_inst.env;
    combined["MY_VAR"] = "hello_msvc";
    combined["PATH+"] = combined["PATH+"] + ";C:/ExtraTools/bin";
    std::unordered_map<std::string, std::string> merged = core::MsvcEnvDetector::BuildMergedProcessEnv(combined);

    CHECK(merged["INCLUDE"] == "C:/VS/include");
    CHECK(merged["LIB"] == "C:/VS/lib");
    CHECK(merged["MY_VAR"] == "hello_msvc");
    bool found_path_prepend = false;
    for (const auto& kv : merged) {
        std::string klow = kv.first;
        for (char& c : klow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (klow == "path") {
            if (kv.second.find("C:/ExtraTools/bin") != std::string::npos &&
                kv.second.find("C:/VS/VC/Tools/MSVC/14.40/bin/Hostx64/x64") != std::string::npos) {
                found_path_prepend = true;
            }
        }
    }
    CHECK(found_path_prepend);

    // Ensure detection routine never crashes regardless of host VS install state
    core::MsvcEnvCacheState detected = core::MsvcEnvDetector::DetectAndCaptureAll(false);
    (void)detected;
}

static void TestToolchainSerializationAndResolution() {
    std::cout << "[TEST] TestToolchainSerializationAndResolution...\n";
    fs::path tmp = MakeTempDir("v2c_tc");

    core::Project proj;
    proj.Load(tmp);

    core::ToolchainProfile elf_tc;
    elf_tc.name = "x86_64-elf-gcc";
    elf_tc.family = core::ToolchainFamily::Gcc;
    elf_tc.cc = "${root}/tools/bin/x86_64-elf-gcc";
    elf_tc.cxx = "${root}/tools/bin/x86_64-elf-g++";
    elf_tc.ar = "x86_64-elf-ar";
    elf_tc.ld = "x86_64-elf-ld";
    elf_tc.objcopy = "x86_64-elf-objcopy";
    elf_tc.targetTriple = "x86_64-elf";
    elf_tc.sysroot = "${root}/sysroot";
    elf_tc.os = "freestanding";
    elf_tc.arch = "x64";
    elf_tc.linkWith = "ld";
    elf_tc.env = { { "PATH+", "${root}/tools/bin" }, { "CUSTOM_TC", "1" } };
    elf_tc.extraCompileFlags = { "-mgeneral-regs-only" };
    elf_tc.extraLinkFlags = { "-z", "max-page-size=0x1000" };
    elf_tc.isProjectLocal = true;

    proj.SetProjectToolchains({ elf_tc });
    proj.SetToolchain("x86_64-elf-gcc");
    CHECK(proj.SaveImmediate());

    // Reload from disk and verify roundtrip
    core::Project reloaded;
    CHECK(reloaded.Load(tmp));
    CHECK(reloaded.GetToolchain() == "x86_64-elf-gcc");
    CHECK(reloaded.GetProjectToolchains().size() == 1);
    CHECK(reloaded.GetProjectToolchains()[0].sysroot == "${root}/sysroot");
    CHECK(reloaded.GetProjectToolchains()[0].linkWith == "ld");

    // Variable substitution
    core::ToolchainProfile resolved = core::ToolchainManager::ResolveProfileVariables(reloaded.GetProjectToolchains()[0], tmp);
    std::string root_str = tmp.generic_u8string();
    CHECK(resolved.sysroot == (root_str + "/sysroot"));
    CHECK(resolved.cc == (root_str + "/tools/bin/x86_64-elf-gcc"));
    CHECK(resolved.env["PATH+"] == (root_str + "/tools/bin"));

    // Condition context with active toolchain profile
    core::ConditionContext ctx = core::BuildConditionContext(reloaded, &resolved);
    CHECK(ctx.os == "freestanding");
    CHECK(ctx.arch == "x64");
    CHECK(ctx.compilerFamily == "gcc");

    // Fallback when requested toolchain is missing
    core::CompilerManager cm;
    reloaded.SetToolchain("nonexistent-cross-gcc");
    core::ToolchainResolution fb = cm.ResolveProjectToolchain(reloaded);
    if (!cm.GetDetectedCompilers().empty()) {
        CHECK(fb.isFallback);
        CHECK(!fb.bannerMessage.empty());
        CHECK(fb.requestedName == "nonexistent-cross-gcc");
    }

    // Per-toolchain obj dir isolation
    core::Target t;
    t.name = "app";
    fs::path obj_a = t.GetObjectDir(tmp, "Debug", "aaaa1111");
    fs::path obj_b = t.GetObjectDir(tmp, "Debug", "bbbb2222");
    CHECK(obj_a != obj_b);
    CHECK(obj_a.parent_path().filename() == "Debug-aaaa1111");
    CHECK(obj_b.parent_path().filename() == "Debug-bbbb2222");

    // Command hash salt sensitivity
    core::ToolchainProfile tc_salt_1 = resolved;
    tc_salt_1.version = "14.1.0";
    tc_salt_1.ComputeIdHash();
    core::ToolchainProfile tc_salt_2 = tc_salt_1;
    tc_salt_2.sysroot = root_str + "/other_sysroot";
    tc_salt_2.ComputeIdHash();

    core::CompilerInfo ci1 = cm.MakeCompilerInfoFromProfile(tc_salt_1);
    core::CompilerInfo ci2 = cm.MakeCompilerInfoFromProfile(tc_salt_2);
    core::EffectiveTargetSettings eff = ResolveOneTarget(reloaded, t);
    core::BuildCommand c1 = core::CompilerManager::FormulateCompileCommand(ci1, "src/main.cpp", tmp / "o1.o", reloaded, eff);
    core::BuildCommand c2 = core::CompilerManager::FormulateCompileCommand(ci2, "src/main.cpp", tmp / "o2.o", reloaded, eff);
    CHECK(core::CompilerManager::ComputeCommandHash(c1) != core::CompilerManager::ComputeCommandHash(c2));

    // TestProfile on detected host compiler
    auto detected_tcs = cm.GetDetectedToolchains();
    if (!detected_tcs.empty()) {
        auto tr = core::ToolchainManager::TestProfile(detected_tcs[0]);
        CHECK(tr.success);
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

static void TestFreestandingKernelAndLinkerScript() {
    std::cout << "[TEST] TestFreestandingKernelAndLinkerScript...\n";
    fs::path tmp = MakeTempDir("v2c_kernel");
    core::Target created_t;
    std::vector<std::string> created_files;
    core::CreateKernelStarterTemplate(tmp, "mykernel", core::KernelBootProtocol::Multiboot2, created_t, &created_files);
    CHECK(fs::exists(tmp / "kernel/kernel.c"));
    CHECK(fs::exists(tmp / "boot/boot.S"));
    CHECK(fs::exists(tmp / "kernel/linker.ld"));
    CHECK(fs::exists(tmp / "iso/boot/grub/grub.cfg"));

    // Non-overwrite check on existing files
    WriteText(tmp / "kernel/kernel.c", "// sentinel custom kernel\nvoid kernel_main() {}\n");
    core::Target created_t2;
    core::CreateKernelStarterTemplate(tmp, "mykernel2", core::KernelBootProtocol::Multiboot2, created_t2);
    CHECK(ReadText(tmp / "kernel/kernel.c").find("sentinel custom kernel") != std::string::npos);

    core::Project proj;
    CHECK(proj.Load(tmp));
    std::string add_err;
    if (proj.FindTarget("mykernel")) {
        proj.UpdateTarget(created_t);
    } else {
        CHECK(proj.AddTarget(created_t, add_err));
    }
    proj.SaveImmediate();

    core::Project reloaded;
    CHECK(reloaded.Load(tmp));
    const core::Target* kt_ptr = reloaded.FindTarget("mykernel");
    CHECK(kt_ptr != nullptr);
    const core::Target& kt = *kt_ptr;
    CHECK(kt.freestanding);
    CHECK(kt.linkerScript == "kernel/linker.ld");
    CHECK(kt.entry == "_start");

    // Freestanding targets must NEVER auto-append .exe even on Windows
    fs::path kbin = kt.GetBinaryPath(reloaded.GetAbsoluteOutputDir(), reloaded.GetConfiguration(), false);
    CHECK(kbin.extension() != ".exe");
    CHECK(kbin.filename() == "kernel.elf");

    core::ToolchainProfile cross;
    cross.name = "x86_64-elf-clang";
    cross.family = core::ToolchainFamily::Clang;
    cross.cc = "clang";
    cross.cxx = "clang++";
    cross.ld = "ld.lld";
    cross.objcopy = "llvm-objcopy";
    cross.targetTriple = "x86_64-elf";
    cross.os = "freestanding";
    cross.arch = "x64";
    cross.linkWith = "ld";
    cross.ComputeIdHash();

    core::CompilerManager cm;
    core::CompilerInfo ci_cpp = cm.MakeCompilerInfoFromProfile(cross, false);
    core::CompilerInfo ci_c = cm.MakeCompilerInfoFromProfile(cross, true);
    core::EffectiveTargetSettings eff = ResolveOneTarget(reloaded, kt);

    // Verify cxxFlags applied ONLY to C++ and NOT C
    core::BuildCommand cpp_cmd = core::CompilerManager::FormulateCompileCommand(ci_cpp, "src/kernel.cpp", tmp / "kernel.o", reloaded, eff);
    core::BuildCommand c_cmd = core::CompilerManager::FormulateCompileCommand(ci_c, "src/hal.c", tmp / "hal.o", reloaded, eff);
    CHECK(ContainsArg(cpp_cmd.arguments, "--target=x86_64-elf"));
    CHECK(ContainsArg(cpp_cmd.arguments, "-fno-exceptions"));
    CHECK(!ContainsArg(c_cmd.arguments, "-fno-exceptions"));

    // Verify direct ld invocation with -T, -e, -nostdlib
    core::BuildCommand link_cmd = core::CompilerManager::FormulateLinkCommand(
        ci_cpp, kt, eff, { tmp / "boot.o", tmp / "kernel.o" }, kbin, reloaded, false
    );
    CHECK(link_cmd.program == "ld.lld");
    CHECK(ContainsArg(link_cmd.arguments, "-T"));
    CHECK(ContainsArg(link_cmd.arguments, "-e"));
    CHECK(ContainsArg(link_cmd.arguments, "_start"));
    CHECK(ContainsArg(link_cmd.arguments, "-nostdlib"));

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

static void TestStepPresetsAndRunRunnerE2E() {
    std::cout << "[TEST] TestStepPresetsAndRunRunnerE2E...\n";
    auto presets = core::GetBuiltInStepPresets();
    CHECK(presets.size() >= 4);

    fs::path tmp = MakeTempDir("v2c_run_e2e");
    WriteText(tmp / "main.cpp",
        "#include <cstdlib>\n"
        "#include <iostream>\n"
        "#include <string>\n"
        "int main() {\n"
        "    const char* v = std::getenv(\"TEST_RUN_ENV\");\n"
        "    std::string line;\n"
        "    if (std::getline(std::cin, line)) {\n"
        "        std::cout << \"ECHO:\" << line << \"|ENV:\" << (v ? v : \"none\") << std::endl;\n"
        "    }\n"
        "    return 7;\n"
        "}\n");

    core::Project proj;
    proj.Load(tmp);
    core::Target t = proj.GetTargets()[0];
    core::SourceRule sr;
    sr.include = "main.cpp";
    t.sources = { sr };
    t.run.command = "{bin}";
    t.run.env = { { "TEST_RUN_ENV", "hello_v2c" } };
    t.run.buildBeforeRun = true;
    proj.UpdateTarget(t);
    proj.AddFile("main.cpp");

    core::CompilerManager cm;
    CHECK(cm.HasCompilers());
    core::TrustStore::SetTrusted(tmp, true);

    core::BuildRunner runner;
    std::string berr;
    CHECK(runner.StartBuild(proj, cm, berr, t.name));
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    auto res = runner.GetLastResult();
    CHECK(res.success);

    // Check FormulateRunCommand expands placeholders & env
    auto tc_res = cm.ResolveProjectToolchain(proj);
    fs::path run_cwd;
    std::unordered_map<std::string, std::string> run_env;
    core::BuildCommand rc = core::FormulateRunCommand(proj, t, t.run, &tc_res.profile, run_cwd, run_env);
    CHECK(!rc.program.empty());
    CHECK(run_env["TEST_RUN_ENV"] == "hello_v2c");

    // Launch real compiled binary under RunRunner with stdin/stdout streaming
    core::RunRunner rr;
    std::string rerr;
    CHECK(rr.Start(rc, run_cwd, &run_env, &rerr));
    CHECK(rr.SendStdin("ping123"));

    std::vector<core::OutLine> captured;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto chunk = rr.DrainOutput();
        captured.insert(captured.end(), chunk.begin(), chunk.end());
        if (!rr.IsRunning()) {
            auto tail = rr.DrainOutput();
            captured.insert(captured.end(), tail.begin(), tail.end());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK(!rr.IsRunning());
    CHECK(rr.GetLastExitCode() == 7);

    bool saw_echo = false;
    for (const auto& ol : captured) {
        if (ol.text.find("ECHO:ping123|ENV:hello_v2c") != std::string::npos) {
            saw_echo = true;
            break;
        }
    }
    CHECK(saw_echo);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

int main() {
    std::cout << std::unitbuf;
    TestMsvcEnvCacheAndEnvMerge();
    TestToolchainSerializationAndResolution();
    TestFreestandingKernelAndLinkerScript();
    TestStepPresetsAndRunRunnerE2E();
    std::cout << "ALL V2-C TESTS PASSED\n";
    return 0;
}
