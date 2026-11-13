// shell.run on Windows (add-core-tools-and-mcp task 2.1): Windows PowerShell through the tool-sandbox process API,
// the per-call timeout and the policy limits, the mode-off and unavailable-shell cases, complete results.
#include "platform/crypto.h"
#include "platform/win.h"
#include "policy/enforcer.h"
#include "policy/paths.h"
#include "sandbox/proxy.h"
#include "tools/shell.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace wb;
using namespace wb::tools;
using runtime::ToolContext;
using runtime::ToolOutput;

namespace {

struct ShellFixture {
    std::filesystem::path root = std::filesystem::path(local_appdata()) / L"skowork-test" / widen(random_urlsafe(6));
    std::filesystem::path data = root / L"data", ws = data / L"workspace";
    std::mutex mx;
    std::vector<std::string> denied;  // "<host> <rule>" of each refusal
    bool allow_egress = false;        // the policy allows every host
    sandbox::EgressProxy proxy{[this](const std::string&) {
                                   return allow_egress ? policy::RuleVerdict{true, "test", "allowed in this test"}
                                                       : policy::RuleVerdict{false, "test", "no network in tests"};
                               },
                               [this](const sandbox::EgressProxy::Owner&, const std::string& host, const policy::RuleVerdict& v) {
                                   const std::lock_guard lock(mx);
                                   denied.push_back(host + " " + v.rule);
                               }};
    sandbox::Sandbox box{&proxy};
    std::shared_ptr<const policy::EffectivePolicy> current;
    ShellToolEnv env;

    explicit ShellFixture(json shell = {{"mode", "sandboxed"}}) {
        std::filesystem::create_directories(ws);
        set_policy(std::move(shell));
        env.policy = [this] { return current; };
        env.rules = [this] { return policy::path_rules(current.get(), narrow(ws.wstring()), narrow(data.wstring()), {}); };
        env.workspace = *policy::canonicalize(narrow(ws.wstring()));
        env.sandbox = &box;
    }
    ~ShellFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void set_policy(json shell) {
        policy::PolicyBundle b;
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = {{"tools", {{"shell", std::move(shell)}}}};
        current = std::make_shared<policy::EffectivePolicy>(b);
    }
    ToolOutput run(const json& args) {
        ToolContext ctx;
        ctx.run_id = "run-1";
        ctx.call_id = "call-1";
        return shell_tool(env).run(ctx, args);
    }
    // A call from the subagent of the Skill `skill`.
    ToolOutput run_as_skill(const std::string& skill, bool no_network, const json& args) {
        ToolContext ctx;
        ctx.run_id = "run-1";
        ctx.call_id = "call-1";
        ctx.skill = skill;
        ctx.skill_no_network = no_network;
        return shell_tool(env).run(ctx, args);
    }
    void set_policy_with(json shell, json skills) {
        policy::PolicyBundle b;
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = {{"tools", {{"shell", std::move(shell)}}}, {"skills", std::move(skills)}};
        current = std::make_shared<policy::EffectivePolicy>(b);
    }
};

}  // namespace

TEST(ShellTool, RunsPowerShellInTheWorkspaceAndReturnsExitCodeAndStreams) {
    ShellFixture f;
    auto r = f.run({{"command", "Write-Output \"hello $((Get-Location).Path)\"; [Console]::Error.WriteLine('warn'); exit 3"}});
    EXPECT_FALSE(r.ok) << "a non-zero exit code is a failed call";
    EXPECT_NE(r.content.find("exit code: 3"), std::string::npos) << r.content;
    EXPECT_NE(r.content.find("hello " + f.env.workspace), std::string::npos) << r.content;
    EXPECT_NE(r.content.find("--- stderr ---\nwarn"), std::string::npos) << r.content;
    auto ok = f.run({{"command", "'caf\u00e9 ' + (1+1)"}});
    ASSERT_TRUE(ok.ok) << ok.content;
    EXPECT_NE(ok.content.find("caf\xC3\xA9 2"), std::string::npos) << ok.content;
}

TEST(ShellTool, PerCallTimeoutStopsTheTreeAndKeepsOutput) {
    ShellFixture f;
    const auto start = std::chrono::steady_clock::now();
    auto r = f.run({{"command", "Write-Output 'before'; Start-Process powershell -ArgumentList '-Command','Start-Sleep 60' -NoNewWindow; Start-Sleep 60"},
                    {"timeout_seconds", 3}});
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(r.ok);
    EXPECT_LT(took, std::chrono::seconds(20));
    EXPECT_NE(r.content.find("time limit of 3 seconds"), std::string::npos) << r.content;
    EXPECT_NE(r.content.find("timeout_seconds"), std::string::npos) << r.content;
    EXPECT_NE(r.content.find("before"), std::string::npos) << r.content;
}

TEST(ShellTool, PolicyLimitsApply) {
    ShellFixture f({{"mode", "sandboxed"}, {"limits", {{"memory_mb", 256}}}});
    auto r = f.run({{"command", "$a = New-Object byte[] (1GB); $a.Length"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.content.find("memory limit of 256 MB") != std::string::npos || r.content.find("OutOfMemory") != std::string::npos ||
                r.content.find("System.OutOfMemoryException") != std::string::npos)
        << r.content;
    // A per-call timeout above the policy limit does not raise it.
    f.set_policy({{"mode", "sandboxed"}, {"limits", {{"wall_minutes", 1}}}});
    auto spec_check = f.run({{"command", "exit 0"}, {"timeout_seconds", 1800}});
    EXPECT_TRUE(spec_check.ok) << spec_check.content;
}

TEST(ShellTool, ModeOffAndAMissingShellKeepTheToolAway) {
    ShellFixture f({{"mode", "off"}});
    auto off = f.run({{"command", "Write-Output 1"}});
    EXPECT_FALSE(off.ok);
    EXPECT_NE(off.content.find("switched off"), std::string::npos) << off.content;

    policy::EnforcerEnv ee;
    ee.policy = [&f] { return f.current; };
    ee.workspace = narrow(f.ws.wstring());
    ee.data_root = narrow(f.data.wstring());
    ee.unavailable = [&f](const std::string& id) -> std::optional<std::string> {
        return id == "shell.run" ? shell_unavailable(f.env) : std::nullopt;
    };
    policy::PolicyEnforcer enforcer(ee);
    EXPECT_FALSE(enforcer.tool_permitted("shell.run", runtime::Risk::destructive)) << "mode off hides shell.run";

    f.set_policy({{"mode", "sandboxed"}, {"executable", "pwsh"}});
    f.env.locate = [](const std::string& name) -> std::optional<std::wstring> {
        return name == "pwsh" ? std::nullopt : find_shell(name);
    };
    const auto why = shell_unavailable(f.env);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("pwsh"), std::string::npos);
    EXPECT_FALSE(enforcer.tool_permitted("shell.run", runtime::Risk::destructive)) << "a missing pwsh hides shell.run";
    runtime::ToolCallInfo call;
    call.tool = "shell.run";
    call.risk = runtime::Risk::write_local;
    call.risk_declared = true;
    EXPECT_EQ(enforcer.evaluate(call).decision, policy::Decision::deny);
    auto missing = f.run({{"command", "Write-Output 1"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.content.find("not installed"), std::string::npos) << missing.content;

    f.set_policy({{"mode", "sandboxed"}});
    EXPECT_FALSE(shell_unavailable(f.env).has_value()) << "Windows PowerShell 5.1 is present on Windows 11";
}

TEST(ShellTool, DeclaresDestructiveForMatchingCommands) {
    ShellFixture f({{"mode", "sandboxed"}, {"destructive_patterns", {"\\bgit\\s+push\\s+--force\\b"}}});
    const auto tool = shell_tool(f.env);
    EXPECT_EQ(runtime::effective_risk(tool, {{"command", "Get-ChildItem"}}), runtime::Risk::write_local);
    EXPECT_EQ(runtime::effective_risk(tool, {{"command", "Remove-Item C:\\x -Recurse -Force"}}), runtime::Risk::destructive);
    EXPECT_EQ(runtime::effective_risk(tool, {{"command", "git push --force origin main"}}), runtime::Risk::destructive);
}

TEST(ShellTool, RunningAQuarantinedDownloadNeedsApproval) {
    ShellFixture f;
    f.env.quarantine_dir = narrow((f.data / L"quarantine").wstring());
    const auto tool = shell_tool(f.env);
    const std::string file = f.env.quarantine_dir + "\\20261023-101500-setup.exe";
    std::string forward = file;
    std::replace(forward.begin(), forward.end(), '\\', '/');
    for (const std::string& c : {"& '" + file + "' /S", "Start-Process \"" + forward + "\"", "Copy-Item '" + file + "' .\\setup.exe",
                                 std::string("Get-ChildItem $env:LOCALAPPDATA\\Skowork\\Quarantine")})
        EXPECT_EQ(runtime::effective_risk(tool, {{"command", c}}), runtime::Risk::destructive) << c;
    EXPECT_EQ(runtime::effective_risk(tool, {{"command", "& .\\build\\app.exe"}}), runtime::Risk::write_local);

    // Destructive calls are denied by default; where policy lets them through it is with the user's approval.
    policy::EnforcerEnv ee;
    ee.policy = [&f] { return f.current; };
    ee.workspace = narrow(f.ws.wstring());
    ee.data_root = narrow(f.data.wstring());
    policy::PolicyEnforcer enforcer(ee);
    runtime::ToolCallInfo call;
    call.tool = "shell.run";
    call.args = {{"command", "& '" + file + "'"}};
    call.risk = runtime::effective_risk(tool, call.args);
    call.risk_declared = true;
    EXPECT_EQ(enforcer.evaluate(call).decision, policy::Decision::deny);
    policy::PolicyBundle b;
    b.ver = 2;
    b.exp = 4102444800;
    b.policy = {{"tools", {{"shell", {{"mode", "sandboxed"}}}, {"risk", {{"destructive", "approve"}}}}}};
    f.current = std::make_shared<policy::EffectivePolicy>(b);
    EXPECT_EQ(enforcer.evaluate(call).decision, policy::Decision::approve);
}

// add-skills 3.3: a Skill's scripts run through shell.run, sandboxed, with the runtimes policy lists.
TEST(ShellTool, SkillScriptsRunOnlyWithListedRuntimes) {
    ShellFixture f;
    const std::filesystem::path skill_dir = f.ws / L"weekly-report" / L"scripts";
    std::filesystem::create_directories(skill_dir);
    { std::ofstream(skill_dir / L"collect.ps1") << "Write-Output \"collected $($args[0])\"\n"; }
    { std::ofstream(skill_dir / L"collect.py") << "open('ran.txt', 'w').write('x')\n"; }
    const std::string ps1 = narrow((skill_dir / L"collect.ps1").wstring());
    auto ok = f.run_as_skill("weekly-report", false, {{"command", "& '" + ps1 + "' 43"}});
    ASSERT_TRUE(ok.ok) << ok.content;
    EXPECT_NE(ok.content.find("collected 43"), std::string::npos) << ok.content;

    // Python is not in the default [powershell]: refused before anything starts.
    const std::string py = narrow((skill_dir / L"collect.py").wstring());
    for (const std::string& c : {"python '" + py + "'", "$r = py -3 '" + py + "'; $r", "Get-Date; & \"" + py + "\"",
                                 std::string("Start-Process -FilePath python.exe -ArgumentList x.py -Wait")}) {
        auto refused = f.run_as_skill("weekly-report", false, {{"command", c}});
        EXPECT_FALSE(refused.ok) << c;
        EXPECT_NE(refused.content.find("the runtime python is not in policy.skills.script_runtimes"), std::string::npos) << refused.content;
        EXPECT_NE(refused.content.find("Nothing was executed"), std::string::npos);
    }
    EXPECT_FALSE(std::filesystem::exists(f.ws / L"ran.txt"));
    // The same command outside a Skill is not subject to the Skill runtimes.
    EXPECT_EQ(f.run({{"command", "python --version"}}).content.find("script_runtimes"), std::string::npos);

    // Policy that lists python lets it through to the shell (whether python is installed does not matter here).
    f.set_policy_with({{"mode", "sandboxed"}}, {{"script_runtimes", {"powershell", "python"}}});
    EXPECT_EQ(f.run_as_skill("weekly-report", false, {{"command", "python '" + py + "'"}}).content.find("script_runtimes"), std::string::npos);
    // Policy without powershell refuses every Skill command: the command itself is PowerShell.
    f.set_policy_with({{"mode", "sandboxed"}}, {{"script_runtimes", {"python"}}});
    auto none = f.run_as_skill("weekly-report", false, {{"command", "Get-Date"}});
    EXPECT_FALSE(none.ok);
    EXPECT_NE(none.content.find("the runtime powershell"), std::string::npos) << none.content;
}

// add-skills 2.4: a Skill declaring network none runs its scripts in no-network mode; the proxy refuses even a host
// that policy allows.
TEST(ShellTool, NetworkNoneSkillScriptsAreRefusedByTheProxy) {
    ShellFixture f;
    f.allow_egress = true;
    const std::string connect =
        "$u = [Uri]$env:HTTPS_PROXY; $c = New-Object Net.Sockets.TcpClient($u.Host, $u.Port); $s = $c.GetStream(); "
        "$a = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes([Uri]::UnescapeDataString($u.UserInfo))); "
        "$b = [Text.Encoding]::ASCII.GetBytes(\"CONNECT allowed.test:443 HTTP/1.1`r`nHost: allowed.test:443`r`nProxy-Authorization: Basic $a`r`n`r`n\"); "
        "$s.Write($b, 0, $b.Length); $buf = New-Object byte[] 512; $n = $s.Read($buf, 0, 512); "
        "'status: ' + [Text.Encoding]::ASCII.GetString($buf, 0, $n).Split(\"`r\")[0]";
    auto none = f.run_as_skill("offline-report", true, {{"command", connect}});
    EXPECT_NE(none.content.find("status: HTTP/1.1 403"), std::string::npos) << none.content;
    {
        const std::lock_guard lock(f.mx);
        ASSERT_EQ(f.denied.size(), 1u);
        EXPECT_EQ(f.denied[0], "allowed.test no-network");
        f.denied.clear();
    }
    // The default network (proxy) follows policy: the host is allowed, so the proxy does not refuse it.
    auto proxy = f.run_as_skill("online-report", false, {{"command", connect}});
    EXPECT_EQ(proxy.content.find(" 403"), std::string::npos) << proxy.content;
    const std::lock_guard lock(f.mx);
    EXPECT_TRUE(f.denied.empty());
}

TEST(ShellTool, CompleteOutputReachesTheRuntime) {
    ShellFixture f;
    // 1 MB of stdout: the default capture limit of the sandbox; nothing is cut by the tool itself.
    auto r = f.run({{"command", "[Console]::Out.Write('x' * 1048000)"}});
    ASSERT_TRUE(r.ok) << r.content.substr(0, 300);
    EXPECT_GE(r.content.size(), 1048000u);
    EXPECT_EQ(r.content.find("truncated"), std::string::npos);
}
