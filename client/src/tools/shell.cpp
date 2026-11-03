#include "tools/shell.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "platform/win.h"
#include "policy/paths.h"
#include "tools/files.h"
#include "tools/shell_classify.h"

#include <algorithm>
#include <filesystem>

namespace wb::tools {

using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;

namespace {

std::wstring env_var(const wchar_t* name) {
    const DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    v.resize(::GetEnvironmentVariableW(name, v.data(), n));
    return v;
}

// Makes PowerShell's own output predictable: no progress records, UTF-8 on the console streams.
constexpr char kPrelude[] = "$ProgressPreference='SilentlyContinue'; [Console]::OutputEncoding=[System.Text.Encoding]::UTF8; ";

std::string encoded_command(const std::string& command) {
    const std::wstring w = widen(kPrelude + command);
    return base64_encode(reinterpret_cast<const std::uint8_t*>(w.data()), w.size() * sizeof(wchar_t));
}

std::string format_result(const sandbox::ProcessResult& r, const sandbox::ProcessSpec& spec, const char* timeout_source) {
    std::string out;
    if (r.limit != sandbox::LimitHit::none) {
        out += sandbox::describe_limit(r.limit, spec.limits, sandbox::effective_wall(spec));
        if (r.limit == sandbox::LimitHit::time) out += std::string(" The limit came from ") + timeout_source + ".";
        out += " The process tree was terminated; the output captured so far follows.\n";
    } else {
        out += "exit code: " + std::to_string(r.exit_code) + "\n";
    }
    out += "--- stdout";
    if (r.out_truncated) out += " (truncated at " + std::to_string(spec.limits.output_kb) + " KB of " + std::to_string(r.out_bytes) + " bytes)";
    out += " ---\n" + r.out;
    if (!r.out.empty() && r.out.back() != '\n') out += "\n";
    if (!r.err.empty() || r.err_truncated) {
        out += "--- stderr";
        if (r.err_truncated) out += " (truncated at " + std::to_string(spec.limits.output_kb) + " KB of " + std::to_string(r.err_bytes) + " bytes)";
        out += " ---\n" + r.err;
        if (r.err.back() != '\n') out += "\n";
    }
    return out;
}

}  // namespace

std::optional<std::wstring> find_shell(const std::string& name) {
    if (name == "powershell") {
        std::wstring root = env_var(L"SystemRoot");
        if (root.empty()) root = L"C:\\Windows";
        const std::filesystem::path p = std::filesystem::path(root) / L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p.wstring();
        return std::nullopt;
    }
    if (name == "pwsh") {
        for (const wchar_t* var : {L"ProgramW6432", L"ProgramFiles"}) {
            const std::wstring pf = env_var(var);
            if (pf.empty()) continue;
            const std::filesystem::path p = std::filesystem::path(pf) / L"PowerShell" / L"7" / L"pwsh.exe";
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return p.wstring();
        }
        wchar_t buf[MAX_PATH];
        if (::SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr)) return std::wstring(buf);
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> shell_unavailable(const ShellToolEnv& env) {
    const auto p = env.policy ? env.policy() : nullptr;
    const std::string name = p ? p->string("tools.shell.executable", "powershell") : "powershell";
    const auto found = env.locate ? env.locate(name) : find_shell(name);
    if (!found) {
        return name == "pwsh" ? "PowerShell 7 (pwsh), which policy.tools.shell.executable selects, is not installed on this computer"
                              : "Windows PowerShell is not available on this computer";
    }
    return std::nullopt;
}

Risk shell_risk(const policy::EffectivePolicy* policy, std::string_view command, std::string_view quarantine_dir) {
    const auto extra = policy ? policy->strings("tools.shell.destructive_patterns") : std::vector<std::string>{};
    if (destructive_match(command, extra)) return Risk::destructive;
    // Managed Downloads: anything that names the quarantine (the folder, or the word) runs only with the user's approval.
    const std::string c = to_lower(command);
    std::string q = to_lower(quarantine_dir);
    if (c.find("quarantine") != std::string::npos) return Risk::destructive;
    if (!q.empty()) {
        if (c.find(q) != std::string::npos) return Risk::destructive;
        std::replace(q.begin(), q.end(), '\\', '/');
        if (c.find(q) != std::string::npos) return Risk::destructive;
    }
    return Risk::write_local;
}

ToolDef shell_tool(ShellToolEnv env) {
    auto e = std::make_shared<ShellToolEnv>(std::move(env));
    ToolDef t;
    t.id = "shell.run";
    t.description = "Run a PowerShell command and return its exit code, stdout and stderr. The command runs in a sandbox with "
                    "limits; working_dir defaults to the agent workspace; timeout_seconds defaults to 120 (at most 1800). "
                    "Prefer the file tools for reading and editing files.";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"command", {{"type", "string"}, {"description", "PowerShell command text."}}},
                      {"working_dir", {{"type", "string"}, {"description", "Working folder (absolute, or relative to the workspace)."}}},
                      {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", kShellMaxTimeoutS}}}}},
                    {"required", {"command"}}};
    t.risk_of = [e](const json& a) {
        const auto p = e->policy ? e->policy() : nullptr;
        return shell_risk(p.get(), jstr(a, "command"), e->quarantine_dir);
    };
    t.run = [e](ToolContext& ctx, const json& a) -> ToolOutput {
        const std::string command = jstr(a, "command");
        if (command.empty()) return ToolOutput::failure("command is required.");
        const auto p = e->policy ? e->policy() : nullptr;
        if (const auto why = shell_unavailable(*e)) return ToolOutput::failure("shell.run is not available: " + *why + ".");
        if (!ctx.skill.empty()) {
            // A Skill's scripts run only with the runtimes policy lists (client/skills Skill Scripts); the command
            // itself is PowerShell.
            const auto allowed = p ? p->strings("skills.script_runtimes") : std::vector<std::string>{"powershell"};
            auto needed = script_runtimes(command);
            if (std::find(needed.begin(), needed.end(), "powershell") == needed.end()) needed.insert(needed.begin(), "powershell");
            for (const auto& r : needed)
                if (std::find(allowed.begin(), allowed.end(), r) == allowed.end())
                    return ToolOutput::failure("The Skill " + ctx.skill + " cannot run this command: the runtime " + r +
                                               " is not in policy.skills.script_runtimes. Nothing was executed.");
        }
        const std::string name = p ? p->string("tools.shell.executable", "powershell") : "powershell";
        const auto exe = e->locate ? e->locate(name) : find_shell(name);
        const std::string dir_arg = absolute_path(jstr(a, "working_dir").empty() ? e->workspace : jstr(a, "working_dir"), e->workspace);
        auto dir = policy::canonicalize(dir_arg);
        if (!dir) return ToolOutput::failure("The working folder " + dir_arg + " cannot be used: " + dir.error().message);
        std::error_code ec;
        if (!std::filesystem::is_directory(widen(*dir), ec)) return ToolOutput::failure("The working folder " + *dir + " does not exist.");
        auto spec = sandbox::shell_process(p.get(), e->rules(), *dir);
        if (!spec) return ToolOutput::failure(spec.error().message);
        long long timeout_s = jint(a, "timeout_seconds", kShellDefaultTimeoutS);
        timeout_s = std::clamp<long long>(timeout_s, 1, kShellMaxTimeoutS);
        spec->executable = *exe;
        spec->arguments = L"-NoProfile -NonInteractive -EncodedCommand " + widen(encoded_command(command));
        spec->timeout = std::chrono::seconds(timeout_s);
        spec->run_id = ctx.run_id;
        spec->call_id = ctx.call_id;
        spec->tool = "shell.run";
        if (ctx.skill_no_network) spec->network = sandbox::Network::none;  // a Skill declaring network none
        if (!e->sandbox) return ToolOutput::failure("The tool sandbox is not running.");
        auto r = e->sandbox->run(*spec, [&ctx] { return ctx.cancelled(); });
        if (!r) return ToolOutput::failure("The command could not be started: " + r.error().message);
        // Which limit decided the time: the smaller one wins (client/tool-sandbox).
        const bool per_call = std::chrono::seconds(timeout_s) < std::chrono::minutes(spec->limits.wall_minutes);
        const char* source = per_call ? "the call's timeout_seconds" : "the policy limit policy.tools.shell.limits.wall_minutes";
        ToolOutput out;
        out.ok = r->limit == sandbox::LimitHit::none && r->exit_code == 0;
        out.content = format_result(*r, *spec, source);
        return out;
    };
    return t;
}

}  // namespace wb::tools
