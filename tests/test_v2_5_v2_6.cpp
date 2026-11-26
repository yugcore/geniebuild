#include "core/condition.h"
#include "core/build_steps.h"
#include "core/project.h"
#include "core/target.h"
#include "core/settings_resolver.h"
#include "core/source_rules.h"
#include "core/build_runner.h"
#include "core/compiler.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
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
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

static void TestConditions() {
    std::cout << "Running V2-5 Condition unit tests...\n";

    core::ConditionContext win_msvc;
    win_msvc.os = "windows";
    win_msvc.compilerFamily = "msvc";
    win_msvc.arch = "x64";
    win_msvc.config = "Debug";
    win_msvc.toolchain = "MSVC 2022 x64";

    core::ConditionContext lin_gcc;
    lin_gcc.os = "linux";
    lin_gcc.compilerFamily = "gcc";
    lin_gcc.arch = "arm64";
    lin_gcc.config = "Shipping";
    lin_gcc.toolchain = "g++";

    // Basic identifiers & empty
    CHECK(core::EvaluateCondition("", win_msvc).active);
    CHECK(core::EvaluateCondition("   ", win_msvc).active);
    CHECK(core::EvaluateCondition("windows", win_msvc).active);
    CHECK(!core::EvaluateCondition("linux", win_msvc).active);
    CHECK(core::EvaluateCondition("msvc", win_msvc).active);
    CHECK(core::EvaluateCondition("x64", win_msvc).active);
    CHECK(core::EvaluateCondition("config:Debug", win_msvc).active);
    CHECK(core::EvaluateCondition("config:debug", win_msvc).active); // case-insensitive
    CHECK(!core::EvaluateCondition("config:Shipping", win_msvc).active);
    CHECK(core::EvaluateCondition("toolchain:\"MSVC 2022 x64\"", win_msvc).active);
    CHECK(core::EvaluateCondition("toolchain:g++", lin_gcc).active);
    CHECK(core::EvaluateCondition("arm64 && linux && gcc && config:Shipping", lin_gcc).active);

    // Complex expression from spec
    std::string spec_expr = "windows && (msvc || clangcl) && !config:Shipping";
    CHECK(core::EvaluateCondition(spec_expr, win_msvc).active);
    CHECK(!core::EvaluateCondition(spec_expr, lin_gcc).active);

    win_msvc.config = "Shipping";
    CHECK(!core::EvaluateCondition(spec_expr, win_msvc).active);
    win_msvc.config = "Debug";

    // Operator precedence: && binds tighter than ||
    // true || false && false -> true || (false && false) -> true
    CHECK(core::EvaluateCondition("windows || linux && gcc", win_msvc).active);
    // (windows || linux) && gcc -> false for msvc
    CHECK(!core::EvaluateCondition("(windows || linux) && gcc", win_msvc).active);
    // Double negation
    CHECK(core::EvaluateCondition("!!windows", win_msvc).active);

    // Inactive reason formatting
    auto inactive_res = core::EvaluateCondition("linux", win_msvc);
    CHECK(inactive_res.valid);
    CHECK(!inactive_res.active);
    CHECK(inactive_res.reason.find("linux") != std::string::npos);
    CHECK(inactive_res.reason.find("inactive") != std::string::npos);

    // Unknown identifier with suggestion
    std::string err;
    CHECK(!core::ValidateCondition("winows", err));
    CHECK(err.find("windows") != std::string::npos);

    // Syntax errors
    CHECK(!core::ValidateCondition("windows & msvc", err));
    CHECK(err.find("&&") != std::string::npos);
    CHECK(!core::ValidateCondition("windows | msvc", err));
    CHECK(err.find("||") != std::string::npos);
    CHECK(!core::ValidateCondition("(windows && msvc", err));
    CHECK(!core::ValidateCondition("windows)", err));
    CHECK(!core::ValidateCondition("()", err));
    CHECK(!core::ValidateCondition("config:", err));
    CHECK(!core::ValidateCondition("toolchain:", err));
    CHECK(!core::ValidateCondition("windows &&", err));
}

static void TestTargetAndLibConditions() {
    std::cout << "Running V2-5 Target/Lib/Override condition tests...\n";

    core::Target t_core;
    t_core.name = "core";
    t_core.kind = core::TargetKind::Static;

    core::Target t_metal;
    t_metal.name = "metal_backend";
    t_metal.kind = core::TargetKind::Static;
    t_metal.condition = "macos";

    core::Target t_editor;
    t_editor.name = "editor";
    t_editor.kind = core::TargetKind::Executable;
    t_editor.deps = { "core" };

    core::ConditionContext win_ctx{ "windows", "gcc", "x64", "Debug", "g++" };
    core::ConditionContext mac_ctx{ "macos", "clang", "arm64", "Debug", "clang++" };

    std::vector<const core::Target*> order;
    std::string err;

    // Inactive target excluded when building all
    CHECK(core::ResolveTargetBuildOrder({ t_core, t_metal, t_editor }, "", order, err, &win_ctx));
    CHECK_EQ(order.size(), static_cast<size_t>(2));

    // Active on macos
    CHECK(core::ResolveTargetBuildOrder({ t_core, t_metal, t_editor }, "", order, err, &mac_ctx));
    CHECK_EQ(order.size(), static_cast<size_t>(3));

    // Active target depending on inactive target -> error!
    t_editor.deps = { "core", "metal_backend" };
    CHECK(!core::ResolveTargetBuildOrder({ t_core, t_metal, t_editor }, "", order, err, &win_ctx));
    CHECK(err.find("metal_backend") != std::string::npos);
    CHECK(err.find("inactive") != std::string::npos);

    // Invalid condition blocks ResolveTargetBuildOrder
    t_editor.deps = { "core" };
    t_editor.condition = "winows";
    CHECK(!core::ResolveTargetBuildOrder({ t_core, t_metal, t_editor }, "", order, err, &win_ctx));
    CHECK(err.find("windows") != std::string::npos);

    // ValidateAllTargetConditions catches invalid conditions on steps & libs
    t_editor.condition.clear();
    t_editor.priv.libs = { "ws2_32" };
    t_editor.priv.libConditions["ws2_32"] = "bad_ident";
    CHECK(!core::ValidateAllTargetConditions({ t_core, t_editor }, err));
    CHECK(err.find("ws2_32") != std::string::npos);
}

static void TestStepsPlaceholdersAndIncrementality() {
    std::cout << "Running V2-6 Step placeholder, depfile, and incremental tests...\n";

    fs::path tmp = fs::temp_directory_path() / "geniebuild_unit_test_v26";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "shaders", ec);
    fs::create_directories(tmp / "proto", ec);
    WriteText(tmp / "shaders" / "basic.frag", "// frag\n");
    WriteText(tmp / "shaders" / "common.glsl", "// helper\n");
    WriteText(tmp / "proto" / "msg.proto", "message M {}\n");
    WriteText(tmp / "src" / "main.cpp", "int main() { return 0; }\n");

    core::Project proj;
    proj.Load(tmp);
    while (proj.IsScanning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        proj.Update();
    }
    proj.Update();

    core::Target tgt;
    tgt.name = "renderer";
    tgt.kind = core::TargetKind::Executable;
    tgt.files = { "src/main.cpp" };
    tgt.priv.libs = { "common", "ws2_32", "pthread" };
    tgt.priv.libConditions["ws2_32"] = "windows";
    tgt.priv.libConditions["pthread"] = "linux";

    core::BuildStep s_shaders;
    s_shaders.name = "shaders";
    s_shaders.phase = core::StepPhase::PreBuild;
    s_shaders.foreachPattern = "shaders/**/*.{vert,frag,comp}";
    s_shaders.command = "glslc -MD -MF {out}.d {in} -o {out}";
    s_shaders.outputs = { "{bindir}/shaders/{name}.spv" };
    s_shaders.depfile = "{out}.d";

    core::BuildStep s_proto;
    s_proto.name = "protobuf";
    s_proto.phase = core::StepPhase::PreBuild;
    s_proto.foreachPattern = "proto/*.proto";
    s_proto.command = "protoc --cpp_out={gen} {in}";
    s_proto.outputs = { "{gen}/{stem}.pb.cc", "{gen}/{stem}.pb.h" };
    s_proto.generatesSources = true;

    core::BuildStep s_env;
    s_env.name = "envcheck";
    s_env.phase = core::StepPhase::PostBuild;
    s_env.command = "echo ${config} ${target} {bin:renderer}";
    s_env.outputs = { "{gen}/stamp.txt" };

    tgt.steps = { s_shaders, s_proto, s_env };
    std::string err;
    proj.RemoveTarget(proj.GetTargets()[0].name, err); // won't remove last, update instead:
    core::Target updated = proj.GetTargets()[0];
    updated.name = "renderer";
    proj.RenameTarget(proj.GetTargets()[0].name, "renderer", err);
    tgt.name = "renderer";
    proj.UpdateTarget(tgt);

    // Save & reload roundtrip check
    CHECK(proj.SaveImmediate());
    core::Project reloaded;
    CHECK(reloaded.Load(tmp));
    const core::Target* rt = reloaded.FindTarget("renderer");
    CHECK(rt != nullptr);
    CHECK_EQ(rt->steps.size(), static_cast<size_t>(3));
    CHECK_EQ(rt->steps[0].name, "shaders");
    CHECK_EQ(rt->steps[0].depfile, "{out}.d");
    CHECK(rt->steps[1].generatesSources);
    CHECK_EQ(rt->priv.libConditions.at("ws2_32"), "windows");

    // Conditional libs & {gen} include dir propagation check
    std::unordered_map<std::string, const core::Target*> tmap;
    tmap["renderer"] = rt;
    auto eff = core::ResolveEffectiveTargetSettings(reloaded, *rt, tmap, "Debug", false);
#if defined(_WIN32)
    CHECK(std::find(eff.libs.begin(), eff.libs.end(), "ws2_32") != eff.libs.end());
    CHECK(std::find(eff.libs.begin(), eff.libs.end(), "pthread") == eff.libs.end());
#endif
    CHECK(std::find(eff.includeDirs.begin(), eff.includeDirs.end(), ".geniebuild/gen/Debug/renderer") != eff.includeDirs.end());

    // Placeholder expansion check
    std::unordered_map<std::string, std::string> bin_map;
    bin_map["renderer"] = ".geniebuild/out/Debug/bin/renderer.exe";
    auto jobs_sh = core::ExpandStepJobs(reloaded, *rt, 0, rt->steps[0], reloaded.GetDirectoryCache(), bin_map);
    CHECK_EQ(jobs_sh.size(), static_cast<size_t>(1));
    CHECK_EQ(jobs_sh[0].label, "basic.frag");
    CHECK_EQ(jobs_sh[0].outputs[0], ".geniebuild/out/Debug/bin/shaders/basic.frag.spv");
    CHECK_EQ(jobs_sh[0].depfile, ".geniebuild/out/Debug/bin/shaders/basic.frag.spv.d");
    CHECK(jobs_sh[0].cmd.display_command.find("shaders/basic.frag") != std::string::npos);

    auto jobs_pb = core::ExpandStepJobs(reloaded, *rt, 1, rt->steps[1], reloaded.GetDirectoryCache(), bin_map);
    CHECK_EQ(jobs_pb.size(), static_cast<size_t>(1));
    CHECK_EQ(jobs_pb[0].outputs[0], ".geniebuild/gen/Debug/renderer/msg.pb.cc");
    CHECK_EQ(jobs_pb[0].outputs[1], ".geniebuild/gen/Debug/renderer/msg.pb.h");

    auto jobs_env = core::ExpandStepJobs(reloaded, *rt, 2, rt->steps[2], reloaded.GetDirectoryCache(), bin_map);
    CHECK_EQ(jobs_env.size(), static_cast<size_t>(1));
    CHECK(jobs_env[0].cmd.display_command.find("Debug renderer .geniebuild/out/Debug/bin/renderer.exe") != std::string::npos);
    CHECK_EQ(jobs_env[0].referencedTargets.size(), static_cast<size_t>(1));
    CHECK_EQ(jobs_env[0].referencedTargets[0], "renderer");

    // Incremental up-to-date & depfile test
    std::unordered_map<std::string, std::string> state;
    std::string reason;
    CHECK(core::CheckStepJobNeedsRun(reloaded, jobs_sh[0], state, reason));
    state[jobs_sh[0].stateKey] = jobs_sh[0].cmdHash;
    // Output still missing -> must run
    CHECK(core::CheckStepJobNeedsRun(reloaded, jobs_sh[0], state, reason));
    CHECK(reason.find("output missing") != std::string::npos);

    // Create output and depfile after input
    auto out_p = reloaded.ResolvePath(jobs_sh[0].outputs[0]);
    WriteText(out_p, "spv");
    auto dep_p = reloaded.ResolvePath(jobs_sh[0].depfile);
    WriteText(dep_p, out_p.generic_u8string() + ": shaders/basic.frag shaders/common.glsl\n");
    auto base_t = fs::last_write_time(out_p);
    fs::last_write_time(tmp / "shaders" / "basic.frag", base_t - std::chrono::seconds(2));
    fs::last_write_time(tmp / "shaders" / "common.glsl", base_t - std::chrono::seconds(2));
    CHECK(!core::CheckStepJobNeedsRun(reloaded, jobs_sh[0], state, reason));

    // Touch depfile dependency (shaders/common.glsl) -> must trigger rerun!
    WriteText(tmp / "shaders" / "common.glsl", "// helper modified\n");
    fs::last_write_time(tmp / "shaders" / "common.glsl", base_t + std::chrono::seconds(2));
    bool dep_needs_run = core::CheckStepJobNeedsRun(reloaded, jobs_sh[0], state, reason);
    if (!dep_needs_run) {
        std::cerr << "DEBUG dep_p=" << dep_p << " reason=" << reason << "\n";
    }
    CHECK(dep_needs_run);
    CHECK(reason.find("depfile") != std::string::npos);

    // TrustStore check
    core::TrustStore::SetTrusted(tmp, false);
    CHECK(!core::TrustStore::IsTrusted(tmp));
    core::TrustStore::SetTrusted(tmp, true);
    CHECK(core::TrustStore::IsTrusted(tmp));
    core::TrustStore::SetTrusted(tmp, false);

    fs::remove_all(tmp, ec);
}

static void TestEndToEndBuildStepsAndConditions() {
    std::cout << "Running E2E BuildRunner tests for V2-5 and V2-6...\n";

    core::CompilerManager cm;
    if (!cm.HasCompilers()) {
        std::cout << "Skipping E2E build test (no compiler on PATH)\n";
        return;
    }

    fs::path tmp = fs::temp_directory_path() / "geniebuild_e2e_v25_v26";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "src", ec);
    fs::create_directories(tmp / "gen_inputs", ec);
    fs::create_directories(tmp / "assets", ec);

    // Platform-specific source files
    WriteText(tmp / "src" / "plat_win.cpp", "int plat_val() { return 42; }\n");
    WriteText(tmp / "src" / "plat_linux.cpp", "#error linux file must not compile on windows!\n");
    WriteText(tmp / "src" / "main.cpp",
              "#include <iostream>\n"
              "#include \"gen_val.h\"\n"
              "int plat_val();\n"
              "int gen_extra();\n"
              "int main() { std::cout << (plat_val() + gen_extra() + GEN_HDR_VAL) << \"\\n\"; return 0; }\n");

    WriteText(tmp / "gen_inputs" / "a.txt", "10\n");
    WriteText(tmp / "assets" / "data.txt", "asset1\n");

    // Write project.json directly with V2-5 conditions and V2-6 preBuild + postBuild steps
    // Use standard cmd / python or C++? Wait: let's check if python or cmake/sh/cmd exists;
    // Better: use `cmd /c` on Windows with `shell: true` (or tiny C++/echo/powershell? Wait: `cmd /c echo ... > ...` works universally on Windows without Python!).
#if defined(_WIN32)
    std::string gen_cmd = "cmd /c \"echo #define GEN_HDR_VAL 8 > {gen}/gen_val.h && echo int gen_extra() { return 5; } > {gen}/gen_extra.cpp\"";
    std::string post_cmd = "cmd /c \"copy /y {in} {out} > nul\"";
#else
    std::string gen_cmd = "sh -c \"echo '#define GEN_HDR_VAL 8' > {gen}/gen_val.h && echo 'int gen_extra() { return 5; }' > {gen}/gen_extra.cpp\"";
    std::string post_cmd = "cp {in} {out}";
#endif

    // Note: use shell: true for compound commands and test missing tool guard separately!
    WriteText(tmp / ".geniebuild" / "project.json",
              "{\n"
              "  \"version\": 2,\n"
              "  \"compiler\": \"g++\",\n"
              "  \"cCompiler\": \"gcc\",\n"
              "  \"configuration\": \"Debug\",\n"
              "  \"targets\": [\n"
              "    {\n"
              "      \"name\": \"linux_only\",\n"
              "      \"kind\": \"static\",\n"
              "      \"if\": \"linux\",\n"
              "      \"sources\": [ { \"include\": \"src/plat_linux.cpp\" } ]\n"
              "    },\n"
              "    {\n"
              "      \"name\": \"app\",\n"
              "      \"kind\": \"executable\",\n"
              "      \"sources\": [\n"
              "        { \"include\": \"src/main.cpp\" },\n"
              "        { \"include\": \"src/plat_win.cpp\", \"if\": \"windows\" },\n"
              "        { \"include\": \"src/plat_linux.cpp\", \"if\": \"linux\" }\n"
              "      ],\n"
              "      \"steps\": [\n"
              "        {\n"
              "          \"name\": \"codegen\",\n"
              "          \"phase\": \"preBuild\",\n"
              "          \"inputs\": [\"gen_inputs/a.txt\"],\n"
              "          \"command\": " + std::string("\"") +
#if defined(_WIN32)
              "echo #define GEN_HDR_VAL 8 > {gen}/gen_val.h && echo int gen_extra() { return 5; } > {gen}/gen_extra.cpp"
#else
              "echo '#define GEN_HDR_VAL 8' > {gen}/gen_val.h && echo 'int gen_extra() { return 5; }' > {gen}/gen_extra.cpp"
#endif
              + "\",\n"
              "          \"outputs\": [\"{gen}/gen_extra.cpp\", \"{gen}/gen_val.h\"],\n"
              "          \"generatesSources\": true,\n"
              "          \"shell\": true\n"
              "        },\n"
              "        {\n"
              "          \"name\": \"pack assets\",\n"
              "          \"phase\": \"postBuild\",\n"
              "          \"inputs\": [\"assets/data.txt\"],\n"
              "          \"command\": " + std::string("\"") +
#if defined(_WIN32)
              "type assets\\\\data.txt > {bindir}/data.pak"
#else
              "cp assets/data.txt {bindir}/data.pak"
#endif
              + "\",\n"
              "          \"outputs\": \"{bindir}/data.pak\",\n"
              "          \"shell\": true\n"
              "        }\n"
              "      ]\n"
              "    }\n"
              "  ]\n"
              "}\n");

    core::Project proj;
    CHECK(proj.Load(tmp));
    proj.Update();

    // 1. Untrusted project must be blocked before build!
    core::TrustStore::SetTrusted(tmp, false);
    core::BuildRunner runner;
    std::string start_err;
    CHECK(!runner.StartBuild(proj, cm, start_err));
    CHECK(start_err.find("not trusted") != std::string::npos);

    // 2. Trust folder and build!
    core::TrustStore::SetTrusted(tmp, true);
    CHECK(runner.StartBuild(proj, cm, start_err));
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto lines1 = runner.DrainOutput();
    if (!runner.GetLastResult().success) {
        for (const auto& l : lines1) std::cerr << "E2E1: " << l.text << "\n";
    }
    CHECK(runner.GetLastResult().success);
    bool saw_codegen = false, saw_pack = false;
    for (const auto& l : lines1) {
        if (l.text.find("[step codegen]") != std::string::npos) saw_codegen = true;
        if (l.text.find("[step pack assets]") != std::string::npos) saw_pack = true;
    }
    CHECK(saw_codegen);
    CHECK(saw_pack);
    CHECK(fs::exists(tmp / ".geniebuild" / "out" / "Debug" / "bin" / "data.pak"));

    // 3. Second build with no changes -> Up to date (0 steps, 0 compiles)
    CHECK(runner.StartBuild(proj, cm, start_err));
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto lines2 = runner.DrainOutput();
    CHECK(runner.GetLastResult().success);
    bool saw_up_to_date = false;
    for (const auto& l : lines2) {
        if (l.text.find("Up to date") != std::string::npos) saw_up_to_date = true;
    }
    CHECK(saw_up_to_date);

    // 4. Touch asset -> only postBuild runs, 0 compiles!
    auto pak_p = tmp / ".geniebuild" / "out" / "Debug" / "bin" / "data.pak";
    auto pak_t = fs::last_write_time(pak_p);
    WriteText(tmp / "assets" / "data.txt", "asset2\n");
    fs::last_write_time(tmp / "assets" / "data.txt", pak_t + std::chrono::seconds(2));
    CHECK(runner.StartBuild(proj, cm, start_err));
    while (runner.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto lines3 = runner.DrainOutput();
    CHECK(runner.GetLastResult().success);
    bool saw_pack3 = false, saw_codegen3 = false, saw_0_compiled = false;
    for (const auto& l : lines3) {
        if (l.text.find("[step pack assets]") != std::string::npos) saw_pack3 = true;
        if (l.text.find("[step codegen]") != std::string::npos) saw_codegen3 = true;
        if (l.text.find("0 compiled") != std::string::npos) saw_0_compiled = true;
    }
    CHECK(saw_pack3);
    CHECK(!saw_codegen3);
    CHECK(saw_0_compiled);

    // 5. Missing tool guard check
    core::Target app_copy = *proj.FindTarget("app");
    core::BuildStep missing_tool_step;
    missing_tool_step.name = "bad_tool";
    missing_tool_step.phase = core::StepPhase::PreBuild;
    missing_tool_step.command = "definitely_nonexistent_tool_xyz_12345 --flag";
    missing_tool_step.shell = false;
    app_copy.steps.push_back(missing_tool_step);
    proj.UpdateTarget(app_copy);

    CHECK(!runner.StartBuild(proj, cm, start_err));
    CHECK(start_err.find("bad_tool") != std::string::npos);
    CHECK(start_err.find("definitely_nonexistent_tool_xyz_12345") != std::string::npos);

    core::TrustStore::SetTrusted(tmp, false);
    fs::remove_all(tmp, ec);
}

int main() {
    TestConditions();
    TestTargetAndLibConditions();
    TestStepsPlaceholdersAndIncrementality();
    TestEndToEndBuildStepsAndConditions();

    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED!\n";
        return 0;
    } else {
        std::cerr << g_failures << " TEST(S) FAILED!\n";
        return 1;
    }
}
