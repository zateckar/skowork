// client/mcp-client on Windows (add-core-tools-and-mcp tasks 3.1-3.5): the stdio transport against the official SDK's
// server (dev/mcp-test-server), Streamable HTTP with the user's identity-provider token, pinned packages, namespacing and
// filtering, catalog risk classes, untrusted provenance, the lifecycle and health.
#include "base/encoding.h"
#include "extract/ooxml_fixtures.h"
#include "mcp/manager.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "sandbox/proxy.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace wb;
using namespace wb::mcp;
using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path find_node() {
    wchar_t buf[MAX_PATH];
    if (::SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, buf, nullptr) == 0) return {};
    return buf;
}

fs::path reference_server() { return fs::path(WORKBENCH_MCP_TEST_SERVER_DIR) / "server.mjs"; }

bool reference_server_ready() {
    std::error_code ec;
    return !find_node().empty() && fs::exists(fs::path(WORKBENCH_MCP_TEST_SERVER_DIR) / "node_modules" / "@modelcontextprotocol" / "sdk", ec);
}

// A token as the identity provider issues it (decoded, not verified, by the test server).
std::string fake_jwt(const std::string& sub, long long exp = 4102444800LL) {
    const json header = {{"alg", "ES256"}, {"typ", "JWT"}};
    const json payload = {{"iss", "https://idp.test/realms/workbench"}, {"azp", "workbench"}, {"sub", sub}, {"exp", exp}};
    return base64url_encode(header.dump()) + "." + base64url_encode(payload.dump()) + ".c2ln";
}

// The registry side of the agent runtime.
struct Registry {
    std::mutex mx;
    std::map<std::string, ToolDef> tools;
    Status add(ToolDef d) {
        const std::lock_guard lock(mx);
        const std::string id = d.id;
        tools[id] = std::move(d);
        return {};
    }
    bool remove(const std::string& id) {
        const std::lock_guard lock(mx);
        return tools.erase(id) > 0;
    }
    bool has(const std::string& id) {
        const std::lock_guard lock(mx);
        return tools.count(id) > 0;
    }
    std::optional<ToolDef> get(const std::string& id) {
        const std::lock_guard lock(mx);
        const auto it = tools.find(id);
        if (it == tools.end()) return std::nullopt;
        return it->second;
    }
    size_t size() {
        const std::lock_guard lock(mx);
        return tools.size();
    }
};

struct McpFixture {
    fs::path root = fs::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
    sandbox::EgressProxy proxy{[](const std::string&) { return policy::RuleVerdict{false, "test", "no network in tests"}; },
                               [](const sandbox::EgressProxy::Owner&, const std::string&, const policy::RuleVerdict&) {}};
    sandbox::Sandbox box{&proxy};
    std::mutex pmx;
    std::shared_ptr<const policy::EffectivePolicy> current;
    policy::EgressGuard guard{[this] { return policy(); }};
    Registry reg;
    std::string package;  // the zip
    std::string sha;
    std::atomic<int> fetches{0};
    std::function<std::string()> serve = [this] { return package; };
    std::vector<std::pair<std::string, json>> events;
    std::vector<bool> token_requests;  // force flag of each user_token call
    std::function<std::string(bool force)> user_token = [](bool) { return fake_jwt("u1"); };
    std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();
    std::unique_ptr<McpManager> mgr;

    McpFixture() {
        fs::create_directories(root);
        const std::string exe = read_all(exe_dir() / L"mcp_test_server.exe");
        package = test::zip_of({{"bin/server.exe", exe}, {"README.txt", "test package"}});
        sha = hex_encode(sha256(package));
    }
    ~McpFixture() {
        mgr.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::shared_ptr<const policy::EffectivePolicy> policy() {
        const std::lock_guard lock(pmx);
        return current;
    }
    void set_policy(json servers, json extra = json::object()) {
        policy::PolicyBundle b;
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = {{"mcp", {{"servers", std::move(servers)}}}, {"network", {{"egress_allow", {"127.0.0.1"}}}}};
        b.policy.merge_patch(extra);
        const std::lock_guard lock(pmx);
        current = std::make_shared<policy::EffectivePolicy>(b);
    }
    json local_entry(json patch = json::object()) {
        json e = {{"id", "local-tool"}, {"name", "Local tool"}, {"kind", "local"}, {"spec_version", "2025-06-18"}, {"package", sha},
                  {"args", {"bin/server.exe"}}, {"env", {{"MODE", "ro"}}}, {"include_tools", json::array()},
                  {"exclude_tools", {"delete_issue"}}, {"risk", {{"echo", "read"}}}};
        e.merge_patch(patch);
        return e;
    }
    McpManager& start() {
        McpEnv env;
        env.policy = [this] { return policy(); };
        env.sandbox = &box;
        env.guard = &guard;
        env.data_dir = root / L"mcp";
        env.client_version = "0.0.0-test";
        env.fetch_package = [this](const std::string& s) -> Result<std::string> {
            ++fetches;
            if (s != sha) return make_error("not_found");
            return serve();
        };
        env.user_token = [this](bool force) -> Result<std::string> {
            token_requests.push_back(force);
            return user_token(force);
        };
        env.security_event = [this](const std::string& kind, const json& d) { events.emplace_back(kind, d); };
        env.add_tool = [this](ToolDef d) { return reg.add(std::move(d)); };
        env.remove_tool = [this](const std::string& id) { return reg.remove(id); };
        env.start_timeout = std::chrono::milliseconds(20000);
        env.call_timeout = std::chrono::milliseconds(20000);
        env.now = [this] { return clock; };
        mgr = std::make_unique<McpManager>(std::move(env));
        return *mgr;
    }
    void apply() {
        mgr->apply();
        ASSERT_TRUE(mgr->wait_idle(std::chrono::seconds(60)));
    }
    json server_status(const std::string& id) {
        for (const auto& s : mgr->status())
            if (s["id"] == id) return s;
        return nullptr;
    }
    runtime::ToolOutput call(const std::string& id, const json& args) {
        auto d = reg.get(id);
        if (!d) return runtime::ToolOutput::failure("no such tool " + id);
        ToolContext ctx;
        ctx.run_id = "run-1";
        ctx.call_id = "call-1";
        return d->run(ctx, args);
    }
};

// A process that is not sandboxed (the HTTP reference server); terminated on destruction.
struct ChildProcess {
    PROCESS_INFORMATION pi{};
    HANDLE out = nullptr;
    std::string first_line;
    ChildProcess(const fs::path& exe, const std::wstring& args) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE w = nullptr;
        ::CreatePipe(&out, &w, &sa, 0);
        ::SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = w;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        ::CloseHandle(w);
        char c = 0;
        DWORD n = 0;
        while (::ReadFile(out, &c, 1, &n, nullptr) && n == 1 && c != '\n') first_line.push_back(c);
    }
    ~ChildProcess() {
        if (pi.hProcess) {
            ::TerminateProcess(pi.hProcess, 0);
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(pi.hThread);
        }
        if (out) ::CloseHandle(out);
    }
};

}  // namespace

// ---- 3.1 stdio against the MCP reference server ----------------------------------------------------------------------

TEST(McpStdio, TalksToTheReferenceServerInTheSandbox) {
    if (!reference_server_ready()) GTEST_SKIP() << "node or dev/mcp-test-server/node_modules missing (npm install there)";
    McpFixture f;
    sandbox::ProcessSpec spec;
    spec.executable = find_node().wstring();
    spec.arguments = L"\"" + reference_server().wstring() + L"\" --stdio";
    spec.working_dir = narrow(fs::path(WORKBENCH_MCP_TEST_SERVER_DIR).wstring());
    spec.limits = sandbox::limits_for(nullptr, "mcp");
    spec.tool = "mcp:reference";
    auto proc = f.box.start(spec);
    ASSERT_TRUE(proc.ok()) << proc.error().message;
    auto session = Session::open(std::make_unique<StdioTransport>(std::move(*proc)), "2025-06-18", "test", std::chrono::seconds(30));
    ASSERT_TRUE(session.ok()) << session.error().message;
    EXPECT_EQ((*session)->info().name, "workbench-mock-jira");
    EXPECT_TRUE((*session)->info().has_tools);
    auto tools = (*session)->list_tools(std::chrono::seconds(10));
    ASSERT_TRUE(tools.ok()) << tools.error().message;
    EXPECT_EQ(tools->size(), 4u);
    auto r = (*session)->call_tool("search_issues", {{"query", "build"}}, std::chrono::seconds(10), {});
    ASSERT_TRUE(r.ok()) << r.error().message;
    const CallText c = call_result_text(*r);
    EXPECT_NE(c.text.find("DEMO-101"), std::string::npos);
    EXPECT_NE(c.text.find("DEMO-103"), std::string::npos);
    auto issue = (*session)->call_tool("get_issue", {{"key", "DEMO-102"}}, std::chrono::seconds(10), {});
    ASSERT_TRUE(issue.ok());
    EXPECT_EQ(call_result_text(*issue).item, "jira://issue/DEMO-102");
    auto bad = (*session)->call_tool("no_such_tool", json::object(), std::chrono::seconds(10), {});
    // The SDK reports an unknown tool either as a JSON-RPC error or as an isError result.
    EXPECT_TRUE(!bad.ok() || call_result_text(*bad).is_error);
}

// ---- 3.3 / 3.4 local package, namespacing, filtering, risk, provenance -----------------------------------------------

TEST(McpManager, LocalServerToolsAreNamespacedFilteredAndClassified) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    const json st = f.server_status("local-tool");
    ASSERT_FALSE(st.is_null());
    EXPECT_EQ(st["state"], "connected") << st.dump();
    EXPECT_EQ(st["tools"], 8);  // both tools/list pages
    EXPECT_EQ(st["usable_tools"], 7);
    // Scenario: Excluded tool.
    EXPECT_FALSE(f.reg.has("mcp:local-tool/delete_issue"));
    ASSERT_TRUE(f.reg.has("mcp:local-tool/echo"));
    EXPECT_EQ(f.reg.get("mcp:local-tool/echo")->risk, Risk::read);
    // Scenario: Server claims read-only (list_items has readOnlyHint, the catalog has no annotation).
    EXPECT_EQ(f.reg.get("mcp:local-tool/list_items")->risk, Risk::external);

    auto echo = f.call("mcp:local-tool/echo", {{"text", "hi"}, {"rationale", "testing"}});
    EXPECT_TRUE(echo.ok);
    EXPECT_EQ(echo.content, "echo: hi");
    // Scenario: MCP result provenance.
    auto page = f.call("mcp:local-tool/get_page", {{"page_id", "7"}});
    EXPECT_TRUE(page.ok);
    EXPECT_NE(page.content.find("Page 7 body"), std::string::npos);
    ASSERT_TRUE(page.provenance.has_value());
    EXPECT_EQ(page.provenance->source_kind, "mcp");
    EXPECT_EQ(page.provenance->source_id, "local-tool/get_page#page://7");
    EXPECT_FALSE(page.provenance->trusted);
    // The catalog environment reaches the server; the client's own environment does not; network goes to the proxy.
    ::SetEnvironmentVariableW(L"WORKBENCH_TEST_SECRET", L"x");
    auto env = f.call("mcp:local-tool/env", json::object());
    ::SetEnvironmentVariableW(L"WORKBENCH_TEST_SECRET", nullptr);
    EXPECT_EQ(env.content, "MODE=ro PROXY=yes SECRET=no");
}

TEST(McpManager, CachedToolListRegistersWithoutStartingTheServer) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    f.mgr.reset();
    f.reg.tools.clear();
    f.start();
    f.mgr->apply();  // no background discovery needed
    EXPECT_EQ(f.server_status("local-tool")["state"], "idle");
    EXPECT_TRUE(f.reg.has("mcp:local-tool/echo"));
    // Lazy start on first use.
    EXPECT_EQ(f.call("mcp:local-tool/echo", {{"text", "x"}}).content, "echo: x");
    EXPECT_EQ(f.server_status("local-tool")["state"], "connected");
}

TEST(McpManager, TamperedPackageIsRefused) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.serve = [&f] {
        std::string t = f.package;
        t[t.size() / 2] ^= 0x5a;
        return t;
    };
    f.start();
    f.apply();
    const json st = f.server_status("local-tool");
    EXPECT_EQ(st["state"], "unavailable");
    EXPECT_NE(st["error"].get<std::string>().find("SHA-256"), std::string::npos);
    EXPECT_EQ(f.reg.size(), 0u);
    ASSERT_EQ(f.events.size(), 1u);
    EXPECT_EQ(f.events[0].first, "mcp_package_mismatch");
    EXPECT_EQ(f.events[0].second["expected"], f.sha);
    EXPECT_FALSE(fs::exists(f.root / L"mcp" / L"packages" / widen(f.sha + ".zip")));
}

TEST(McpManager, TamperedStoredPackageIsFetchedAgain) {
    McpFixture f;
    fs::create_directories(f.root / L"mcp" / L"packages");
    std::ofstream(f.root / L"mcp" / L"packages" / widen(f.sha + ".zip"), std::ios::binary) << "tampered";
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    EXPECT_EQ(f.fetches.load(), 1);
    EXPECT_TRUE(f.events.empty());
    EXPECT_EQ(f.server_status("local-tool")["state"], "connected");
    EXPECT_EQ(read_all(f.root / L"mcp" / L"packages" / widen(f.sha + ".zip")), f.package);
}

// ---- 3.5 lifecycle and health -----------------------------------------------------------------------------------------

TEST(McpManager, CrashingServerIsRestartedThreeTimesThenFailed) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    for (int i = 1; i <= 3; ++i) {
        auto r = f.call("mcp:local-tool/crash", json::object());
        EXPECT_FALSE(r.ok);
        EXPECT_NE(r.content.find("crashing on purpose"), std::string::npos) << r.content;
        EXPECT_EQ(f.server_status("local-tool")["state"], "idle");
        EXPECT_EQ(f.call("mcp:local-tool/echo", {{"text", std::to_string(i)}}).content, "echo: " + std::to_string(i));  // restarted
    }
    // Scenario: Crashing server (the fourth crash within an hour).
    EXPECT_FALSE(f.call("mcp:local-tool/crash", json::object()).ok);
    const json st = f.server_status("local-tool");
    EXPECT_EQ(st["state"], "failed");
    EXPECT_EQ(st["usable_tools"], 0);
    EXPECT_EQ(f.reg.size(), 0u);
    EXPECT_TRUE(f.mgr->tool_unavailable("mcp:local-tool/echo").has_value());
    // A policy refresh does not bring a failed server's tools back.
    f.apply();
    EXPECT_EQ(f.reg.size(), 0u);
}

TEST(McpManager, CrashesOlderThanAnHourDoNotCount) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(f.call("mcp:local-tool/crash", json::object()).ok);
    f.clock += std::chrono::minutes(61);
    EXPECT_FALSE(f.call("mcp:local-tool/crash", json::object()).ok);
    EXPECT_EQ(f.server_status("local-tool")["state"], "idle");
}

TEST(McpManager, ServerWithNoUsableToolsIsAWarningWithTheReason) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry({{"exclude_tools", {"*"}}})}));
    f.start();
    f.apply();
    json st = f.server_status("local-tool");
    EXPECT_EQ(st["usable_tools"], 0);
    ASSERT_TRUE(st["warning"].is_string());
    EXPECT_NE(st["warning"].get<std::string>().find("no usable tools"), std::string::npos);
    EXPECT_NE(st["warning"].get<std::string>().find("excluded by the catalog (*)"), std::string::npos);

    // The same through policy.tools.rules.
    f.set_policy(json::array({f.local_entry()}), {{"tools", {{"rules", {{"mcp:local-tool/*", "deny"}}}}}});
    f.apply();
    f.mgr->apply();
    st = f.server_status("local-tool");
    EXPECT_EQ(st["usable_tools"], 0);
    EXPECT_NE(st["warning"].get<std::string>().find("denied by policy (tools.rules[\"mcp:local-tool/*\"]"), std::string::npos) << st.dump();
    EXPECT_EQ(f.reg.size(), 0u);
}

TEST(McpManager, IdleServerIsStoppedAndStartedAgainOnUse) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}), {{"mcp", {{"idle_timeout_minutes", 2}}}});
    f.start();
    f.apply();
    EXPECT_EQ(f.server_status("local-tool")["state"], "connected");
    f.clock += std::chrono::minutes(1);
    f.mgr->tick();
    EXPECT_EQ(f.server_status("local-tool")["state"], "connected");
    f.clock += std::chrono::minutes(2);
    f.mgr->tick();
    EXPECT_EQ(f.server_status("local-tool")["state"], "idle");
    EXPECT_EQ(f.call("mcp:local-tool/echo", {{"text", "back"}}).content, "echo: back");
    EXPECT_EQ(f.server_status("local-tool")["state"], "connected");
}

TEST(McpManager, ServerRemovedCentrallyIsStoppedAndItsToolsWithdrawn) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    ASSERT_TRUE(f.reg.has("mcp:local-tool/echo"));
    auto held = f.reg.get("mcp:local-tool/echo");  // a call that is already planned
    f.set_policy(json::array());
    f.mgr->apply();
    EXPECT_EQ(f.reg.size(), 0u);
    EXPECT_TRUE(f.mgr->status().empty());
    EXPECT_EQ(f.mgr->server_unavailable("local-tool"), "the MCP server local-tool is not in your catalog");
    ToolContext ctx;
    EXPECT_FALSE(held->run(ctx, {{"text", "late"}}).ok);
}

TEST(McpManager, UnlistedServerIsUnavailable) {
    // Scenario: Unlisted server (a skill's dependency that is not in the catalog).
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}));
    f.start();
    f.apply();
    EXPECT_FALSE(f.mgr->server_unavailable("local-tool").has_value());
    EXPECT_TRUE(f.mgr->server_unavailable("skill-declared").has_value());
    EXPECT_TRUE(f.mgr->tool_unavailable("mcp:skill-declared/run").has_value());
}

// ---- 3.2 Streamable HTTP with the user's identity-provider token ------------------------------------------------------

TEST(McpHttp, RemoteServerGetsTheUsersToken) {
#ifndef WORKBENCH_DEV_ENDPOINTS
  GTEST_SKIP() << "the test server listens on loopback http, which only debug and test builds accept";
#endif
    if (!reference_server_ready()) GTEST_SKIP() << "node or dev/mcp-test-server/node_modules missing (npm install there)";
    ChildProcess server(find_node(), L"\"" + reference_server().wstring() + L"\" --http 0 --issuer https://idp.test/realms/workbench");
    const auto j = parse_json(server.first_line);
    ASSERT_TRUE(j.has_value()) << server.first_line;
    const std::string url = jstr(*j, "listening");
    McpFixture f;
    f.set_policy(json::array({{{"id", "jira-corp"}, {"name", "Jira"}, {"kind", "remote"}, {"url", url}, {"auth", "user-token"},
                               {"exclude_tools", {"delete_issue"}}, {"risk", json::object()}}}));
    f.start();
    f.apply();
    const json st = f.server_status("jira-corp");
    EXPECT_EQ(st["state"], "connected") << st.dump();
    EXPECT_EQ(st["usable_tools"], 3);
    // Scenario: User token presented.
    auto who = f.call("mcp:jira-corp/whoami", json::object());
    EXPECT_EQ(who.content, "sub=u1");
    ASSERT_FALSE(f.token_requests.empty());
    for (const bool force : f.token_requests) EXPECT_FALSE(force);
    auto search = f.call("mcp:jira-corp/search_issues", {{"query", "DEMO-102"}});
    EXPECT_TRUE(search.ok);
    EXPECT_NE(search.content.find("onboarding"), std::string::npos);
    EXPECT_EQ(search.provenance->source_id, "jira-corp/search_issues");
    auto issue = f.call("mcp:jira-corp/get_issue", {{"key", "DEMO-101"}});
    EXPECT_EQ(issue.provenance->source_id, "jira-corp/get_issue#jira://issue/DEMO-101");
    EXPECT_EQ(f.reg.get("mcp:jira-corp/search_issues")->risk, Risk::external);
}

TEST(McpHttp, TokenIsRefreshedOnceAfter401) {
#ifndef WORKBENCH_DEV_ENDPOINTS
  GTEST_SKIP() << "the test server listens on loopback http, which only debug and test builds accept";
#endif
    if (!reference_server_ready()) GTEST_SKIP() << "node or dev/mcp-test-server/node_modules missing (npm install there)";
    ChildProcess server(find_node(), L"\"" + reference_server().wstring() + L"\" --http 0");
    const std::string url = jstr(*parse_json(server.first_line), "listening");
    McpFixture f;
    // Scenario: Token refreshed after 401. The cached token has expired; the refreshed one is accepted.
    f.user_token = [](bool force) { return force ? fake_jwt("u1") : fake_jwt("u1", 1000); };
    f.set_policy(json::array({{{"id", "jira-corp"}, {"kind", "remote"}, {"url", url}, {"auth", "user-token"}}}));
    f.start();
    f.apply();
    EXPECT_EQ(f.server_status("jira-corp")["state"], "connected") << f.server_status("jira-corp").dump();
    ASSERT_GE(f.token_requests.size(), 2u);
    EXPECT_FALSE(f.token_requests[0]);
    EXPECT_TRUE(f.token_requests[1]);
}

TEST(McpHttp, RefusedTokenAndBlockedEgressAreReported) {
#ifndef WORKBENCH_DEV_ENDPOINTS
  GTEST_SKIP() << "the test server listens on loopback http, which only debug and test builds accept";
#endif
    if (!reference_server_ready()) GTEST_SKIP() << "node or dev/mcp-test-server/node_modules missing (npm install there)";
    ChildProcess server(find_node(), L"\"" + reference_server().wstring() + L"\" --http 0");
    const std::string url = jstr(*parse_json(server.first_line), "listening");
    McpFixture f;
    f.user_token = [](bool) { return fake_jwt("u1", 1000); };  // refused even after the refresh
    f.set_policy(json::array({{{"id", "jira-corp"}, {"kind", "remote"}, {"url", url}, {"auth", "user-token"}}}));
    f.start();
    f.apply();
    json st = f.server_status("jira-corp");
    EXPECT_EQ(st["state"], "unavailable");
    EXPECT_NE(st["error"].get<std::string>().find("refused the token"), std::string::npos) << st.dump();

    f.user_token = [](bool) { return fake_jwt("u1"); };
    f.set_policy(json::array({{{"id", "jira-corp"}, {"kind", "remote"}, {"url", url}, {"auth", "user-token"}}}),
                 {{"network", {{"egress_allow", {"jira.example"}}}}});
    f.apply();
    st = f.server_status("jira-corp");
    EXPECT_EQ(st["state"], "unavailable");
    EXPECT_NE(st["error"].get<std::string>().find("blocked by the network policy"), std::string::npos) << st.dump();

    // The same catalog entry, and a policy that now allows the address: discovered again without a restart.
    f.set_policy(json::array({{{"id", "jira-corp"}, {"kind", "remote"}, {"url", url}, {"auth", "user-token"}}}),
                 {{"network", {{"egress_allow", {"127.0.0.1"}}}}});
    f.apply();
    st = f.server_status("jira-corp");
    EXPECT_EQ(st["state"], "connected") << st.dump();
}

TEST(McpManager, LocalServerLimitHitCountsAsACrash) {
    McpFixture f;
    f.set_policy(json::array({f.local_entry()}), {{"tools", {{"mcp", {{"limits", {{"memory_mb", 64}}}}}}}});
    f.start();
    f.apply();
    EXPECT_EQ(f.call("mcp:local-tool/alloc", {{"mb", 8}}).content, "allocated 8 MB");
    auto r = f.call("mcp:local-tool/alloc", {{"mb", 256}});
    EXPECT_FALSE(r.ok);
    const json st = f.server_status("local-tool");
    EXPECT_EQ(st["state"], "idle") << st.dump();
    EXPECT_NE(st["error"].get<std::string>().find("crashed"), std::string::npos) << st.dump();
    EXPECT_EQ(f.call("mcp:local-tool/echo", {{"text", "ok"}}).content, "echo: ok");
}
