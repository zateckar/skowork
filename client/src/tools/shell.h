// client/core-tools shell.run (add-core-tools-and-mcp design D1): a command in Windows PowerShell 5.1 or pwsh, run
// through the client/tool-sandbox process API, which owns the shell mode, the limits and the timeout rule.
#pragma once

#include "policy/core.h"
#include "runtime/tools.h"
#include "sandbox/sandbox.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wb::tools {

struct ShellToolEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::function<policy::PathRules()> rules;
    std::string workspace;      // the default working folder
    sandbox::Sandbox* sandbox = nullptr;
    std::string quarantine_dir;  // commands that name it are destructive (Managed Downloads)
    // Finds a shell executable; tests replace it. Default: find_shell.
    std::function<std::optional<std::wstring>(const std::string& name)> locate;
};

// The full path of `powershell` (Windows PowerShell 5.1) or `pwsh`, if installed.
std::optional<std::wstring> find_shell(const std::string& name);

// Why shell.run cannot be offered right now (the selected shell is not installed), or nullopt.
std::optional<std::string> shell_unavailable(const ShellToolEnv& env);

// The Risk class of a command under the policy's destructive patterns; a command that names the quarantine folder
// (or the word "quarantine") is destructive too.
runtime::Risk shell_risk(const policy::EffectivePolicy* policy, std::string_view command, std::string_view quarantine_dir = {});

runtime::ToolDef shell_tool(ShellToolEnv env);

inline constexpr int kShellDefaultTimeoutS = 120;
inline constexpr int kShellMaxTimeoutS = 30 * 60;

}  // namespace wb::tools
