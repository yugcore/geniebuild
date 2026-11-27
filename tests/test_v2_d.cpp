#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "build_log.h"
#include "build_runner.h"
#include "compdb.h"
#include "compiler.h"
#include "fs_util.h"
#include "hints.h"
#include "child_process.h"
#include "project.h"
#include "settings_resolver.h"
#include "target.h"

#include <nlohmann/json.hpp>

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
    core::fs_util::RemoveAllLong(p, ec);
    core::fs_util::CreateDirsLong(p, ec);
    return p;
}

static void WriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    core::fs_util::CreateDirsLong(p.parent_path(), ec);
    CHECK(core::fs_util::WriteFileTextLong(p, text));
}

static void TestChildProcessStressAndCancel() {
    std::cout << "[TEST] TestChildProcessStressAndCancel...\n";
    core::CompilerManager cm;
    CHECK(cm.HasCompilers());

    // Stress test launching 30 short processes via ChildProcess cleanly (V2-18)
    core::BuildCommand ping_cmd;
#if defined(_WIN32)
    ping_cmd.program = "cmd.exe";
    ping_cmd.arguments = { "/c", "echo", "hello_proc" };
#else
    ping_cmd.program = "sh";
    ping_cmd.arguments = { "-c", "echo hello_proc" };
#endif
    for (int i = 0; i < 30; ++i) {
        core::ChildProcess cp;
        core::ProcessOptions opts;
        CHECK(cp.Start(ping_cmd, opts));
        uint8_t buf[256];
        bool eof = false;
        std::string cap;
        while (!eof) {
            std::ptrdiff_t n = cp.ReadStdout(buf, sizeof(buf), eof);
            if (n > 0) cap.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
        }
        CHECK(cp.WaitExit(2000) == 0);
        CHECK(cap.find("hello_proc") != std::string::npos);
    }

    // Cancel running build test: pre-build sleep step + normal cpp compile (V2-18)
    fs::path tmp = MakeTempDir("v2d_cancel");
    WriteText(tmp / "src/a.cpp", "int fa() { return 1; }\n");
    WriteText(tmp / "src/main.cpp", "int fa(); int main() { return fa(); }\n");

    core::Project proj;
    CHECK(proj.Load(tmp));
    core::Target t = proj.GetTargets()[0];
    core::SourceRule sr;
    sr.include = "src/*.cpp";
    t.sources = { sr };
    core::BuildStep sleep_step;
    sleep_step.name = "slow_gen";
    sleep_step.phase = core::StepPhase::PreBuild;
    sleep_step.shell = true;
    sleep_step.always = true;
    sleep_step.outputs = { ".geniebuild/gen/Debug/slow.txt" };
#if defined(_WIN32)
    sleep_step.command = "powershell -NoProfile -Command \"Start-Sleep -Seconds 8\"";
#else
    sleep_step.command = "sleep 8";
#endif
    t.steps = { sleep_step };
    proj.UpdateTarget(t);
    core::TrustStore::SetTrusted(tmp, true);

    core::BuildRunner runner;
    std::string berr;
    if (!runner.StartBuild(proj, cm, berr, t.name)) {
        std::cerr << "[FAIL] TestLongPaths StartBuild error: " << berr << "\n";
        CHECK(false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(runner.IsRunning());

    auto c_t0 = std::chrono::steady_clock::now();
    runner.CancelBuild();
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    double cancel_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c_t0).count();
    std::cout << "  cancel latency = " << cancel_ms << " ms\n";
    CHECK(cancel_ms < 2500.0);
    auto res = runner.GetLastResult();
    CHECK(res.cancelled);
    CHECK(!res.success);
    CHECK(!fs::exists(tmp / ".geniebuild/gen/Debug/slow.txt"));

    // Next build without sleep finishes cleanly
    t.steps.clear();
    proj.UpdateTarget(t);
    CHECK(runner.StartBuild(proj, cm, berr, t.name));
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(runner.GetLastResult().success);

    std::error_code ec;
    core::fs_util::RemoveAllLong(tmp, ec);

    // Parallel ChildProcess pipe isolation regression (fast child must NOT block on slow sibling's pipe)
    {
        core::BuildCommand fast_cmd, slow_cmd;
#if defined(_WIN32)
        fast_cmd.program = "cmd.exe";
        fast_cmd.arguments = { "/c", "echo", "fast_ok" };
        slow_cmd.program = "cmd.exe";
        slow_cmd.arguments = { "/c", "ping", "-n", "3", "127.0.0.1" };
#else
        fast_cmd.program = "sh";
        fast_cmd.arguments = { "-c", "echo fast_ok" };
        slow_cmd.program = "sh";
        slow_cmd.arguments = { "-c", "sleep 2.0" };
#endif
        core::ChildProcess slow_p;
        core::ProcessOptions opts;
        CHECK(slow_p.Start(slow_cmd, opts));

        auto ft0 = std::chrono::steady_clock::now();
        core::ChildProcess fast_p;
        CHECK(fast_p.Start(fast_cmd, opts));
        uint8_t fbuf[256];
        bool feof = false;
        while (!feof) {
            std::ptrdiff_t n = fast_p.ReadStdout(fbuf, sizeof(fbuf), feof);
            if (n <= 0) break;
        }
        CHECK(fast_p.WaitExit(2000) == 0);
        double fast_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ft0).count();
        std::cout << "  concurrent fast child latency = " << fast_ms << " ms\n";
        CHECK(fast_ms < 800.0);

        slow_p.TerminateTree();
        slow_p.WaitExit(2000);
    }
}

static void TestCompileCommandsExportAndNoOpMtime() {
    std::cout << "[TEST] TestCompileCommandsExportAndNoOpMtime...\n";
    fs::path tmp = MakeTempDir("v2d_compdb_exp");
    WriteText(tmp / "src/pch.h", "#pragma once\n#include <vector>\n");
    WriteText(tmp / "src/a.cpp", "int a_fn() { std::vector<int> v{1}; return (int)v.size(); }\n");
    WriteText(tmp / "src/b.cpp", "int b_fn() { return 2; }\n");
    WriteText(tmp / ".geniebuild/gen/Debug/app/gen_extra.cpp", "int g_fn() { return 3; }\n");

    core::Project proj;
    CHECK(proj.Load(tmp));
    core::Target t = proj.GetTargets()[0];
    t.name = "app";
    core::SourceRule sr;
    sr.include = "src/*.cpp";
    t.sources = { sr };
    t.pch.header = "src/pch.h";
    t.unity.enabled = true; // Verify compdb still exports per-file entries (V2-19)
    t.priv.defines = { "APP_DEF=42" };
    std::string aerr;
    CHECK(proj.RenameTarget(proj.GetTargets()[0].name, "app", aerr));
    proj.UpdateTarget(t);
    proj.SaveImmediate();

    core::CompilerManager cm;
    auto exp1 = core::CompDbManager::ExportCompileCommands(proj, cm);
    CHECK(exp1.success);
    CHECK(exp1.fileModified);
    // Should contain a.cpp, b.cpp, and generated gen_extra.cpp individually (3 entries, not unity batch)
    CHECK(exp1.entryCount == 3);
    CHECK(fs::exists(exp1.outputPath));

    std::string raw_cd;
    CHECK(core::fs_util::ReadFileTextLong(exp1.outputPath, raw_cd));
    nlohmann::json cdj = nlohmann::json::parse(raw_cd);
    CHECK(cdj.is_array() && cdj.size() == 3);
    for (const auto& item : cdj) {
        CHECK(item["file"].get<std::string>().find("unity_") == std::string::npos);
        CHECK(item.contains("arguments") && item["arguments"].is_array());
        std::string flat;
        for (const auto& arg : item["arguments"]) flat += arg.get<std::string>() + " ";
        CHECK(flat.find("-include-pch") == std::string::npos);
        CHECK(flat.find(".gch") == std::string::npos);
        CHECK(flat.find("pch.h") != std::string::npos);
    }

    auto mtime1 = fs::last_write_time(exp1.outputPath);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto exp2 = core::CompDbManager::ExportCompileCommands(proj, cm);
    CHECK(exp2.success);
    CHECK(!exp2.fileModified);
    auto mtime2 = fs::last_write_time(exp2.outputPath);
    CHECK(mtime1 == mtime2);

    std::error_code ec;
    core::fs_util::RemoveAllLong(tmp, ec);
}

static void TestProgressSummaryAnd2MLineLog() {
    std::cout << "[TEST] TestProgressSummaryAnd2MLineLog...\n";
    fs::path tmp = MakeTempDir("v2d_log_2m");
    fs::path big_log = tmp / "synthetic_2m.log";
    {
        std::ofstream out(big_log, std::ios::binary | std::ios::trunc);
        CHECK(out.is_open());
        for (int i = 0; i < 2000000; ++i) {
            if (i == 500000 || i == 1500000) {
                out << "src/engine/renderer.cpp(" << i << "): error C2065: undeclared identifier\n";
            } else if (i == 750000) {
                out << "src/engine/core.cpp:12:5: warning: unused variable 'x'\n";
            } else {
                out << "[1/100] compiling unit_" << (i % 100) << ".cpp ok\n";
            }
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    core::BuildLog log;
    CHECK(log.LoadFromFile(big_log));
    double idx_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  indexed 2M lines in " << idx_ms << " ms\n";

    CHECK(log.GetTotalLines() == 2000000);
    CHECK(log.GetErrorCount() == 2);
    CHECK(log.GetWarningCount() == 1);

    // Verify F8 forward/backward jump order
    size_t e1 = log.FindNextError(0, true);
    size_t e2 = log.FindNextError(e1, true);
    size_t e_back = log.FindNextError(e2, false);
    CHECK(e1 == 500000);
    CHECK(e2 == 1500000);
    CHECK(e_back == 500000);

    // Random line access via LRU / seekg
    core::LogLineEntry meta;
    std::string txt;
    CHECK(log.GetLineMetaAndText(1500000, meta, txt));
    CHECK(txt.find("error C2065") != std::string::npos);

    // Filtered error index
    auto errs = log.GetFilteredIndices(core::LogFilterMode::Errors, "", "");
    CHECK(errs.size() == 2);
    CHECK(errs[0] == 500000 && errs[1] == 1500000);

    // Background search
    log.StartSearch("unused variable", false);
    while (log.IsSearching()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto sm = log.GetSearchMatches();
    CHECK(sm.size() == 1 && sm[0] == 750000);

    // Diagnostic location parser (MSVC + GCC)
    WriteText(tmp / "src/foo.cpp", "int main(){return 0;}\n");
    fs::path df;
    int dl = 0, dc = 0;
    CHECK(core::HintsManager::ParseDiagnosticLocation("src/foo.cpp(12,7): error C2065: x", tmp, df, dl, dc));
    CHECK(dl == 12 && dc == 7);
    CHECK(core::HintsManager::ParseDiagnosticLocation("src/foo.cpp:99:4: warning: unused", tmp, df, dl, dc));
    CHECK(dl == 99 && dc == 4);

    std::error_code ec;
    core::fs_util::RemoveAllLong(tmp, ec);
}

static void TestLongPathsShortObjectsAndDefender() {
    std::cout << "[TEST] TestLongPathsShortObjectsAndDefender...\n";
    fs::path tmp = MakeTempDir("v2d_longpath");
    std::string deep_dir = "sub_0123456789abcdef/sub_0123456789abcdef/sub_0123456789abcdef/sub_0123456789abcdef/"
                           "sub_0123456789abcdef/sub_0123456789abcdef/sub_0123456789abcdef/sub_0123456789abcdef/"
                           "sub_0123456789abcdef/sub_0123456789abcdef";
    std::string rel_src = deep_dir + "/deep_source_unit.cpp";
    WriteText(tmp / rel_src, "int deep_val() { return 42; }\n");
    WriteText(tmp / "main.cpp", "int deep_val(); int main() { return deep_val() == 42 ? 0 : 1; }\n");

    core::Project proj;
    CHECK(proj.Load(tmp));
    core::Target t;
    t.name = "deepapp";
    fs::path obj_dir = t.GetObjectDir(tmp, "Debug", "12345678");
    std::string obj_key;
    fs::path safe_obj = core::BuildRunner::ComputeObjectPath(proj, obj_dir, rel_src, false, obj_key);
    CHECK(safe_obj.generic_u8string().find("/h/") != std::string::npos);
    CHECK(safe_obj.generic_u8string().size() < 200);

    // Real compile + link of deep-path source under BuildRunner (verifies short obj path + Phase timing + Slowest files)
    t = proj.GetTargets()[0];
    core::SourceRule sr1; sr1.include = rel_src;
    core::SourceRule sr2; sr2.include = "main.cpp";
    t.sources = { sr1, sr2 };
    proj.UpdateTarget(t);

    core::CompilerManager cm;
    core::BuildRunner runner;
    std::string berr;
    if (!runner.StartBuild(proj, cm, berr, t.name)) {
        std::cerr << "[FAIL] TestLongPaths StartBuild error: " << berr << "\n";
        CHECK(false);
    }
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto res = runner.GetLastResult();
    CHECK(res.success);
    CHECK(res.compiled_count == 2);
    CHECK(res.compile_seconds > 0.0);
    CHECK(!res.slowest_files.empty());
    CHECK(res.compile_commands_ok);

    // V2-23 Defender & DevDrive safety checks
    bool def_active = core::HintsManager::IsWindowsDefenderRealTimeActive();
    bool is_dev = core::HintsManager::IsPathOnDevDrive(tmp);
    (void)def_active;
    (void)is_dev;
    core::UserSettings us;
    us.showPerformanceHints = true;
    CHECK(!core::HintsManager::ShouldSuggestDefenderExclusion(tmp, 100, us)); // <= 500 files must not hint
    us.showPerformanceHints = false;
    CHECK(!core::HintsManager::ShouldSuggestDefenderExclusion(tmp, 2000, us)); // Disabled must not hint

    std::error_code ec;
    core::fs_util::RemoveAllLong(tmp, ec);
}

static void TestCompileCommandsImport() {
    std::cout << "[TEST] TestCompileCommandsImport...\n";
    fs::path tmp = MakeTempDir("v2d_compdb_imp");
    WriteText(tmp / "src/core/core.cpp", "int c() { return 1; }\n");
    WriteText(tmp / "src/core/util.cpp", "int u() { return 2; }\n");
    WriteText(tmp / "src/app/main.cpp", "int main() { return 0; }\n");
    WriteText(tmp / "src/app/simd/vec.cpp", "int v() { return 3; }\n");

    nlohmann::json arr = nlohmann::json::array();
    auto add_entry = [&](const std::string& file_rel, const std::string& out, const std::vector<std::string>& extra) {
        nlohmann::json e = nlohmann::json::object();
        e["directory"] = tmp.generic_u8string();
        e["file"] = (tmp / file_rel).generic_u8string();
        e["output"] = out;
        std::vector<std::string> args = {
            "g++", "-DCOMMON_ALL=1", "-I" + (tmp / "include").generic_u8string(), "-std=c++20", "-Wall"
        };
        for (const auto& x : extra) args.push_back(x);
        args.push_back("-c");
        args.push_back(file_rel);
        args.push_back("-o");
        args.push_back(out);
        e["arguments"] = args;
        arr.push_back(std::move(e));
    };

    add_entry("src/core/core.cpp", "CMakeFiles/core.dir/src/core/core.cpp.obj", { "-DCORE_ONLY=1" });
    add_entry("src/core/util.cpp", "CMakeFiles/core.dir/src/core/util.cpp.obj", { "-DCORE_ONLY=1" });
    add_entry("src/app/main.cpp", "CMakeFiles/editor.dir/src/app/main.cpp.obj", { "-DEDITOR=1", "-O3", "-DNDEBUG", "-g" });
    add_entry("src/app/simd/vec.cpp", "CMakeFiles/editor.dir/src/app/simd/vec.cpp.obj", { "-DEDITOR=1", "-mavx2", "-O3", "-DNDEBUG" });
    add_entry("build/moc_widget.cpp", "CMakeFiles/editor.dir/moc_widget.cpp.obj", { "-DEDITOR=1" });

    // Response-file @includes_CXX.rsp + shared lib (_EXPORTS) detection test
    WriteText(tmp / "src/plugin/plug.cpp", "int plug_fn() { return 7; }\n");
    WriteText(tmp / "build/plug_includes.rsp", "-I" + (tmp / "include").generic_u8string() + " -I" + (tmp / "src/plugin").generic_u8string() + "\n");
    {
        nlohmann::json e = nlohmann::json::object();
        e["directory"] = (tmp / "build").generic_u8string();
        e["file"] = (tmp / "src/plugin/plug.cpp").generic_u8string();
        e["output"] = "CMakeFiles/plug.dir/src/plugin/plug.cpp.obj";
        e["arguments"] = nlohmann::json::array({
            "g++", "-DCOMMON_ALL=1", "-Dplug_EXPORTS", "@plug_includes.rsp", "-std=c++20", "-Wall", "-O3", "-DNDEBUG", "-c",
            (tmp / "src/plugin/plug.cpp").generic_u8string(), "-o", "CMakeFiles/plug.dir/src/plugin/plug.cpp.obj"
        });
        arr.push_back(std::move(e));
    }

    fs::path cc_file = tmp / "build/compile_commands.json";
    WriteText(cc_file, arr.dump(2));

    core::ImportPlan plan = core::CompDbManager::AnalyzeCompileCommands(cc_file, tmp);
    CHECK(plan.success);
    CHECK(plan.excludedGeneratedCount == 1);
    CHECK(plan.targets.size() == 3);
    CHECK(plan.cxxStd == "c++20");
    CHECK(std::find(plan.projectDefines.begin(), plan.projectDefines.end(), "COMMON_ALL=1") != plan.projectDefines.end());
    CHECK(std::find(plan.projectDefines.begin(), plan.projectDefines.end(), "NDEBUG") == plan.projectDefines.end());
    CHECK(std::find(plan.projectCompileFlags.begin(), plan.projectCompileFlags.end(), "-O3") == plan.projectCompileFlags.end());
    CHECK(std::find(plan.projectIncludeDirs.begin(), plan.projectIncludeDirs.end(), "include") != plan.projectIncludeDirs.end());

    const core::ImportedTargetSuggestion* core_t = nullptr;
    const core::ImportedTargetSuggestion* ed_t = nullptr;
    const core::ImportedTargetSuggestion* plug_t = nullptr;
    for (const auto& ts : plan.targets) {
        if (ts.name == "core") core_t = &ts;
        if (ts.name == "editor") ed_t = &ts;
        if (ts.name == "plug") plug_t = &ts;
    }
    CHECK(core_t != nullptr && ed_t != nullptr && plug_t != nullptr);
    CHECK(core_t->kind == core::TargetKind::Static);
    CHECK(plug_t->kind == core::TargetKind::Shared);
    CHECK(std::find(plug_t->includeDirs.begin(), plug_t->includeDirs.end(), "src/plugin") != plug_t->includeDirs.end());
    CHECK(std::find(core_t->defines.begin(), core_t->defines.end(), "CORE_ONLY=1") != core_t->defines.end());
    CHECK(core_t->folders.size() == 1 && core_t->folders[0].useFolderRule);

    // Folder override factored for src/app/simd
    bool found_simd_override = false;
    for (const auto& fg : ed_t->folders) {
        if (fg.folderRel == "src/app/simd") {
            CHECK(std::find(fg.compileFlags.begin(), fg.compileFlags.end(), "-mavx2") != fg.compileFlags.end());
            found_simd_override = true;
        }
    }
    CHECK(found_simd_override);

    std::string werr;
    CHECK(core::CompDbManager::WriteImportedProject(plan, tmp, werr));
    CHECK(fs::exists(tmp / ".geniebuild/project.json"));

    core::Project reloaded;
    CHECK(reloaded.Load(tmp));
    CHECK(reloaded.GetTargets().size() == 3);
    CHECK(reloaded.FindTarget("core") != nullptr);
    CHECK(reloaded.FindTarget("editor") != nullptr);
    CHECK(reloaded.FindTarget("plug") != nullptr);

    // Verify FormulateCompileCommand deduplicates -I. and -Iinclude
    core::CompilerManager cm;
    const core::Target* plug_r = reloaded.FindTarget("plug");
    CHECK(plug_r != nullptr && plug_r->kind == core::TargetKind::Shared);
    auto tc_res = cm.ResolveProjectToolchain(reloaded);
    core::CompilerInfo ci = cm.MakeCompilerInfoFromProfile(tc_res.profile, false);
    core::EffectiveTargetSettings feff;
    feff.includeDirs = { ".", "include", "include/", "src/plugin" };
    core::BuildCommand fcmd = core::CompilerManager::FormulateCompileCommand(ci, "src/plugin/plug.cpp", tmp / "plug.o", reloaded, feff);
    int dot_inc_cnt = 0, inc_inc_cnt = 0;
    for (const auto& a : fcmd.arguments) {
        if (a == "-I.") ++dot_inc_cnt;
        if (a == "-Iinclude" || a == "-Iinclude/") ++inc_inc_cnt;
    }
    CHECK(dot_inc_cnt == 1 && inc_inc_cnt == 1);

    std::error_code ec;
    core::fs_util::RemoveAllLong(tmp, ec);
}

static void TestCompileCommandsCustomCodegenImport() {
    std::cout << "[TEST] TestCompileCommandsCustomCodegenImport...\n";
    // 1) Ninja build.ninja CUSTOM_COMMAND import (generates .cpp + .inc + unwraps cmd.exe /C)
    {
        fs::path tmp = MakeTempDir("v2d_compdb_ninja_codegen");
        WriteText(tmp / "src/app/main.cpp",
            "#include \"gen_val.inc\"\nint gen_fn();\nint main() { return (gen_fn() + GEN_INC_VAL == 42) ? 0 : 1; }\n");
        WriteText(tmp / "proto/spec.txt", "42\n");
        WriteText(tmp / "tools/gen_code.py",
            "import sys, pathlib\n"
            "mode, out_p = sys.argv[1], pathlib.Path(sys.argv[2])\n"
            "out_p.parent.mkdir(parents=True, exist_ok=True)\n"
            "if mode == 'inc':\n"
            "    out_p.write_text('#define GEN_INC_VAL 30\\n', encoding='utf-8')\n"
            "else:\n"
            "    out_p.write_text('int gen_fn() { return 12; }\\n', encoding='utf-8')\n");

        nlohmann::json arr = nlohmann::json::array();
        arr.push_back({
            {"directory", (tmp / "build").generic_u8string()},
            {"file", (tmp / "src/app/main.cpp").generic_u8string()},
            {"output", "CMakeFiles/app.dir/src/app/main.cpp.obj"},
            {"arguments", nlohmann::json::array({
                "g++", "-I" + (tmp / "build/gen").generic_u8string(), "-std=c++17", "-c",
                (tmp / "src/app/main.cpp").generic_u8string(), "-o", "CMakeFiles/app.dir/src/app/main.cpp.obj"
            })}
        });
        arr.push_back({
            {"directory", (tmp / "build").generic_u8string()},
            {"file", (tmp / "build/gen/gen_src.cpp").generic_u8string()},
            {"output", "CMakeFiles/app.dir/gen/gen_src.cpp.obj"},
            {"arguments", nlohmann::json::array({
                "g++", "-std=c++17", "-c", (tmp / "build/gen/gen_src.cpp").generic_u8string(),
                "-o", "CMakeFiles/app.dir/gen/gen_src.cpp.obj"
            })}
        });

        WriteText(tmp / "build/compile_commands.json", arr.dump(2));
        WriteText(tmp / "build/CMakeFiles/app.dir/link.txt",
            "g++ CMakeFiles/app.dir/src/app/main.cpp.obj CMakeFiles/app.dir/gen/gen_src.cpp.obj -o app.exe\n");

        std::string ninja_src =
            "cmake_ninja_workdir = " + (tmp / "build").generic_u8string() + "/\n"
            "build gen/gen_val.inc: CUSTOM_COMMAND ../proto/spec.txt\n"
            "  COMMAND = C:\\WINDOWS\\system32\\cmd.exe /C \"cd /D " + (tmp / "build").generic_u8string() +
            " && python " + (tmp / "tools/gen_code.py").generic_u8string() + " inc gen/gen_val.inc\"\n"
            "  DESC = Generating gen_val.inc\n"
            "build gen/gen_src.cpp: CUSTOM_COMMAND ../proto/spec.txt | gen/gen_val.inc\n"
            "  COMMAND = python " + (tmp / "tools/gen_code.py").generic_u8string() + " cpp gen/gen_src.cpp\n"
            "  DESC = Generating gen_src.cpp\n"
            "build cmake_object_order_depends_target_app: phony || gen/gen_val.inc\n";
        WriteText(tmp / "build/build.ninja", ninja_src);

        core::ImportPlan plan = core::CompDbManager::AnalyzeCompileCommands(tmp / "build/compile_commands.json", tmp);
        CHECK(plan.success);
        CHECK(plan.importedStepCount == 2);
        CHECK(plan.targets.size() == 1);
        CHECK(plan.targets[0].steps.size() == 2);

        bool has_inc_step = false, has_cpp_step = false;
        for (const auto& st : plan.targets[0].steps) {
            if (st.name == "Generating gen_val.inc") {
                has_inc_step = true;
                CHECK(!st.generatesSources);
                CHECK(st.command.find("cmd.exe") == std::string::npos);
            }
            if (st.name == "Generating gen_src.cpp") {
                has_cpp_step = true;
                CHECK(st.generatesSources);
            }
        }
        CHECK(has_inc_step && has_cpp_step);

        // Generated gen_src.cpp is covered by an imported step -> no manual-recreate warning
        for (const auto& w : plan.warningsList) {
            CHECK(w.find("recreate with a build step") == std::string::npos);
        }

        std::string werr;
        CHECK(core::CompDbManager::WriteImportedProject(plan, tmp, werr));

        // Verify imported project actually executes codegen steps + compiles + links + exits 0!
        core::Project proj;
        CHECK(proj.Load(tmp));
        core::TrustStore::SetTrusted(tmp, true);
        core::CompilerManager cm;
        core::BuildRunner runner;
        std::string berr;
        CHECK(runner.StartBuild(proj, cm, berr, "app"));
        while (runner.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!runner.GetLastResult().success) {
            for (const auto& l : runner.DrainOutput()) std::cerr << "  [codegen build] " << l.text << "\n";
        }
        CHECK(runner.GetLastResult().success);
        CHECK(fs::exists(tmp / "build/gen/gen_val.inc"));
        CHECK(fs::exists(tmp / "build/gen/gen_src.cpp"));

        std::error_code ec;
        core::fs_util::RemoveAllLong(tmp, ec);
    }

    // 2) CMake Makefile CMakeFiles/<target>.dir/build.make custom command import
    {
        fs::path tmp = MakeTempDir("v2d_compdb_make_codegen");
        WriteText(tmp / "src/main.cpp", "int main() { return 0; }\n");
        WriteText(tmp / "schema.proto", "message M {}\n");

        nlohmann::json arr = nlohmann::json::array();
        arr.push_back({
            {"directory", (tmp / "build").generic_u8string()},
            {"file", (tmp / "src/main.cpp").generic_u8string()},
            {"output", "CMakeFiles/svc.dir/src/main.cpp.obj"},
            {"arguments", nlohmann::json::array({ "g++", "-c", "../src/main.cpp", "-o", "CMakeFiles/svc.dir/src/main.cpp.obj" })}
        });
        arr.push_back({
            {"directory", (tmp / "build").generic_u8string()},
            {"file", (tmp / "build/msg.pb.cc").generic_u8string()},
            {"output", "CMakeFiles/svc.dir/msg.pb.cc.obj"},
            {"arguments", nlohmann::json::array({ "g++", "-c", "msg.pb.cc", "-o", "CMakeFiles/svc.dir/msg.pb.cc.obj" })}
        });
        WriteText(tmp / "build/compile_commands.json", arr.dump(2));
        WriteText(tmp / "build/CMakeFiles/svc.dir/link.txt",
            "g++ CMakeFiles/svc.dir/src/main.cpp.obj CMakeFiles/svc.dir/msg.pb.cc.obj -o svc.exe\n");
        WriteText(tmp / "build/CMakeFiles/svc.dir/build.make",
            "CMAKE_COMMAND = cmake\n"
            "msg.pb.cc: " + (tmp / "schema.proto").generic_u8string() + "\n"
            "\t@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue \"Generating msg.pb.cc\"\n"
            "\tpython gen_pb.py " + (tmp / "schema.proto").generic_u8string() + " msg.pb.cc\n");

        core::ImportPlan plan = core::CompDbManager::AnalyzeCompileCommands(tmp / "build/compile_commands.json", tmp);
        CHECK(plan.success);
        CHECK(plan.importedStepCount == 1);
        CHECK(plan.targets.size() == 1);
        CHECK(plan.targets[0].steps.size() == 1);
        CHECK(plan.targets[0].steps[0].generatesSources);
        CHECK(plan.targets[0].steps[0].command.find("python gen_pb.py") != std::string::npos);

        std::error_code ec;
        core::fs_util::RemoveAllLong(tmp, ec);
    }
}

int main() {
    std::cout << std::unitbuf;
    TestChildProcessStressAndCancel();
    TestCompileCommandsExportAndNoOpMtime();
    TestProgressSummaryAnd2MLineLog();
    TestLongPathsShortObjectsAndDefender();
    TestCompileCommandsImport();
    TestCompileCommandsCustomCodegenImport();
    std::cout << "ALL V2-D TESTS PASSED\n";
    return 0;
}
