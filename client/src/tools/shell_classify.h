// Shell Risk Classification of client/core-tools: a shell.run command is destructive when it matches a destructive
// pattern. The built-in patterns are always applied; policy.tools.shell.destructive_patterns can only add to them.
// Patterns are ECMAScript regular expressions, matched case-insensitively anywhere in the command. Portable.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::tools {

// The built-in patterns: recursive deletion, disk formatting, registry deletion, changes to user or security settings,
// and nested encoded commands that cannot be inspected.
const std::vector<std::string>& default_destructive_patterns();

// The first pattern (built-in, then extra) that matches the command; nullopt when none does. An extra pattern that is
// not a valid regular expression is skipped and reported through `invalid`.
std::optional<std::string> destructive_match(std::string_view command, const std::vector<std::string>& extra,
                                             std::vector<std::string>* invalid = nullptr);

// The script runtimes a command starts (client/skills Skill Scripts; add-skills design D9): the programs in command
// position (the first word of each statement or pipeline element, after the & and . call operators, and the program of
// Start-Process), named by program (python.exe, node) or by script extension (.py, .ps1). Names: powershell, python,
// node, cmd, bash, ruby, perl, wsh. Programs that are not script runtimes (git, Get-ChildItem) are not reported.
std::vector<std::string> script_runtimes(std::string_view command);

}  // namespace wb::tools
