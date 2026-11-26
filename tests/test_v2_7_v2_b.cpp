#include "core/dep_log.h"
#include "core/tool_registry.h"
#include "core/pch.h"
#include "core/unity.h"
#include "core/compiler_cache.h"
#include "core/scheduler.h"
#include "core/project.h"
#include "core/target.h"
#include "core/settings_resolver.h"
#include "core/build_steps.h"
#include "core/compiler.h"
#include "core/build_runner.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #expr "\n"; \
        g_failures++; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #a " != " #b \
                  << " (" << (a) << " vs " << (b) << ")\n"; \
        g_failures++; \
    } \
} while (0)

static void WriteText(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

static core::BuildResult RunBlockingBuild(core::Project& proj, core::CompilerManager& cm, bool rebuild, const std::string& target = "") {
    core::TrustStore::SetTrusted(proj.GetRootPath(), true);
    core::BuildRunner runner;
    std::string err;
    bool started = rebuild
        ? runner.StartRebuild(proj, cm, err, target)
        : runner.StartBuild(proj, cm, err, target);
    if (!started) {
        std::cerr << "StartBuild failed: " << err << "\n";
        core::BuildResult res;
        res.success = false;
        res.exit_code = -1;
        return res;
    }
    while (runner.IsRunning()) {
        runner.DrainOutput();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    auto leftover = runner.DrainOutput();
    if (!runner.GetLastResult().success) {
        for (const auto& ol : leftover) {
            std::cerr << "  [build] " << ol.text << "\n";
        }
    }
    return runner.GetLastResult();
}

static void ConfigureSingleTargetProject(core::Project& proj, const fs::path& root, const core::Target& tgt, const core::CompilerManager& cm) {
    fs::create_directories(root);
    WriteText(root / ".geniebuild" / "project.json", "{ \"version\": 2, \"targets\": [] }\n");
    proj.Load(root);
    std::string err;
    std::string old_primary_name;
    if (const core::Target* primary = proj.GetPrimaryTarget()) {
        old_primary_name = primary->name;
    }
    if (!old_primary_name.empty()) {
        core::Target t = tgt;
        if (t.name == old_primary_name) {
            proj.UpdateTarget(t);
        } else {
            proj.AddTarget(t, err);
            proj.RemoveTarget(old_primary_name, err);
            proj.SetStartupTarget(t.name);
        }
    } else {
        proj.AddTarget(tgt, err);
    }
    proj.ExpandAllSourceRules();
    if (!cm.GetDetectedCompilers().empty()) {
        proj.SetCompiler(cm.GetDetectedCompilers().front().name);
    }
}

static void TestDepLog(const fs::path& root) {
    std::cout << "Running V2-11 DepLog unit tests..." << std::endl;
    fs::path droot = root / "deplog";
    fs::remove_all(droot);
    fs::create_directories(droot);

    fs::path bin_path = droot / "deps.bin";
    fs::path h1 = droot / "inc" / "common.h";
    fs::path h2 = droot / "inc" / "rare.h";
    WriteText(h1, "#pragma once\ninline int k() { return 1; }\n");
    WriteText(h2, "#pragma once\n");

    {
        core::DepLog log;
        CHECK(log.Open(bin_path));
        log.RecordOutput((droot / "a.o").generic_u8string(), 1000, 0x11223344ULL, 120, { h1.generic_u8string() });
        log.RecordOutput((droot / "b.o").generic_u8string(), 2000, 0x55667788ULL, 80, { h1.generic_u8string(), h2.generic_u8string() });
        log.RecordStep("pre::gen", 0xCAFEBABEULL, 3000, { h2.generic_u8string() });
        log.Close();
    }

    // Corrupt-tail recovery: append partial garbage bytes
    {
        std::ofstream f(bin_path, std::ios::binary | std::ios::app);
        const char junk[7] = { 'B', 'A', 'D', 1, 2, 3, 4 };
        f.write(junk, sizeof(junk));
    }

    {
        core::DepLog log;
        CHECK(log.Open(bin_path));
        core::DepOutputEntry ea, eb;
        CHECK(log.TryGetOutput((droot / "a.o").generic_u8string(), ea));
        CHECK_EQ(ea.cmdHash, 0x11223344ULL);
        CHECK_EQ(ea.durationMs, 120U);
        CHECK_EQ(ea.depIds.size(), 1U);
        CHECK(log.TryGetOutput((droot / "b.o").generic_u8string(), eb));
        CHECK_EQ(eb.depIds.size(), 2U);

        core::DepStepEntry es;
        CHECK(log.TryGetStep("pre::gen", es));
        CHECK_EQ(es.cmdHash, 0xCAFEBABEULL);

        // SuggestPchHeaders: h1 is in 2/2 TUs (100%), h2 is in 1/2 (50%), check ranking
        auto cands = log.SuggestPchHeaders(
            { (droot / "a.o").generic_u8string(), (droot / "b.o").generic_u8string() },
            droot, 0.5, 10
        );
        CHECK(cands.size() >= 1);
        if (!cands.empty()) {
            CHECK_EQ(cands[0].path, "inc/common.h");
            CHECK_EQ(cands[0].tuCount, 2U);
        }

        // BatchStatFiles accuracy vs DepLog::StatFileNs
        auto st = core::DepLog::BatchStatFiles({ h1.generic_u8string(), h2.generic_u8string(), (droot / "missing.h").generic_u8string() });
        CHECK(st.count(core::DepLog::NormalizeDepPath(h1.generic_u8string())) == 1);
        CHECK(st.count(core::DepLog::NormalizeDepPath(droot.generic_u8string() + "/missing.h")) == 0);
        int64_t expected_ns = core::DepLog::StatFileNs(h1);
        int64_t got_ns = st[core::DepLog::NormalizeDepPath(h1.generic_u8string())];
        CHECK_EQ(got_ns, expected_ns);

        log.Close();
    }

    // Compaction test (>50% dead records and >200 records triggers compaction)
    {
        core::DepLog log;
        CHECK(log.Open(bin_path));
        for (int i = 0; i < 250; ++i) {
            log.RecordOutput((droot / "a.o").generic_u8string(), 1000 + i, 0x100ULL + i, 50, { h1.generic_u8string() });
        }
        log.Close();
        uintmax_t size_before_reopen = fs::file_size(bin_path);
        CHECK(log.Open(bin_path));
        log.Close();
        uintmax_t size_after_compact = fs::file_size(bin_path);
        CHECK(size_after_compact < size_before_reopen);
    }
}

static void TestToolRegistryAndRc(const fs::path& root, core::CompilerManager& cm) {
    std::cout << "Running V2-7 ToolRegistry & Extra Source tests..." << std::endl;
    CHECK(core::ToolRegistry::ClassifySourceFile("res/app.rc") == core::SourceFileKind::Rc);
    CHECK(core::ToolRegistry::ClassifySourceFile("asm/fast.S") == core::SourceFileKind::AsmPre);
    CHECK(core::ToolRegistry::ClassifySourceFile("asm/raw.s") == core::SourceFileKind::AsmRaw);
    CHECK(core::ToolRegistry::ClassifySourceFile("asm/simd.asm") == core::SourceFileKind::AsmNasm);
    CHECK(core::ToolRegistry::ClassifySourceFile("gpu/k.cu") == core::SourceFileKind::Cuda);
    CHECK(core::ToolRegistry::ClassifySourceFile("simd/mix.ispc") == core::SourceFileKind::Ispc);

    core::CustomToolSpec glsl;
    glsl.ext = ".glsl";
    glsl.program = "glslc";
    glsl.args = "{in} -o {out}";
    glsl.output = "{objdir}/{name}.spv";
    CHECK(core::ToolRegistry::ClassifySourceFile("shaders/tri.glsl", { glsl }) == core::SourceFileKind::Custom);

    fs::path troot = root / "toolreg";
    fs::remove_all(troot);
    fs::create_directories(troot / "src");
    WriteText(troot / "src" / "res.h", "#define IDS_HELLO 101\n");
    WriteText(troot / "src" / "extra.txt", "RES_DATA_V1");
    WriteText(troot / "src" / "app.rc", "#include \"res.h\"\nIDS_HELLO RCDATA \"extra.txt\"\n");
    WriteText(troot / "src" / "main.cpp", "int main() { return 0; }\n");

    auto deps = core::ToolRegistry::ScanRcDependencies(troot / "src" / "app.rc", troot, { "src" });
    CHECK(deps.size() >= 2);
    std::string h1 = core::ToolRegistry::ComputeRcContentHash(troot / "src" / "app.rc", troot, { "src" });
    WriteText(troot / "src" / "extra.txt", "RES_DATA_V2");
    std::string h2 = core::ToolRegistry::ComputeRcContentHash(troot / "src" / "app.rc", troot, { "src" });
    CHECK(h1 != h2);

    std::string breakdown = core::ToolRegistry::FormatTypeBreakdown({ "src/main.cpp", "src/app.rc", "src/k.cu" }, {});
    CHECK(breakdown.find("C++") != std::string::npos);
    CHECK(breakdown.find("rc") != std::string::npos);
    CHECK(breakdown.find("CUDA") != std::string::npos);

    std::string windres = core::ToolRegistry::ResolveToolExecutable("windres");
    if (!windres.empty() && !cm.GetDetectedCompilers().empty()) {
        core::Target tgt;
        tgt.name = "rc_app";
        tgt.kind = core::TargetKind::Executable;
        tgt.files = { "src/main.cpp", "src/app.rc" };
        tgt.expandedFiles = tgt.files;

        core::Project proj;
        ConfigureSingleTargetProject(proj, troot, tgt, cm);
        auto r1 = RunBlockingBuild(proj, cm, true);
        CHECK(r1.success);
        CHECK(fs::exists(tgt.GetBinaryPath(proj.GetAbsoluteOutputDir(), proj.GetConfiguration(), false)));
        auto r2 = RunBlockingBuild(proj, cm, false);
        CHECK(r2.success);
    } else {
        std::cout << "  (skipping live windres build; windres not on PATH)\n";
    }
}

static void TestPchEndToEnd(const fs::path& root, core::CompilerManager& cm) {
    std::cout << "Running V2-8 PCH unit + E2E tests..." << std::endl;
    if (cm.GetDetectedCompilers().empty()) return;

    fs::path proot = root / "pch_e2e";
    fs::remove_all(proot);
    WriteText(proot / "src" / "common_pch.h", "#pragma once\n#include <string>\n#define PCH_VAL 7\n");
    WriteText(proot / "src" / "a.cpp", "#include \"src/common_pch.h\"\nint fn_a() { return PCH_VAL + 1; }\n");
    WriteText(proot / "src" / "b.cpp", "#include \"src/common_pch.h\"\nint fn_b() { return PCH_VAL + 2; }\n");
    WriteText(proot / "src" / "excluded.cpp", "int fn_ex() { return 3; }\n");
    WriteText(proot / "src" / "main.cpp", "int fn_a(); int fn_b(); int fn_ex(); int main() { return (fn_a() + fn_b() + fn_ex() == 20) ? 0 : 1; }\n");

    core::Target tgt;
    tgt.name = "pch_app";
    tgt.kind = core::TargetKind::Executable;
    tgt.files = { "src/a.cpp", "src/b.cpp", "src/excluded.cpp", "src/main.cpp" };
    tgt.expandedFiles = tgt.files;
    tgt.pch.header = "src/common_pch.h";
    tgt.pch.exclude = { "src/excluded.cpp" };
    core::FlagOverride fo;
    fo.match = "src/b.cpp";
    fo.settings.defines = { "EXTRA_DEF=1" };
    tgt.overrides.push_back(fo);

    core::Project proj;
    ConfigureSingleTargetProject(proj, proot, tgt, cm);

    const core::CompilerInfo* comp = cm.FindCompiler(proj.GetCompiler());
    CHECK(comp != nullptr);
    bool is_msvc = (comp->name == "cl");
    bool is_clang = (comp->name.find("clang") != std::string::npos);
    std::unordered_map<std::string, const core::Target*> tmap = { { tgt.name, proj.FindTarget(tgt.name) } };
    auto eff_tgt = core::ResolveEffectiveTargetSettings(proj, tgt, tmap, proj.GetConfiguration(), is_msvc);
    std::unordered_map<std::string, std::shared_ptr<const core::EffectiveFileSettings>> feffs;
    for (const auto& f : tgt.expandedFiles) {
        feffs[f] = std::make_shared<const core::EffectiveFileSettings>(
            core::ResolveFileSettings(proj, tgt, eff_tgt, f, proj.GetConfiguration(), proj.GetCompiler(), false)
        );
    }
    auto an = core::PchManager::AnalyzeAndPlanTargetPch(
        proj, tgt, eff_tgt, tgt.expandedFiles, feffs, *comp, *comp, proj.GetConfiguration(), is_msvc, is_clang, false, false
    );
    CHECK(an.cxxPch.enabled);
    CHECK_EQ(an.includedCount, 2U); // a.cpp + main.cpp
    CHECK_EQ(an.totalCount, 4U);

    auto r1 = RunBlockingBuild(proj, cm, true);
    CHECK(r1.success);
    fs::path bin = tgt.GetBinaryPath(proj.GetAbsoluteOutputDir(), proj.GetConfiguration(), is_msvc);
    CHECK(fs::exists(bin));
    if (fs::exists(bin)) {
        CHECK_EQ(std::system(("\"" + bin.string() + "\"").c_str()), 0);
    }
}

static void TestUnityEndToEnd(const fs::path& root, core::CompilerManager& cm) {
    std::cout << "Running V2-9 Unity + Adaptive Hot-File tests..." << std::endl;
    if (cm.GetDetectedCompilers().empty()) return;

    fs::path uroot = root / "unity_e2e";
    fs::remove_all(uroot);
    WriteText(uroot / "src" / "u1.cpp", "int u1() { return 1; }\n");
    WriteText(uroot / "src" / "u2.cpp", "int u2() { return 2; }\n");
    WriteText(uroot / "src" / "u3.cpp", "int u3() { return 3; }\n");
    WriteText(uroot / "src" / "u4.cpp", "int u1(); int u2(); int u3(); int main() { return (u1()+u2()+u3() == 6) ? 0 : 1; }\n");

    core::Target tgt;
    tgt.name = "unity_app";
    tgt.kind = core::TargetKind::Executable;
    tgt.files = { "src/u1.cpp", "src/u2.cpp", "src/u3.cpp", "src/u4.cpp" };
    tgt.expandedFiles = tgt.files;
    tgt.unity.enabled = true;
    tgt.unity.batchSize = 2;
    tgt.unity.adaptive = true;

    core::Project proj;
    ConfigureSingleTargetProject(proj, uroot, tgt, cm);

    const core::CompilerInfo* comp = cm.FindCompiler(proj.GetCompiler());
    bool is_msvc = (comp->name == "cl");
    std::unordered_map<std::string, const core::Target*> tmap = { { tgt.name, proj.FindTarget(tgt.name) } };
    auto eff_tgt = core::ResolveEffectiveTargetSettings(proj, tgt, tmap, proj.GetConfiguration(), is_msvc);
    std::unordered_map<std::string, std::shared_ptr<const core::EffectiveFileSettings>> feffs;
    std::unordered_map<std::string, bool> uses_pch;
    for (const auto& f : tgt.expandedFiles) {
        feffs[f] = std::make_shared<const core::EffectiveFileSettings>(
            core::ResolveFileSettings(proj, tgt, eff_tgt, f, proj.GetConfiguration(), proj.GetCompiler(), false)
        );
    }

    auto p1 = core::UnityManager::PlanTargetUnity(proj, tgt, tgt.expandedFiles, feffs, uses_pch, proj.GetConfiguration(), proj.GetCompiler(), is_msvc, nullptr, true);
    CHECK_EQ(p1.batches.size(), 2U);
    auto mtime1 = fs::last_write_time(p1.batches[0].generatedFileAbs);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto p2 = core::UnityManager::PlanTargetUnity(proj, tgt, tgt.expandedFiles, feffs, uses_pch, proj.GetConfiguration(), proj.GetCompiler(), is_msvc, nullptr, true);
    CHECK(fs::last_write_time(p2.batches[0].generatedFileAbs) == mtime1);

    auto br = RunBlockingBuild(proj, cm, true);
    CHECK(br.success);
    fs::path bin = tgt.GetBinaryPath(proj.GetAbsoluteOutputDir(), proj.GetConfiguration(), is_msvc);
    CHECK(fs::exists(bin));
    if (fs::exists(bin)) {
        CHECK_EQ(std::system(("\"" + bin.string() + "\"").c_str()), 0);
    }

    // Adaptive hot-file tracking + 1-build graduation check:
    core::UnityPlan hot_plan = p2;
    hot_plan.hotFiles = { "src/u1.cpp" };
    core::UnityManager::SaveAdaptiveHotState(proj, tgt, proj.GetConfiguration(), hot_plan);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    WriteText(uroot / "src" / "u1.cpp", "int u1() { return 1; } // modified while hot\n");
    auto p_hot = core::UnityManager::PlanTargetUnity(proj, tgt, tgt.expandedFiles, feffs, uses_pch, proj.GetConfiguration(), proj.GetCompiler(), is_msvc, nullptr, false);
    CHECK_EQ(p_hot.hotFiles.size(), 1U);
    if (!p_hot.hotFiles.empty()) {
        CHECK_EQ(p_hot.hotFiles[0], "src/u1.cpp");
    }
    // Save state at current mtime, leave untouched, and verify it graduates back into batches next build:
    core::UnityManager::SaveAdaptiveHotState(proj, tgt, proj.GetConfiguration(), p_hot);
    auto p_grad = core::UnityManager::PlanTargetUnity(proj, tgt, tgt.expandedFiles, feffs, uses_pch, proj.GetConfiguration(), proj.GetCompiler(), is_msvc, nullptr, false);
    CHECK_EQ(p_grad.hotFiles.size(), 0U);
    CHECK_EQ(p_grad.batches.size(), 2U);
}

static void TestCacheLinkerSchedulerKeepGoing(const fs::path& root, core::CompilerManager& cm) {
    std::cout << "Running V2-10 Cache, V2-12 Linker, V2-13 Scheduler + KeepGoing tests..." << std::endl;

    // V2-10: Launcher excluded from hash + PopulateEnv
    core::BuildCommand cmd;
    cmd.program = "g++";
    cmd.arguments = { "-c", "main.cpp", "-o", "main.o" };
    std::string hash_plain = core::CompilerManager::ComputeCommandHash(cmd);
    core::DetectedCompilerCache fake_ccache;
    fake_ccache.kind = core::CacheKind::Ccache;
    fake_ccache.name = "ccache";
    fake_ccache.executablePath = "ccache";
    fake_ccache.active = true;
    core::BuildCommand wrapped = cmd;
    core::CompilerCacheManager::PrependLauncher(wrapped, fake_ccache);
    CHECK_EQ(wrapped.program, "ccache");
    CHECK_EQ(wrapped.arguments.front(), "g++");
    CHECK_EQ(hash_plain, core::CompilerManager::ComputeCommandHash(cmd));

    std::unordered_map<std::string, std::string> env;
    core::CompilerCachePrefs prefs;
    prefs.maxSize = "15G";
    core::CompilerCacheManager::PopulateEnv(env, fake_ccache, prefs, true);
    CHECK_EQ(env["CCACHE_MAXSIZE"], "15G");
    CHECK(env["CCACHE_SLOPPINESS"].find("pch_defines") != std::string::npos);

    // V2-12: Linker validation & flag emission
    core::Project p_link;
    fs::create_directories(root / "lnk_cfg");
    WriteText(root / "lnk_cfg" / ".geniebuild" / "project.json", "{ \"version\": 2, \"targets\": [] }\n");
    p_link.Load(root / "lnk_cfg");
    p_link.SetSplitDwarf(true);
    std::string lerr;
    CHECK(!core::CompilerManager::ValidateLinkerOptions(p_link, true, false, lerr));
    p_link.SetSplitDwarf(false);
    p_link.SetDebugLink("fastlink");
    CHECK(!core::CompilerManager::ValidateLinkerOptions(p_link, false, false, lerr));

    core::CompilerInfo gcc;
    gcc.name = "g++";
    gcc.executable_path = "g++";
    core::Target t_link;
    t_link.name = "app";
    t_link.kind = core::TargetKind::Executable;
    core::EffectiveTargetSettings eff_link;
    p_link.SetConfiguration("Debug");
    p_link.SetLinker("gold");
    p_link.SetDebugLink("split-dwarf");
    p_link.SetSplitDwarf(true);
    auto lcmd = core::CompilerManager::FormulateLinkCommand(gcc, t_link, eff_link, { "a.o" }, "app.exe", p_link, false);
    bool has_gold = false, has_gdb_idx = false;
    for (const auto& a : lcmd.arguments) {
        if (a == "-fuse-ld=gold") has_gold = true;
        if (a == "-Wl,--gdb-index") has_gdb_idx = true;
    }
    CHECK(has_gold);
    CHECK(has_gdb_idx);

    // V2-13: Scheduler RAM cap
    auto wb = core::Scheduler::ComputeWorkerBudget(0, 1000000);
    CHECK_EQ(wb.workers, 1);
    CHECK(wb.ramCapped);

    // V2-13: KeepGoing multi-target E2E (one failing target + one good target)
    if (!cm.GetDetectedCompilers().empty()) {
        fs::path kroot = root / "keep_going";
        fs::remove_all(kroot);
        fs::create_directories(kroot);
        WriteText(kroot / "src" / "bad.cpp", "this is invalid c++ syntax ???;\n");
        WriteText(kroot / "src" / "good.cpp", "int main() { return 0; }\n");

        core::Project kp;
        WriteText(kroot / ".geniebuild" / "project.json", "{ \"version\": 2, \"targets\": [] }\n");
        kp.Load(kroot);
        kp.SetCompiler(cm.GetDetectedCompilers().front().name);
        kp.SetKeepGoing(true);

        std::string err;
        std::string old_primary;
        if (const core::Target* primary = kp.GetPrimaryTarget()) {
            old_primary = primary->name;
        }

        core::Target tbad;
        tbad.name = "bad";
        tbad.kind = core::TargetKind::Executable;
        tbad.files = { "src/bad.cpp" };
        tbad.expandedFiles = tbad.files;

        core::Target tgood;
        tgood.name = "good";
        tgood.kind = core::TargetKind::Executable;
        tgood.files = { "src/good.cpp" };
        tgood.expandedFiles = tgood.files;

        kp.AddTarget(tbad, err);
        kp.AddTarget(tgood, err);
        if (!old_primary.empty()) {
            kp.RemoveTarget(old_primary, err);
        }
        kp.SetStartupTarget("good");

        auto res = RunBlockingBuild(kp, cm, true);
        CHECK(!res.success);
        bool is_msvc = (kp.GetCompiler() == "cl");
        CHECK(fs::exists(tgood.GetBinaryPath(kp.GetAbsoluteOutputDir(), kp.GetConfiguration(), is_msvc)));
        CHECK(!fs::exists(tbad.GetBinaryPath(kp.GetAbsoluteOutputDir(), kp.GetConfiguration(), is_msvc)));
    }
}

int main() {
    fs::path temp_root = fs::temp_directory_path() / "geniebuild_v2_7_v2_b_tests";
    fs::remove_all(temp_root);
    fs::create_directories(temp_root);

    core::CompilerManager cm;
    cm.DetectCompilers();

    TestDepLog(temp_root);
    TestToolRegistryAndRc(temp_root, cm);
    TestPchEndToEnd(temp_root, cm);
    TestUnityEndToEnd(temp_root, cm);
    TestCacheLinkerSchedulerKeepGoing(temp_root, cm);

    std::error_code ec;
    fs::remove_all(temp_root, ec);

    if (g_failures == 0) {
        std::cout << "ALL V2-7 and V2-B TESTS PASSED.\n";
        return 0;
    }
    std::cerr << g_failures << " TEST(S) FAILED.\n";
    return 1;
}
