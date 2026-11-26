#pragma once

#include <string>

namespace core {

class Project;
struct ToolchainProfile;

// Evaluation context derived from the active toolchain / compiler + configuration (V2-5)
struct ConditionContext {
    std::string os;             // "windows", "linux", "macos", "none"
    std::string compilerFamily; // "msvc", "gcc", "clang", "clangcl"
    std::string arch;           // "x64", "x86", "arm64", "arm"
    std::string config;         // active configuration name, e.g. "Debug", "Shipping"
    std::string toolchain;      // active toolchain / compiler name, e.g. "g++", "cl"
};

struct ConditionEvalResult {
    bool active = true;
    bool valid = true;
    std::string error;  // non-empty if expression has syntax or identifier errors
    std::string reason; // human-readable explanation when !active or !valid
};

// Builds a ConditionContext from a Project's active toolchain/compiler, configuration, and platform
ConditionContext BuildConditionContext(const Project& project, const ToolchainProfile* active_profile = nullptr);

// Parses (with thread-safe AST cache) and validates an "if" condition expression.
// Empty expressions are valid.
bool ValidateCondition(const std::string& expr, std::string& out_error);

// Parses (cached AST) and evaluates an "if" condition expression against ctx.
ConditionEvalResult EvaluateCondition(const std::string& expr, const ConditionContext& ctx);

} // namespace core
