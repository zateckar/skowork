// Skowork.exe end to end (client/desktop-host): single instance with argument hand-off, sign-in through the system
// browser and the loopback redirect, no listening sockets outside sign-in, no process for the window, and no tokens
// in the logs.
#include "e2e_support.h"

#include "platform/crypto.h"
#include "platform/pipe.h"
#include "policy/core.h"
#include "policy/decide.h"
#include "platform/win.h"
#include "base/encoding.h"
#include "base/strings.h"
#include "shell/feed.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/keystore.h"
#include "../support/dev_endpoints.h"
#include "tools/image_info.h"
#include "ui/generated_surface.h"

#include <sqlite3mc_amalgamation.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tlhelp32.h>
#include <udpmib.h>

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using namespace wb;
using namespace wb::e2e;
using namespace std::chrono_literals;

namespace {

struct AppProcess {
    unique_handle process;
    DWORD pid = 0;
};

AppProcess launch(const std::wstring& args, const wchar_t* executable = L"Skowork.exe") {
    const auto exe = exe_dir() / executable;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return {};
    ::CloseHandle(pi.hThread);
    return {unique_handle(pi.hProcess), pi.dwProcessId};
}

struct Instance {
    std::wstring pipe_short = L"skowork-e2e-" + widen(random_urlsafe(6));
    std::wstring pipe = L"\\\\.\\pipe\\" + pipe_short;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / L"skowork-test" / widen("app-" + random_urlsafe(6));
    AppProcess main;
    bool keep = false;  // keep the data root (and its sign-in) after the test

    std::wstring common_args() const {
        return L"--data-root \"" + (dir / L"data").wstring() + L"\" --pipe-name " + pipe_short + L" --no-register --config \"" +
               (dir / L"machine.json").wstring() + L"\"";
    }
    void write_config(const json& cfg) {
        std::filesystem::create_directories(dir);
        std::ofstream(dir / L"machine.json") << cfg.dump();
    }
    bool start(const std::wstring& extra = L"--background") {
        main = launch(common_args() + L" " + extra);
        for (int i = 0; i < 100; ++i) {
            if (call("app.version")) return true;
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }
    Result<json> call(const std::string& method, const json& params = json::object()) {
        auto r = pipe_call(pipe, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump(), 2000);
        if (!r) return r.error();
        auto j = parse_json(*r);
        if (!j) return make_error("parse", *r);
        if (j->contains("error")) return make_error("rpc", (*j)["error"].dump());
        return (*j)["result"];
    }
    ~Instance() {
        if (main.process) {
            (void)call("app.quit");
            if (::WaitForSingleObject(main.process.get(), 10000) != WAIT_OBJECT_0) ::TerminateProcess(main.process.get(), 1);
        }
        if (keep) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

bool exited_within(AppProcess& p, DWORD ms, DWORD* code = nullptr) {
    if (::WaitForSingleObject(p.process.get(), ms) != WAIT_OBJECT_0) return false;
    if (code) ::GetExitCodeProcess(p.process.get(), code);
    return true;
}

bool process_alive(DWORD pid) {
    unique_handle h(::OpenProcess(SYNCHRONIZE, FALSE, pid));
    return h && ::WaitForSingleObject(h.get(), 0) == WAIT_TIMEOUT;
}

// The direct child processes of a process: pid -> executable name.
std::map<DWORD, std::wstring> child_processes(DWORD parent) {
    std::map<DWORD, std::wstring> out;
    unique_handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap || snap.get() == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = ::Process32FirstW(snap.get(), &e); ok; ok = ::Process32NextW(snap.get(), &e))
        if (e.th32ParentProcessID == parent) out[e.th32ProcessID] = e.szExeFile;
    return out;
}

// Listening TCP sockets and UDP sockets owned by the process.
int listening(DWORD pid) {
    int n = 0;
    ULONG size = 0;
    ::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buf(size);
    if (::GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) n += t->table[i].dwOwningPid == pid;
    }
    size = 0;
    ::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
    buf.assign(size, 0);
    if (::GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) n += t->table[i].dwOwningPid == pid;
    }
    size = 0;
    ::GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    buf.assign(size, 0);
    if (::GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
        auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) n += t->table[i].dwOwningPid == pid;
    }
    return n;
}

// The machine configuration for the local control plane and the organization identity provider (system browser).
json stack_config() {
    return {{"control_plane_url", env()->cp},       {"idp_issuer", env()->issuer}, {"idp_client_id", env()->client_id},
            {"idp_redirect_uri", env()->redirect_uri}, {"idp_scope", env()->scope}, {"channel", "e2e"}};
}

}  // namespace

TEST(AppE2E, ExistingClientOpensProfileWithNewerAdditiveSchema) {
    WB_REQUIRES_DEV_ENDPOINTS();
    // Characterizes the current local binary. Replace with the actual previous-release barrier test when available.
    Instance a;
    a.write_config(json{{"control_plane_url", ""}});
    {
        auto root = DataRoot::open(a.dir / L"data");
        ASSERT_TRUE(root);
        KeyStore keys(root->keys());
        ASSERT_TRUE(keys.load());
        auto key = keys.get_or_create("app.db");
        ASSERT_TRUE(key);
        auto migrations = app_migrations();
        ASSERT_FALSE(migrations.empty());
        migrations.push_back({migrations.back().version + 1, "CREATE TABLE prototype_recovery_ledger(id INTEGER PRIMARY KEY);"});
        MigrationOutcome outcome;
        auto db = open_and_migrate(root->file("app.db"), *key, migrations, outcome);
        ASSERT_TRUE(db);
        ASSERT_FALSE(outcome.degraded);
    }
    ASSERT_TRUE(a.start());
    auto status = a.call("app.status");
    ASSERT_TRUE(status);
    ASSERT_TRUE(status->contains("storage"));
    EXPECT_FALSE(jbool((*status)["storage"], "read_only", true));
    EXPECT_TRUE(jstr((*status)["storage"], "error").empty());
}

TEST(AppE2E, SecondLaunchHandsOverAndExits) {
    Instance a;
    a.write_config(json{{"control_plane_url", ""}});
    ASSERT_TRUE(a.start());
    auto ui = a.call("dev.ui", {{"action", "status"}});
    ASSERT_TRUE(ui);
    EXPECT_FALSE(jbool(*ui, "open"));

    // Launched again from the Start menu: brings the window up in the running instance and exits.
    AppProcess second = launch(a.common_args());
    DWORD code = 99;
    ASSERT_TRUE(exited_within(second, 10000, &code));
    EXPECT_EQ(code, 0u);
    EXPECT_TRUE(process_alive(a.main.pid));
    bool opened = false;
    for (int i = 0; i < 50 && !opened; ++i) {
        auto s = a.call("dev.ui", {{"action", "status"}});
        opened = s && jbool(*s, "open");
        if (!opened) std::this_thread::sleep_for(100ms);
    }
    EXPECT_TRUE(opened);
    (void)a.call("dev.ui", {{"action", "close"}});
}

TEST(AppE2E, SignInCompletesThroughTheLoopbackRedirect) {
    SKOWORK_REQUIRE_STACK();
    Instance a;
    a.write_config(stack_config());
    // First start without a session opens the system browser; single sign-on (or the user) completes it.
    ASSERT_TRUE(a.start());
    json status;
    for (int i = 0; i < 1800; ++i) {  // up to 3 minutes for an interactive sign-in
        auto s = a.call("app.status");
        if (s) status = *s;
        if (s && jbool((*s)["control_plane"], "connected")) break;
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_EQ(jstr(status["auth"], "state"), "signed_in") << status.dump();
    EXPECT_FALSE(jstr(status["auth"], "user").empty());
    EXPECT_TRUE(jbool(status["control_plane"], "connected")) << status.dump();
    // The loopback listener closed with the sign-in (client/desktop-host Network Surface).
    EXPECT_EQ(listening(a.main.pid), 0);

    // client/desktop-host Diagnostic Logs: authenticating wrote no token-shaped strings.
    (void)a.call("app.quit");
    ASSERT_TRUE(exited_within(a.main, 10000));
    a.main = {};
    std::string all;
    for (const auto& e : std::filesystem::directory_iterator(a.dir / L"data" / L"logs")) {
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        all += ss.str();
    }
    EXPECT_NE(all.find("registered installation"), std::string::npos);
    EXPECT_FALSE(std::regex_search(all, std::regex(R"(eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.)")));
    EXPECT_FALSE(std::regex_search(all, std::regex(R"([A-Za-z0-9_-]{40,})")));
    EXPECT_EQ(all.find("code="), std::string::npos);
}

TEST(AppE2E, NoListeningSocketsAndNoProcessForTheWindow) {
    Instance a;
    a.write_config(json{{"control_plane_url", ""}});
    ASSERT_TRUE(a.start());

    EXPECT_EQ(listening(a.main.pid), 0);
    const auto before = child_processes(a.main.pid);
    auto opened = a.call("dev.ui", {{"action", "open"}});
    ASSERT_TRUE(opened);
    EXPECT_TRUE(jbool(*opened, "open"));
    std::this_thread::sleep_for(1500ms);  // anything the window started would be running by now
    // The native window is drawn by the client itself (client/desktop-host On-Demand UI Window): no new process.
    for (const auto& [pid, name] : child_processes(a.main.pid))
        if (!before.contains(pid)) EXPECT_EQ(name, L"skowork-worker.exe") << "process started with the window";
    auto closed = a.call("dev.ui", {{"action", "close"}});
    ASSERT_TRUE(closed);
    EXPECT_FALSE(jbool(*closed, "open"));
    EXPECT_TRUE(process_alive(a.main.pid)) << "the client keeps running in the tray";
    EXPECT_EQ(listening(a.main.pid), 0);
}

TEST(AppE2E, QuitWithOpenWindowAndNoPendingDrafts) {
    Instance a;
    a.write_config(json{{"control_plane_url", ""}});
    ASSERT_TRUE(a.start());
    const auto opened = a.call("dev.ui", {{"action", "open"}});
    ASSERT_TRUE(opened);
    ASSERT_TRUE(jbool(*opened, "open"));
    ASSERT_TRUE(a.call("app.quit"));
    ASSERT_TRUE(exited_within(a.main, 10000));
    a.main = {};
}

TEST(AppE2E, GeneratedSurfacePreferenceSurvivesClientRestart) {
    Instance a;
    a.write_config(json{{"control_plane_url", "https://127.0.0.1:1"}, {"idp_issuer", "https://127.0.0.1:1/realm"},
                        {"idp_client_id", "skowork-test"}, {"dev_browser", "record"}});
    ASSERT_TRUE(a.start());
    Result<json> initial = make_error("unavailable", "waiting for settings");
    for (int i = 0; i < 100 && !initial; ++i) {
        initial = a.call("settings.get");
        if (!initial) std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(initial);
    EXPECT_FALSE(jbool((*initial)["ui.generated_surfaces"], "value"));
    ASSERT_TRUE(a.call("settings.set", {{"key", "ui.generated_surfaces"}, {"value", true}}));
    ASSERT_TRUE(a.call("app.quit"));
    ASSERT_TRUE(exited_within(a.main, 10000));
    a.main = {};
    ASSERT_TRUE(a.start());
    Result<json> restored = make_error("unavailable", "waiting for settings");
    for (int i = 0; i < 100 && !restored; ++i) {
        restored = a.call("settings.get");
        if (!restored) std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(restored);
    EXPECT_TRUE(jbool((*restored)["ui.generated_surfaces"], "value"));
}
// add-agent-runtime 6.1-6.3: the agent runtime in Skowork.exe. A message sent over the (development) pipe API starts
// a Run against the real gateway and model; the Run context gives the model today's date; while the control plane is
// unreachable a new Run is refused with the reason (needs SKOWORK_E2E_CP_UPSTREAM for the outage part).
TEST(AppE2E, AgentRunThroughTheLocalApi) {
    SKOWORK_REQUIRE_STACK();
    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());
    const auto status_until = [&a](const std::function<bool(const json&)>& pred, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            if (auto s = a.call("app.status")) {
                last = *s;
                if (pred(last)) return last;
            }
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    json st = status_until([](const json& s) { return s.contains("agent") && jstr(s["agent"], "start_blocked").empty(); }, 180);
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    char today[16];
    std::snprintf(today, sizeof today, "%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);
    auto sent = a.call("threads.send", {{"text", "What is today's date? Answer with the date only, as YYYY-MM-DD."}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    ASSERT_FALSE(run.empty());
    std::string state;
    for (int i = 0; i < 1800 && state != "succeeded" && state != "failed" && state != "cancelled"; ++i) {
        if (auto r = a.call("runs.get", {{"run_id", run}})) state = jstr(*r, "state");
        std::this_thread::sleep_for(100ms);
    }
    auto got = a.call("threads.get", {{"thread_id", thread}});
    ASSERT_TRUE(got);
    ASSERT_EQ(state, "succeeded") << got->dump();
    const std::string answer = jstr((*got)["messages"].back(), "content");
    EXPECT_NE(answer.find(today), std::string::npos) << answer;
    auto events = a.call("runs.events", {{"run_id", run}});
    ASSERT_TRUE(events);
    EXPECT_FALSE((*events)["events"].empty());
    auto list = a.call("threads.list");
    ASSERT_TRUE(list);
    EXPECT_EQ(jstr((*list)["threads"].at(0), "id"), thread);

    if (!cp_proxy()) {
        std::printf("[ SKIPPED part ] outage check: set SKOWORK_E2E_CP_UPSTREAM to run it through the proxy\n");
        return;
    }
    cp_proxy()->set_down(true);
    st = status_until([](const json& s) { return jstr(s["agent"], "start_blocked").find("disconnected") != std::string::npos; }, 120);
    auto refused = a.call("threads.send", {{"text", "hello"}});
    cp_proxy()->set_down(false);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().message.find("disconnected"), std::string::npos) << refused.error().message;
    st = status_until([](const json& s) { return jstr(s["agent"], "start_blocked").empty(); }, 180);
    EXPECT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
}

// add-policy-and-sandbox 5.1: an admin publishes a stricter developers policy to a pilot; the pilot client receives the
// policy-changed push, a tool call it allowed is now decided as approve, and simulation shows the same result. The
// test uses its own Groups (e2e-developers, e2e-pilot) and tool id, so it does not disturb the dev stack's policy.
TEST(AppE2E, PilotRolloutReachesTheClientAndMatchesSimulation) {
    SKOWORK_REQUIRE_STACK();
    auto admin = admin_token();
    ASSERT_TRUE(admin) << admin.error().message;
    const std::string user = user_name_of(*admin);
    const std::string group = "/api/v1/admin/policy/groups/e2e-developers";
    const std::string tool = "e2e.rollout.probe";
    const auto members = [&](const char* method) {
        for (const char* g : {"e2e-developers", "e2e-pilot"})
            (void)cp_call(method, "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", g}});
    };
    const auto publish = [&](const char* decision, const json& rollout) {
        auto d = cp_call("PUT", group + "/draft", *admin,
                         {{"document", {{"tools", {{"rules", {{tool, decision}}}}}}}, {"priority", 0}, {"reason", "e2e 5.1"}});
        EXPECT_EQ(d.first, 200) << d.second.dump();
        json body = {{"reason", "e2e 5.1"}};
        if (!rollout.is_null()) body["rollout"] = rollout;
        auto p = cp_call("POST", group + "/publish", *admin, body);
        EXPECT_EQ(p.first, 200) << p.second.dump();
        return p.second;
    };
    (void)cp_call("POST", group + "/rollout/cancel", *admin, {{"reason", "e2e 5.1 reset"}});  // a run that stopped halfway
    members("POST");
    struct Cleanup {
        std::function<void()> f;
        ~Cleanup() { f(); }
    } cleanup{[&] {
        (void)cp_call("POST", group + "/rollout/cancel", *admin, {{"reason", "e2e 5.1 done"}});
        members("DELETE");
    }};
    publish("allow", nullptr);

    // The pilot client: registered after the Group assignments, so the control plane knows its Groups.
    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());
    const auto evaluate = [&a, &tool] {
        auto r = a.call("policy.evaluate", {{"tool", tool}, {"risk", "external"}});
        return r ? *r : json{{"error", r.error().message}};
    };
    const auto evaluate_until = [&](const std::string& decision, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            last = evaluate();
            if (jstr(last, "decision") == decision) break;
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    json before = evaluate_until("allow", 180);
    ASSERT_EQ(jstr(before, "decision"), "allow") << before.dump();
    EXPECT_EQ(jstr(before, "source"), "e2e-developers") << before.dump();

    const json p = publish("approve", {{"pilot_group", "e2e-pilot"}});
    EXPECT_GE(jint(p, "notified"), 1) << "the pilot client gets a policy-changed push: " << p.dump();
    // The periodic refresh is hours away, so a change within a minute came from the push.
    json after = evaluate_until("approve", 60);
    ASSERT_EQ(jstr(after, "decision"), "approve") << after.dump();
    EXPECT_EQ(jstr(after, "rule"), "tools.rules[\"" + tool + "\"]") << after.dump();
    EXPECT_GT(jint(after, "policy_version"), jint(before, "policy_version"));

    // Simulation for the pilot user decides the same way; the Group alone (no pilot member) still has the old version.
    const auto simulated = [&](const json& who) {
        auto s = cp_call("POST", "/api/v1/admin/policy/simulate", *admin, who);
        EXPECT_EQ(s.first, 200) << s.second.dump();
        policy::PolicyBundle b;
        b.policy = s.second["policy"];
        b.src = s.second["src"];
        policy::EffectivePolicy ep(std::move(b));
        policy::CallFacts f;
        f.tool = tool;
        f.risk = "external";
        return policy::decide(&ep, f);
    };
    const policy::Verdict sim = simulated({{"user", user}});
    EXPECT_EQ(policy::to_string(sim.decision), jstr(after, "decision"));
    EXPECT_EQ(sim.rule, jstr(after, "rule"));
    EXPECT_EQ(sim.source, jstr(after, "source"));
    EXPECT_EQ(policy::to_string(simulated({{"groups", {"e2e-developers"}}}).decision), std::string("allow"));
}

// add-core-tools-and-mcp 5.1: a developer-Group user asks the agent to search Jira through the catalog server, edit a
// local file and run a build command in the sandbox. Every call passes the policy hook, and the provenance of the Jira
// result appears in the Thread. The Jira server is the dev-only mock (dev/mcp-test-server) on loopback; the test uses
// its own Group (e2e-mcp-developers) and catalog entry (e2e-jira) and removes them afterwards.
namespace {

struct NodeServer {
    PROCESS_INFORMATION pi{};
    std::string url;
    NodeServer() {
        wchar_t node[MAX_PATH];
        if (::SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, node, nullptr) == 0) return;
        const std::filesystem::path script = std::filesystem::path(SKOWORK_MCP_TEST_SERVER_DIR) / "server.mjs";
        if (!std::filesystem::exists(std::filesystem::path(SKOWORK_MCP_TEST_SERVER_DIR) / "node_modules")) return;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE r = nullptr, w = nullptr;
        ::CreatePipe(&r, &w, &sa, 0);
        ::SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = w;
        std::wstring cmd = L"\"" + std::wstring(node) + L"\" \"" + script.wstring() + L"\" --http 0";
        if (!::CreateProcessW(node, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            ::CloseHandle(r);
            ::CloseHandle(w);
            return;
        }
        ::CloseHandle(w);
        std::string line;
        char c = 0;
        DWORD n = 0;
        while (::ReadFile(r, &c, 1, &n, nullptr) && n == 1 && c != '\n') line.push_back(c);
        ::CloseHandle(r);  // the server logs nothing more
        if (auto j = parse_json(line)) url = jstr(*j, "listening");
    }
    ~NodeServer() {
        if (!pi.hProcess) return;
        ::TerminateProcess(pi.hProcess, 0);
        ::WaitForSingleObject(pi.hProcess, 5000);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }
};

}  // namespace

TEST(AppE2E, DeveloperSearchesJiraEditsAFileAndRunsABuild) {
    SKOWORK_REQUIRE_STACK();
    NodeServer jira;
    if (jira.url.empty()) GTEST_SKIP() << "node or dev/mcp-test-server/node_modules missing (npm ci there)";
    auto admin = admin_token();
    ASSERT_TRUE(admin) << admin.error().message;
    const std::string user = user_name_of(*admin);
    const std::string group_name = "e2e-mcp-developers";
    const std::string group = "/api/v1/admin/policy/groups/" + group_name;
    const std::string catalog = "/api/v1/admin/mcp/servers";

    (void)cp_call("DELETE", catalog + "/e2e-jira", *admin);  // a run that stopped halfway
    struct Cleanup {
        std::function<void()> f;
        ~Cleanup() { f(); }
    } cleanup{[&] {
        (void)cp_call("DELETE", catalog + "/e2e-jira", *admin);
        (void)cp_call("DELETE", "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", group_name}});
    }};
    // The catalog entry: discovery runs against the mock with the administrator's identity-provider token.
    auto created = cp_call("POST", catalog, *admin,
                           {{"id", "e2e-jira"}, {"name", "Jira (e2e)"}, {"owner", "e2e"}, {"kind", "remote"}, {"url", jira.url},
                            {"groups", {group_name}}, {"exclude_tools", {"delete_issue"}}, {"risk", {{"search_issues", "read"}, {"get_issue", "read"}}}});
    ASSERT_EQ(created.first, 200) << created.second.dump();
    ASSERT_EQ(jstr(created.second["discovery"], "status"), "ok") << created.second.dump();
    // The developer Group's policy: the sandboxed shell, the Jira tools, and egress to the loopback mock.
    auto draft = cp_call("PUT", group + "/draft", *admin,
                         {{"document",
                           {{"tools", {{"shell", {{"mode", "sandboxed"}}},
                                       {"rules", {{"mcp:e2e-jira/*", "allow"}, {"files.*", "allow"}, {"shell.run", "allow"}}}}},
                            {"network", {{"egress_allow", {"127.0.0.1"}}}}}},
                          {"priority", 0},
                          {"reason", "e2e 5.1 (add-core-tools-and-mcp)"}});
    ASSERT_EQ(draft.first, 200) << draft.second.dump();
    auto published = cp_call("POST", group + "/publish", *admin, {{"reason", "e2e 5.1 (add-core-tools-and-mcp)"}});
    ASSERT_EQ(published.first, 200) << published.second.dump();
    (void)cp_call("POST", "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", group_name}});

    Instance a;
    a.write_config(stack_config());
    const auto ws = a.dir / L"data" / L"workspace";
    std::filesystem::create_directories(ws);
    std::ofstream(ws / L"notes.txt") << "Release blocker: TODO\n";
    std::ofstream(ws / L"build.ps1") << "if ((Get-Content notes.txt -Raw) -match 'DEMO-10[13]') { 'BUILD OK'; exit 0 } else { 'BUILD FAILED'; exit 1 }\n";
    ASSERT_TRUE(a.start());
    const auto status_until = [&a](const std::function<bool(const json&)>& pred, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            if (auto s = a.call("app.status")) {
                last = *s;
                if (pred(last)) return last;
            }
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    // Connections: the catalog server with its usable tools (delete_issue is excluded by the catalog).
    json st = status_until([](const json& s) {
        if (!s.contains("agent") || !jstr(s["agent"], "start_blocked").empty() || !s.contains("mcp")) return false;
        for (const auto& m : s["mcp"])
            if (jstr(m, "id") == "e2e-jira" && jint(m, "usable_tools") == 3) return true;
        return false;
    }, 180);
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
    ASSERT_NE(st["mcp"].dump().find("\"e2e-jira\""), std::string::npos) << st["mcp"].dump();

    auto sent = a.call("threads.send",
                       {{"text",
                         "Please do these three steps in order. 1) Use the Jira tool mcp:e2e-jira/search_issues with the query \"build\". "
                         "2) In the file notes.txt in your workspace folder, replace the word TODO with the key of the first issue "
                         "that search returned, using files.edit. 3) Run the command .\\build.ps1 with shell.run in your workspace "
                         "folder. Then reply with the build output."}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    std::string state;
    for (int i = 0; i < 3000 && state != "succeeded" && state != "failed" && state != "cancelled" && state != "interrupted"; ++i) {
        if (auto r = a.call("runs.get", {{"run_id", run}})) state = jstr(*r, "state");
        std::this_thread::sleep_for(100ms);
    }
    auto got = a.call("threads.get", {{"thread_id", thread}});
    ASSERT_TRUE(got);
    ASSERT_EQ(state, "succeeded") << got->dump(2);

    // The file was edited and the sandboxed build saw the edit.
    std::ifstream notes(ws / L"notes.txt");
    std::stringstream ns;
    ns << notes.rdbuf();
    EXPECT_TRUE(ns.str().find("DEMO-101") != std::string::npos || ns.str().find("DEMO-103") != std::string::npos) << ns.str();
    EXPECT_EQ(ns.str().find("TODO"), std::string::npos) << ns.str();

    auto events = a.call("runs.events", {{"run_id", run}});
    ASSERT_TRUE(events);
    std::map<std::string, std::string> calls;  // call_id -> tool
    json jira_result;
    std::string build_output;
    for (const auto& e : (*events)["events"]) {
        const json& p = e["payload"];
        if (jstr(e, "kind") == "tool_call") calls[jstr(p, "call_id")] = jstr(p, "tool");
        if (jstr(e, "kind") == "tool_result" && jstr(p, "tool") == "mcp:e2e-jira/search_issues") jira_result = p;
        if (jstr(e, "kind") == "tool_result" && jstr(p, "tool") == "shell.run") build_output += jstr(p, "content");
    }
    std::set<std::string> tools;
    for (const auto& [id, t] : calls) tools.insert(t);
    EXPECT_TRUE(tools.count("mcp:e2e-jira/search_issues")) << (*events).dump();
    EXPECT_TRUE(tools.count("files.edit") || tools.count("files.write")) << (*events).dump();
    EXPECT_TRUE(tools.count("shell.run")) << (*events).dump();
    EXPECT_NE(build_output.find("BUILD OK"), std::string::npos) << build_output;

    // Provenance of the Jira result: untrusted, the catalog server and tool.
    ASSERT_TRUE(jira_result.is_object()) << (*events).dump();
    EXPECT_NE(jstr(jira_result, "content").find("DEMO-101"), std::string::npos);
    EXPECT_EQ(jstr(jira_result["provenance"], "source_kind"), "mcp");
    EXPECT_EQ(jstr(jira_result["provenance"], "source_id"), "e2e-jira/search_issues");
    EXPECT_FALSE(jbool(jira_result["provenance"], "trusted", true));

    // Every call passed the policy hook: the hook's decision is recorded with each call, and the policy in force
    // decides each of these tools by the developer Group's rule. (The approval log keeps only deny and approve.)
    for (const auto& e : (*events)["events"]) {
        if (jstr(e, "kind") != "tool_call") continue;
        const json& p = e["payload"];
        const std::string t = jstr(p, "tool");
        EXPECT_EQ(jstr(p, "decision"), "allow") << p.dump();
        if (t == "find_tools") continue;  // the runtime's own tool
        auto v = a.call("policy.evaluate", {{"tool", t}, {"risk", jstr(p, "risk")}});
        ASSERT_TRUE(v) << v.error().message;
        EXPECT_EQ(jstr(*v, "decision"), "allow") << t << " " << v->dump();
        EXPECT_EQ(jstr(*v, "source"), group_name) << t << " " << v->dump();
    }
    // Connections after the Run: the server is connected and healthy.
    st = status_until([](const json& s) { return s.contains("mcp"); }, 5);
    for (const auto& m : st["mcp"])
        if (jstr(m, "id") == "e2e-jira") {
            EXPECT_EQ(jstr(m, "state"), "connected") << m.dump();
            EXPECT_TRUE(m["warning"].is_null()) << m.dump();
        }
}

// add-work-inbox 8.1 (and the toast part of 6.1): a user-started Run in a Thread raises an Approval while the window
// is closed; Windows shows a Skowork toast (needs the AUMID registration under HKCU); the toast's click handling opens
// the window on that Thread, whose Approval carries its provenance; once approved the Run completes, and with the
// window closed again the Thread becomes unread. The physical click on the toast is checked by hand.
TEST(AppE2E, BackgroundApprovalRaisesAToastThatOpensTheThread) {
    SKOWORK_REQUIRE_STACK();
    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());  // --background: the window is closed
    const auto until = [&a](const std::string& method, const json& params, const std::function<bool(const json&)>& pred, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            if (auto r = a.call(method, params)) {
                last = *r;
                if (pred(last)) return last;
            }
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    json st = until("app.status", json::object(), [](const json& s) { return s.contains("agent") && jstr(s["agent"], "start_blocked").empty(); }, 180);
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
    if (auto toasts = a.call("dev.toasts"); toasts && jint(*toasts, "history") < 0)
        GTEST_SKIP() << "Windows toast notifications are unavailable (AUMID not registered under HKCU?)";

    // files.write is write-local: the default policy asks for approval.
    auto sent = a.call("threads.send", {{"text", "Create the file toast-e2e.txt in the workspace with the content hello. "
                                                   "Call files.write directly; do not ask me anything first."}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    json r = until("runs.get", {{"run_id", run}}, [](const json& x) {
        const std::string s = jstr(x, "state");
        return s == "interrupted" || s == "succeeded" || s == "failed" || s == "cancelled";
    }, 180);
    ASSERT_EQ(jstr(r, "state"), "interrupted") << r.dump();

    // The toast: shown by the host for this Thread and accepted by Windows into the Action Center.
    json toasts = until("dev.toasts", json::object(), [&thread](const json& t) {
        for (const auto& id : t["shown"]) if (id == thread) return true;
        return false;
    }, 10);
    ASSERT_NE(std::find(toasts["shown"].begin(), toasts["shown"].end(), json(thread)), toasts["shown"].end()) << toasts.dump();
    if (jstr(toasts, "setting") == "enabled") {
        EXPECT_GE(jint(toasts, "history"), 1) << toasts.dump();
    } else {
        // Windows takes the toast but drops it when the user turned notifications off; the rest still runs.
        std::printf("[ SKIPPED part ] Action Center check: Windows notifications are %s for Skowork\n", jstr(toasts, "setting").c_str());
    }
    EXPECT_FALSE(jbool(toasts, "window_open"));
    auto entry = a.call("inbox.entry", {{"thread_id", thread}});
    ASSERT_TRUE(entry);
    EXPECT_TRUE(jbool(*entry, "has_action")) << entry->dump();

    // Clicking it opens the window on the Thread.
    ASSERT_TRUE(a.call("dev.toasts", {{"activate", thread}}));
    toasts = until("dev.toasts", json::object(), [&thread](const json& t) { return jbool(t, "window_open") && jstr(t, "open_thread") == thread; }, 30);
    EXPECT_TRUE(jbool(toasts, "window_open")) << toasts.dump();
    EXPECT_EQ(jstr(toasts, "open_thread"), thread) << toasts.dump();

    // The Approval card's data: the call, the rationale field and the provenance list.
    auto feed = a.call("threads.feed", {{"thread_id", thread}});
    ASSERT_TRUE(feed) << feed.error().message;
    json approval;
    for (const auto& x : (*feed)["actions"])
        if (jstr(x, "kind") == "approval" && x["resolution"].is_null()) approval = x;
    ASSERT_FALSE(approval.is_null()) << feed->dump();
    EXPECT_EQ(jstr(approval["payload"], "tool"), "files.write") << approval.dump();
    EXPECT_TRUE(approval["payload"].contains("untrusted_context")) << approval.dump();

    // Approve, close the window, and the completed Run leaves the Thread unread.
    ASSERT_TRUE(a.call("dev.ui", {{"action", "close"}}));
    toasts = until("dev.toasts", json::object(), [](const json& t) { return jstr(t, "open_thread").empty(); }, 10);
    EXPECT_EQ(jstr(toasts, "open_thread"), "") << toasts.dump();
    ASSERT_TRUE(a.call("threads.mark_unread", {{"thread_id", thread}, {"unread", false}}));
    auto resolved = a.call("actions.resolve", {{"action_id", jstr(approval, "id")}, {"decision", "approve"}});
    ASSERT_TRUE(resolved) << resolved.error().message;
    r = until("runs.get", {{"run_id", run}}, [](const json& x) {
        const std::string s = jstr(x, "state");
        return s == "succeeded" || s == "failed" || s == "cancelled";
    }, 180);
    EXPECT_EQ(jstr(r, "state"), "succeeded") << r.dump();
    const json done = until("inbox.entry", {{"thread_id", thread}}, [](const json& e) { return jbool(e, "unread"); }, 10);
    EXPECT_TRUE(jbool(done, "unread")) << done.dump();
    EXPECT_FALSE(jbool(done, "has_action")) << done.dump();
}

// add-telemetry-and-admin 6.1: the pilot rehearsal. A client at the metadata level runs a session; its records reach
// the ingest. The administrator's dashboards show aggregates and no content; the user's Telemetry viewer lists the
// same records the auditor reads; the auditor's access needs a reason and appears in the access records. The dev
// stack has one user, so the user's Groups are under the k floor: the dashboards with populated Groups are covered
// by the control-plane tests.
TEST(AppE2E, PilotRehearsalMetadataTelemetryAndAuditedAccess) {
    SKOWORK_REQUIRE_STACK();
    auto admin = admin_token();
    ASSERT_TRUE(admin) << admin.error().message;
    const std::string user = user_name_of(*admin);
    const std::string roles = "/api/v1/admin/identity/admin-roles";
    // The bootstrap admin is a platform-admin only: no individual-level access until granted the auditor role.
    (void)cp_call("DELETE", roles, *admin, {{"user_name", user}, {"role", "security-auditor"}});  // a run that stopped halfway
    struct Cleanup {
        std::function<void()> f;
        ~Cleanup() { f(); }
    } cleanup{[&] { (void)cp_call("DELETE", roles, *admin, {{"user_name", user}, {"role", "security-auditor"}}); }};

    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());
    const auto until = [&a](const std::string& method, const json& params, const std::function<bool(const json&)>& pred, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            if (auto r = a.call(method, params)) {
                last = *r;
                if (pred(last)) return last;
            }
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    json st = until("app.status", json::object(), [](const json& s) { return s.contains("agent") && jstr(s["agent"], "start_blocked").empty(); }, 180);
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
    json view = until("telemetry.view", json::object(), [](const json&) { return true; }, 10);
    ASSERT_EQ(jstr(view, "level"), "metadata") << "the dev stack's policy must leave telemetry at metadata: " << view.dump();
    EXPECT_FALSE(jbool(view, "content_active"));

    // The session: a marker in the prompt and in the answer that must not leave the client.
    const std::string marker = "pilot-" + random_urlsafe(8);
    auto sent = a.call("threads.send", {{"text", "Reply with exactly this word and nothing else: " + marker}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    json r = until("runs.get", {{"run_id", run}}, [](const json& x) {
        const std::string s = jstr(x, "state");
        return s == "succeeded" || s == "failed" || s == "cancelled" || s == "interrupted";
    }, 180);
    ASSERT_EQ(jstr(r, "state"), "succeeded") << r.dump();

    // The user's viewer: the Run's records, sent at the next flush (one a minute), with metadata fields only.
    const auto run_records = [&run](const json& v) {
        json out = json::array();
        for (const auto& x : v["records"])
            if (x.contains("attrs") && jstr(x["attrs"], "run.id") == run) out.push_back(x);
        return out;
    };
    view = until("telemetry.view", json::object(), [&](const json& v) {
        bool finished = false;
        for (const auto& x : run_records(v)) {
            if (jstr(x, "state") != "sent") return false;
            finished |= jstr(x, "event") == "run.finished";
        }
        return finished;
    }, 150);
    const json mine = run_records(view);
    ASSERT_FALSE(mine.empty()) << view.dump();
    std::multiset<std::string> viewer_events;
    for (const auto& x : mine) {
        EXPECT_EQ(jstr(x, "state"), "sent") << x.dump();
        viewer_events.insert(jstr(x, "event"));
        for (const auto& [k, v] : x["attrs"].items()) EXPECT_EQ(k.rfind("content.", 0), std::string::npos) << x.dump();
    }
    EXPECT_EQ(viewer_events.count("run.finished"), 1u) << view.dump();
    EXPECT_EQ(view.dump().find(marker), std::string::npos) << "content in the viewer at the metadata level";

    // The administrator: aggregates per Group, nothing per user and no content; no individual-level access.
    auto dash = cp_call("GET", "/api/v1/admin/dashboards?days=1", *admin);
    ASSERT_EQ(dash.first, 200) << dash.second.dump();
    EXPECT_EQ(jint(dash.second, "k"), 5);
    ASSERT_FALSE(dash.second["groups"].empty()) << dash.second.dump();
    const std::string dash_text = dash.second.dump();
    EXPECT_EQ(dash_text.find(marker), std::string::npos);
    EXPECT_EQ(dash_text.find(run), std::string::npos);
    EXPECT_EQ(dash_text.find("\"" + user + "\""), std::string::npos) << dash_text;
    for (const auto& g : dash.second["groups"])
        if (jbool(g, "suppressed")) EXPECT_EQ(g.size(), 2u) << "a Group under the floor shows nothing: " << g.dump();
    const std::string individual = "/api/v1/admin/individual/telemetry?user=" + user + "&reason=e2e%206.1";
    EXPECT_EQ(cp_call("GET", individual, *admin).first, 403);

    // The auditor: a reason is required; the user's records at the ingest are the ones the viewer listed.
    ASSERT_EQ(cp_call("POST", roles, *admin, {{"user_name", user}, {"role", "security-auditor"}}).first, 200);
    EXPECT_EQ(cp_call("GET", "/api/v1/admin/individual/telemetry?user=" + user, *admin).first, 400);
    auto got = cp_call("GET", individual, *admin);
    ASSERT_EQ(got.first, 200) << got.second.dump();
    std::multiset<std::string> ingest_events;
    for (const auto& x : got.second["records"])
        if (jstr(x["attrs"], "run.id") == run) {
            ingest_events.insert(jstr(x, "event"));
            for (const auto& [k, v] : x["attrs"].items()) EXPECT_EQ(k.rfind("content.", 0), std::string::npos) << x.dump();
        }
    EXPECT_EQ(ingest_events, viewer_events) << got.second.dump();
    std::string listed;
    for (const auto& e : viewer_events) listed += " " + e;
    std::printf("[ pilot e2e ] %zu records of the Run sent and read by the auditor:%s\n", ingest_events.size(), listed.c_str());
    EXPECT_EQ(got.second.dump().find(marker), std::string::npos) << "content at the ingest at the metadata level";

    // The access is recorded, with the auditor, the user and the reason, for the works council review.
    auto records = cp_call("GET", "/api/v1/admin/access-records?limit=20", *admin);
    ASSERT_EQ(records.first, 200) << records.second.dump();
    bool recorded = false;
    for (const auto& x : records.second)
        recorded |= jstr(x, "kind") == "telemetry" && jstr(x, "user") == user && jstr(x, "auditor") == user && jstr(x, "reason") == "e2e 6.1";
    EXPECT_TRUE(recorded) << records.second.dump();
}

// add-image-generation 4.1: a user asks for an image in a Thread; the agent finds and calls image.generate on the real
// image model through the gateway; the Thread feed lists the stored image under the tool card, the window can read it,
// and the gateway's usage audit has exactly one image request for the Run. Reads the dev stack's control-plane
// database (dev/.stack/skowork.db, or SKOWORK_E2E_CP_DB) for the audit.
TEST(AppE2E, ImageRequestedInAThreadAppearsInTheFeed) {
    SKOWORK_REQUIRE_STACK();
    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());
    json st;
    for (int i = 0; i < 1800; ++i) {
        if (auto s = a.call("app.status")) {
            st = *s;
            if (st.contains("agent") && jstr(st["agent"], "start_blocked").empty()) break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();

    auto sent = a.call("threads.send", {{"text", "Please generate one square picture of a green Acme Falcon parked in front of "
                                                  "Prague Castle. Do not ask me anything first."}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    std::string state;
    for (int i = 0; i < 3000 && state != "succeeded" && state != "failed" && state != "cancelled" && state != "interrupted"; ++i) {
        if (auto r = a.call("runs.get", {{"run_id", run}})) state = jstr(*r, "state");
        std::this_thread::sleep_for(100ms);
    }
    auto feed = a.call("threads.feed", {{"thread_id", thread}});
    ASSERT_TRUE(feed) << feed.error().message;
    ASSERT_EQ(state, "succeeded") << feed->dump(2);

    // The feed the window builds: an image.generate card whose result lists one stored image.
    std::vector<shell::FileRef> images;
    int calls = 0;
    for (const auto& it : shell::build_feed(shell::snapshot_from(*feed))) {
        if (it.type != shell::ItemType::tool || it.tool != "image.generate") continue;
        ++calls;
        ASSERT_TRUE(it.result) << feed->dump(2);
        EXPECT_TRUE(it.result->ok) << it.result->content;
        for (const auto& f : it.result->files)
            if (f.image()) images.push_back(f);
    }
    EXPECT_EQ(calls, 1) << feed->dump(2);
    ASSERT_EQ(images.size(), 1u) << feed->dump(2);
    EXPECT_EQ(images[0].width, 1024);
    EXPECT_EQ(images[0].height, 1024);

    // What the window reads to draw it: a real image of the listed size.
    auto got = a.call("threads.attachment", {{"thread_id", thread}, {"id", images[0].id}});
    ASSERT_TRUE(got) << got.error().message;
    const auto bytes = base64_decode(jstr(*got, "data"));
    ASSERT_TRUE(bytes);
    EXPECT_EQ(static_cast<long long>(bytes->size()), images[0].size);
    const auto info = tools::image_info(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    ASSERT_TRUE(info);
    EXPECT_EQ(info->mime, images[0].mime);
    EXPECT_EQ(info->width, 1024);

    // The gateway's usage audit: one image request for this Run, one image.
    std::string db_path = [] {
        char buf[1024];
        const DWORD n = ::GetEnvironmentVariableA("SKOWORK_E2E_CP_DB", buf, sizeof buf);
        return n > 0 && n < sizeof buf ? std::string(buf, n) : std::string(SKOWORK_DEV_STACK_DIR "/skowork.db");
    }();
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK) << db_path;
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*), COALESCE(SUM(images), 0), MIN(role), MIN(outcome) FROM llm_usage "
                                     "WHERE run_id = ? AND bucket = 'image'", -1, &s, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
    sqlite3_bind_text(s, 1, run.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s, 0), 1);
    EXPECT_EQ(sqlite3_column_int(s, 1), 1);
    const auto text = [s](int i) { const auto* t = sqlite3_column_text(s, i); return t ? std::string(reinterpret_cast<const char*>(t)) : std::string{}; };
    EXPECT_EQ(text(2), "image");
    std::printf("[ image e2e ] outcome=%s, %lld bytes %s\n", text(3).c_str(), images[0].size, images[0].mime.c_str());
    sqlite3_finalize(s);
    sqlite3_close(db);
}
// add-computer-use 5.1: from a Thread, the agent on the real model fills and saves a form in a line-of-business test
// application (a WinForms window hosted by PowerShell 7) under a Group policy with computer use allowed, external
// actions allowed and destructive ones approved. Saving runs; submitting asks for approval, with the window as
// provenance. The overlay is up while the agent acts; the stop hotkey cancels a second Run; the tool outcomes are in the
// Telemetry viewer. Sends real input: run it in an unlocked session nobody is using at that moment.
TEST(AppE2E, ComputerUseFillsAndSavesAFormWithApprovalOverlayAndStop) {
    SKOWORK_REQUIRE_STACK();
    auto admin = admin_token();
    ASSERT_TRUE(admin) << admin.error().message;
    const std::string user = user_name_of(*admin);
    const std::string group_name = "e2e-computer";
    const std::string group = "/api/v1/admin/policy/groups/" + group_name;
    struct Cleanup {
        std::function<void()> f;
        ~Cleanup() { f(); }
    } cleanup{[&] { (void)cp_call("DELETE", "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", group_name}}); }};
    auto draft = cp_call("PUT", group + "/draft", *admin,
                         {{"document", {{"tools", {{"computer", {{"mode", "allow"}, {"perception", "tree_and_vision"}}},
                                                   {"risk", {{"read", "allow"}, {"external", "allow"}, {"destructive", "approve"}}}}}}},
                          {"priority", 0},
                          {"reason", "e2e 5.1 (add-computer-use)"}});
    ASSERT_EQ(draft.first, 200) << draft.second.dump();
    auto published = cp_call("POST", group + "/publish", *admin, {{"reason", "e2e 5.1 (add-computer-use)"}});
    ASSERT_EQ(published.first, 200) << published.second.dump();
    (void)cp_call("POST", "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", group_name}});

    // The line-of-business form.
    const std::string title = "Order entry " + random_urlsafe(5);
    const std::string script =
        "Add-Type -AssemblyName System.Windows.Forms; $f = New-Object System.Windows.Forms.Form; $f.Text = '" + title +
        "'; $f.Width = 520; $f.Height = 320; $l1 = New-Object System.Windows.Forms.Label; $l1.Text = 'Customer'; $l1.Left = 20; "
        "$l1.Top = 24; $c = New-Object System.Windows.Forms.TextBox; $c.AccessibleName = 'Customer'; $c.Left = 140; $c.Top = 20; "
        "$c.Width = 240; $l2 = New-Object System.Windows.Forms.Label; $l2.Text = 'Quantity'; $l2.Left = 20; $l2.Top = 64; "
        "$q = New-Object System.Windows.Forms.TextBox; $q.AccessibleName = 'Quantity'; $q.Left = 140; $q.Top = 60; $q.Width = 80; "
        "$s = New-Object System.Windows.Forms.Button; $s.Text = 'Save draft'; $s.Left = 140; $s.Top = 110; $s.Width = 110; "
        "$s.Add_Click({ $f.Text = 'Saved: ' + $c.Text + ' / ' + $q.Text }); $o = New-Object System.Windows.Forms.Button; "
        "$o.Text = 'Submit order'; $o.Left = 270; $o.Top = 110; $o.Width = 110; $o.Add_Click({ $f.Text = 'Submitted: ' + $c.Text }); "
        "$f.Controls.AddRange(@($l1, $c, $l2, $q, $s, $o)); [void]$f.ShowDialog()";
    std::wstring cmd = L"pwsh.exe -NoProfile -NonInteractive -STA -Command \"" + widen(script) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        GTEST_SKIP() << "PowerShell 7 (pwsh.exe) is not available";
    struct Kill {
        PROCESS_INFORMATION& p;
        ~Kill() {
            ::TerminateProcess(p.hProcess, 0);
            ::CloseHandle(p.hProcess);
            ::CloseHandle(p.hThread);
        }
    } kill{pi};
    const auto form_title = [&pi]() {
        struct Find {
            DWORD pid;
            std::wstring title;
        } find{pi.dwProcessId, {}};
        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid != f->pid || !::IsWindowVisible(h)) return TRUE;
            wchar_t buf[256] = {};
            ::GetWindowTextW(h, buf, 256);
            f->title = buf;
            return f->title.empty();
        }, reinterpret_cast<LPARAM>(&find));
        return narrow(find.title);
    };
    for (int i = 0; i < 300 && form_title() != title; ++i) std::this_thread::sleep_for(100ms);
    ASSERT_EQ(form_title(), title);

    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());
    const auto until = [&a](const std::string& method, const json& params, const std::function<bool(const json&)>& pred, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            if (auto r = a.call(method, params)) {
                last = *r;
                if (pred(last)) return last;
            }
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    json st = until("app.status", json::object(), [](const json& s) { return s.contains("agent") && jstr(s["agent"], "start_blocked").empty(); }, 180);
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
    json ev = until("policy.evaluate", {{"tool", "computer.click"}, {"risk", "destructive"}}, [](const json& v) { return jstr(v, "decision") == "approve"; }, 60);
    ASSERT_EQ(jstr(ev, "decision"), "approve") << ev.dump();
    ev = until("policy.evaluate", {{"tool", "computer.click"}, {"risk", "external"}}, [](const json& v) { return jstr(v, "decision") == "allow"; }, 10);
    ASSERT_EQ(jstr(ev, "decision"), "allow") << ev.dump();

    const auto settle = [&](const std::string& run, int seconds, std::atomic<bool>* overlay_seen = nullptr) {
        return until("runs.get", {{"run_id", run}}, [&](const json& x) {
            if (overlay_seen && ::FindWindowW(L"SkoworkComputerOverlay", nullptr)) *overlay_seen = true;
            const std::string s = jstr(x, "state");
            return s == "succeeded" || s == "failed" || s == "cancelled" || s == "interrupted";
        }, seconds);
    };

    // Fill and save: external actions, no approval; then submit: destructive, approved by the user.
    auto sent = a.call("threads.send",
                       {{"text", "Use the computer tools (computer.*) to work in the open desktop window titled \"" + title +
                                     "\". Type Acme Auto into the Customer field and 12 into the Quantity field, click Save draft, "
                                     "and then click Submit order. Do not ask me anything first."}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    std::atomic<bool> overlay_seen{false};
    json r = settle(run, 300, &overlay_seen);
    ASSERT_EQ(jstr(r, "state"), "interrupted") << r.dump() << " window: " << form_title();
    EXPECT_EQ(form_title(), "Saved: Acme Auto / 12") << "saving ran without approval";
    EXPECT_TRUE(overlay_seen.load()) << "the overlay was up while the agent acted";
    auto feed = a.call("threads.feed", {{"thread_id", thread}});
    ASSERT_TRUE(feed);
    json approval;
    for (const auto& x : (*feed)["actions"])
        if (jstr(x, "kind") == "approval" && x["resolution"].is_null()) approval = x;
    ASSERT_FALSE(approval.is_null()) << feed->dump();
    EXPECT_EQ(jstr(approval["payload"], "tool"), "computer.click") << approval.dump();
    EXPECT_EQ(jstr(approval["payload"], "risk"), "destructive") << approval.dump();
    bool window_shown = false;
    for (const auto& p : approval["payload"]["untrusted_context"])
        window_shown |= jstr(p, "source_kind") == "computer" && jstr(p, "source_id").find("pwsh") != std::string::npos;
    EXPECT_TRUE(window_shown) << "the Approval shows the window as provenance: " << approval.dump();
    ASSERT_TRUE(a.call("actions.resolve", {{"action_id", jstr(approval, "id")}, {"decision", "approve"}}));
    r = settle(run, 300, &overlay_seen);
    EXPECT_EQ(jstr(r, "state"), "succeeded") << r.dump();
    EXPECT_EQ(form_title(), "Submitted: Acme Auto");

    // The stop hotkey cancels a Run while the agent controls the desktop.
    auto second = a.call("threads.send",
                         {{"thread_id", thread},
                          {"text", "In the same window, replace the Customer field with Acme Auto Springfield, then the Quantity "
                                   "with 15, then click Save draft."}});
    ASSERT_TRUE(second) << second.error().message;
    const std::string run2 = jstr(*second, "run_id");
    bool pressed = false;
    for (int i = 0; i < 3000 && !pressed; ++i) {
        if (::FindWindowW(L"SkoworkComputerOverlay", nullptr)) {
            INPUT in[6] = {};
            const WORD vks[3] = {VK_CONTROL, VK_MENU, VK_END};
            for (int k = 0; k < 3; ++k) {
                in[k].type = in[5 - k].type = INPUT_KEYBOARD;
                in[k].ki.wVk = in[5 - k].ki.wVk = vks[k];
                in[5 - k].ki.dwFlags = KEYEVENTF_KEYUP;
            }
            in[2].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            in[3].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            ::SendInput(6, in, sizeof(INPUT));
            pressed = true;
        }
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(pressed) << "the agent never took control in the second Run";
    r = settle(run2, 30);
    EXPECT_EQ(jstr(r, "state"), "cancelled") << r.dump();
    for (int i = 0; i < 50 && ::FindWindowW(L"SkoworkComputerOverlay", nullptr); ++i) std::this_thread::sleep_for(100ms);
    EXPECT_EQ(::FindWindowW(L"SkoworkComputerOverlay", nullptr), nullptr) << "the overlay is gone after the stop";

    // Telemetry: each computer tool call with its outcome, metadata only.
    json view = until("telemetry.view", json::object(), [&run](const json& v) {
        for (const auto& x : v["records"])
            if (jstr(x, "event") == "tool.call" && jstr(x["attrs"], "run.id") == run && jstr(x["attrs"], "tool.id") == "computer.click")
                return true;
        return false;
    }, 30);
    std::map<std::string, std::multiset<std::string>> outcomes;
    for (const auto& x : view["records"])
        if (jstr(x, "event") == "tool.call" && jstr(x["attrs"], "run.id") == run)
            outcomes[jstr(x["attrs"], "tool.id")].insert(jstr(x["attrs"], "tool.outcome"));
    EXPECT_TRUE(outcomes.count("computer.click")) << view.dump();
    EXPECT_TRUE(outcomes.count("computer.type") || outcomes.count("computer.observe")) << view.dump();
    std::string listed;
    for (const auto& [tool, o] : outcomes)
        for (const auto& x : o) listed += " " + tool + "=" + x;
    std::printf("[ computer e2e ] tool outcomes:%s\n", listed.c_str());
    EXPECT_EQ(view.dump().find("Acme Auto"), std::string::npos) << "no content at the metadata level";
}

// add-computer-use 2.5: a custom-drawn application (no controls in the accessibility tree, everything painted) operated
// by the real model through vision alone. The order code exists only as pixels; the Confirm button is a painted
// rectangle. The model must take a screenshot, read the code through the vision role and click the right pixels.
TEST(AppE2E, ComputerUseOperatesACustomDrawnApplicationThroughVision) {
    SKOWORK_REQUIRE_STACK();
    auto admin = admin_token();
    ASSERT_TRUE(admin) << admin.error().message;
    const std::string user = user_name_of(*admin);
    const std::string group_name = "e2e-computer";
    const std::string group = "/api/v1/admin/policy/groups/" + group_name;
    struct Cleanup {
        std::function<void()> f;
        ~Cleanup() { f(); }
    } cleanup{[&] { (void)cp_call("DELETE", "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", group_name}}); }};
    auto draft = cp_call("PUT", group + "/draft", *admin,
                         {{"document", {{"tools", {{"computer", {{"mode", "allow"}, {"perception", "tree_and_vision"}}},
                                                   {"risk", {{"read", "allow"}, {"external", "allow"}, {"destructive", "approve"}}}}}}},
                          {"priority", 0},
                          {"reason", "e2e 2.5 (add-computer-use)"}});
    ASSERT_EQ(draft.first, 200) << draft.second.dump();
    ASSERT_EQ(cp_call("POST", group + "/publish", *admin, {{"reason", "e2e 2.5 (add-computer-use)"}}).first, 200);
    (void)cp_call("POST", "/api/v1/admin/identity/group-members", *admin, {{"user_name", user}, {"skowork_group", group_name}});

    const std::string title = "Dispatch board " + random_urlsafe(5);
    const int code = 1000 + static_cast<int>(random_bytes(2)[0]) * 31 % 9000;
    // Everything is painted on the form itself; WinForms exposes the form as one empty pane.
    const std::string script =
        "Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing; $f = New-Object System.Windows.Forms.Form; "
        "$f.Text = '" + title + "'; $f.Width = 640; $f.Height = 400; $f.BackColor = 'White'; $f.StartPosition = 'Manual'; $f.Left = 200; "
        "$f.Top = 160; $btn = New-Object System.Drawing.Rectangle 380, 240, 180, 70; $f.Add_Paint({ param($s, $e) "
        "$font = New-Object System.Drawing.Font('Segoe UI', 26); $e.Graphics.DrawString('Order code: " + std::to_string(code) +
        "', $font, [System.Drawing.Brushes]::Black, 40, 60); $e.Graphics.FillRectangle([System.Drawing.Brushes]::ForestGreen, $btn); "
        "$small = New-Object System.Drawing.Font('Segoe UI', 18); $e.Graphics.DrawString('Confirm', $small, [System.Drawing.Brushes]::White, "
        "415, 258) }); $f.Add_MouseClick({ param($s, $e) if ($btn.Contains($e.Location)) { $f.Text = 'Confirmed' } else { $f.Text = "
        "'Missed ' + $e.X + ',' + $e.Y } }); [void]$f.ShowDialog()";
    std::wstring cmd = L"pwsh.exe -NoProfile -NonInteractive -STA -Command \"" + widen(script) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        GTEST_SKIP() << "PowerShell 7 (pwsh.exe) is not available";
    struct Kill {
        PROCESS_INFORMATION& p;
        ~Kill() {
            ::TerminateProcess(p.hProcess, 0);
            ::CloseHandle(p.hProcess);
            ::CloseHandle(p.hThread);
        }
    } kill{pi};
    const auto form_title = [&pi]() {
        struct Find {
            DWORD pid;
            std::wstring title;
        } find{pi.dwProcessId, {}};
        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid != f->pid || !::IsWindowVisible(h)) return TRUE;
            wchar_t buf[256] = {};
            ::GetWindowTextW(h, buf, 256);
            f->title = buf;
            return f->title.empty();
        }, reinterpret_cast<LPARAM>(&find));
        return narrow(find.title);
    };
    for (int i = 0; i < 300 && form_title() != title; ++i) std::this_thread::sleep_for(100ms);
    ASSERT_EQ(form_title(), title);

    Instance a;
    a.write_config(stack_config());
    ASSERT_TRUE(a.start());
    const auto until = [&a](const std::string& method, const json& params, const std::function<bool(const json&)>& pred, int seconds) {
        json last;
        for (int i = 0; i < seconds * 10; ++i) {
            if (auto r = a.call(method, params)) {
                last = *r;
                if (pred(last)) return last;
            }
            std::this_thread::sleep_for(100ms);
        }
        return last;
    };
    json st = until("app.status", json::object(), [](const json& s) { return s.contains("agent") && jstr(s["agent"], "start_blocked").empty(); }, 180);
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
    json ev = until("policy.evaluate", {{"tool", "computer.click"}, {"risk", "external"}}, [](const json& v) { return jstr(v, "decision") == "allow"; }, 60);
    ASSERT_EQ(jstr(ev, "decision"), "allow") << ev.dump();

    auto sent = a.call("threads.send", {{"text", "Use the computer tools (computer.*) on the open desktop window titled \"" + title +
                                                     "\". Tell me the order code shown in it, and click its green Confirm button. "
                                                     "Do not ask me anything first."}});
    ASSERT_TRUE(sent) << sent.error().message;
    const std::string run = jstr(*sent, "run_id"), thread = jstr(*sent, "thread_id");
    json r = until("runs.get", {{"run_id", run}}, [](const json& x) {
        const std::string s = jstr(x, "state");
        return s == "succeeded" || s == "failed" || s == "cancelled" || s == "interrupted";
    }, 300);
    auto events = a.call("runs.events", {{"run_id", run}});
    ASSERT_TRUE(events);
    std::vector<std::string> calls;
    for (const auto& e : (*events)["events"])
        if (jstr(e, "kind") == "tool_call") calls.push_back(jstr(e["payload"], "tool") + (e["payload"].contains("args") ? " " + e["payload"]["args"].dump() : ""));
        else if (jstr(e, "kind") == "tool_result") calls.push_back("  -> " + utf8_truncate(jstr(e["payload"], "content"), 300));
    std::string listed;
    for (const auto& c : calls) listed += "\n  " + c;
    std::printf("[ computer vision e2e ] state=%s window=%s calls:%s\n", jstr(r, "state").c_str(), form_title().c_str(), listed.c_str());
    if (jstr(r, "state") == "interrupted")
        if (auto f = a.call("threads.feed", {{"thread_id", thread}}))
            for (const auto& x : (*f)["actions"]) std::printf("  pending: %s\n", x.dump().c_str());
    if (jstr(r, "state") == "interrupted") {
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(a.dir, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != L".log") continue;
            std::ifstream in(e.path());
            for (std::string line; std::getline(in, line);)
                if (line.find("user input:") != std::string::npos) std::printf("  log: %s\n", line.c_str());
        }
    }
    ASSERT_EQ(jstr(r, "state"), "succeeded") << r.dump();
    EXPECT_NE(listed.find("computer.screenshot"), std::string::npos) << "the model looked at the window";
    EXPECT_EQ(form_title(), "Confirmed") << "the click landed on the painted button";
    auto feed = a.call("threads.get", {{"thread_id", thread}});
    ASSERT_TRUE(feed);
    EXPECT_NE(feed->dump().find(std::to_string(code)), std::string::npos) << "the model read the code from the pixels: " << feed->dump();
}

// Model evaluation against the real gateway (not a regression test; run by hand): the agent builds three Local apps of
// increasing complexity, and answers six chat requests with generated surfaces enabled, including a form submitted back
// and a generated Question answered. Everything lands in SKOWORK_E2E_REPORT (default %TEMP%\skowork-e2e-report) for
// review. With SKOWORK_E2E_HOLD set, the client keeps running until <report>\release exists (up to 30 minutes), so the
// apps can be opened from dev.opened_urls. App launches are recorded, never opened in the user's browser.
TEST(AppE2E, ModelBuildsLocalAppsAndUsesGeneratedSurfaces) {
    SKOWORK_REQUIRE_STACK();
    if (!std::getenv("SKOWORK_E2E_MODEL_EVAL")) GTEST_SKIP() << "set SKOWORK_E2E_MODEL_EVAL to run the model evaluation";
    const char* rep = std::getenv("SKOWORK_E2E_REPORT");
    const std::filesystem::path report = rep ? std::filesystem::path(rep) : std::filesystem::temp_directory_path() / L"skowork-e2e-report";
    std::filesystem::create_directories(report);
    const auto save = [&report](const std::string& name, const std::string& text) { std::ofstream(report / widen(name), std::ios::binary) << text; };

    Instance a;
    // SKOWORK_E2E_KEEP_ROOT: reuse one data root across evaluation runs, so its refresh token spares the next sign-in.
    if (const char* keep = std::getenv("SKOWORK_E2E_KEEP_ROOT")) {
        a.dir = std::filesystem::path(keep);
        a.keep = true;
    }
    json cfg = stack_config();
    cfg["dev_open_urls"] = "record";
    a.write_config(cfg);
    ASSERT_TRUE(a.start());
    json st;
    for (int i = 0; i < 1800; ++i) {
        if (auto s = a.call("app.status")) {
            st = *s;
            if (st.contains("agent") && jstr(st["agent"], "start_blocked").empty()) break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();
    ASSERT_TRUE(a.call("settings.set", {{"key", "ui.generated_surfaces"}, {"value", true}}));
    // The setting applies to the agent on restart.
    ASSERT_TRUE(a.call("app.quit"));
    ASSERT_TRUE(exited_within(a.main, 20000));
    a.main = {};
    ASSERT_TRUE(a.start());
    for (int i = 0; i < 1800; ++i) {
        if (auto s = a.call("app.status")) {
            st = *s;
            if (st.contains("agent") && jstr(st["agent"], "start_blocked").empty()) break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(jstr(st["agent"], "start_blocked").empty()) << st.dump();

    json summary = json::object();
    // Drives the Thread until no Run is active: approves every approval, answers plain Questions with their first
    // choice, and answers generated Questions with their defaults through the window's method.
    const auto drive = [&a](const std::string& thread, int minutes, json& log) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
        int idle = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            auto t = a.call("threads.get", {{"thread_id", thread}});
            if (!t) { std::this_thread::sleep_for(500ms); continue; }
            bool active = false;
            for (const auto& r : (*t)["runs"]) {
                const std::string s = jstr(r, "state");
                if (s != "succeeded" && s != "failed" && s != "cancelled") active = true;
            }
            for (const auto& x : (*t)["actions"]) {
                if (!x["resolution"].is_null()) continue;
                const std::string id = jstr(x, "id"), kind = jstr(x, "kind");
                const json& p = x["payload"];
                if (kind == "approval") {
                    log["approvals"].push_back(jstr(p, "tool"));
                    (void)a.call("actions.resolve", {{"action_id", id}, {"decision", "approve"}});
                } else if (kind == "question" && p.contains("surface")) {
                    std::string button;
                    std::function<void(const json&)> find = [&](const json& cs) {
                        for (const auto& c : cs) {
                            if (jstr(c, "type") == "button" && button.empty()) button = jstr(c, "id");
                            if (c.contains("components")) find(c["components"]);
                        }
                    };
                    find(p["surface"]["components"]);
                    auto r = a.call("dev.call", {{"method", "surfaces.answer_question"},
                                                 {"params", {{"action_id", id}, {"thread_id", thread}, {"run_id", jstr(x, "run_id")},
                                                             {"revision", jstr(p, "surface_revision")}, {"token", jstr(p, "surface_token")},
                                                             {"button_id", button}, {"draft", json::object()}}}});
                    log["generated_questions"].push_back({{"surface", p["surface"]}, {"answered", r.ok()}, {"error", r ? "" : r.error().message}});
                    if (!r) (void)a.call("actions.answer", {{"action_id", id}, {"answer", "Use your best judgment."}});
                } else if (kind == "question") {
                    std::string answer = "Use your best judgment.";
                    if (const json* c = jobj(p, "choices"); c && c->is_array() && !c->empty()) answer = (*c)[0].get<std::string>();
                    log["questions"].push_back({{"prompt", jstr(p, "prompt")}, {"answer", answer}});
                    (void)a.call("actions.answer", {{"action_id", id}, {"answer", answer}});
                }
            }
            if (!active && (*t)["queued"].empty()) {
                if (++idle >= 3) return *t;
            } else {
                idle = 0;
            }
            std::this_thread::sleep_for(1s);
        }
        auto t = a.call("threads.get", {{"thread_id", thread}});
        return t ? *t : json::object();
    };
    const auto tools_of = [&a](const json& thread) {
        json tools = json::array();
        for (const auto& r : thread["runs"]) {
            auto ev = a.call("runs.events", {{"run_id", jstr(r, "id")}});
            if (!ev) continue;
            for (const auto& e : (*ev)["events"])
                if (jstr(e, "kind") == "tool_call" || jstr(e, "kind") == "tool.call") tools.push_back(jstr(e["payload"], "tool"));
        }
        return tools;
    };
    const auto last_assistant = [](const json& thread) {
        std::string text;
        for (const auto& m : thread["messages"]) if (jstr(m, "role") == "assistant" && !jstr(m, "content").empty()) text = jstr(m, "content");
        return text;
    };

    // ---- Local apps ----
    const std::vector<std::pair<std::string, std::string>> apps_prompts = {
        {"app1-presentation",
         "Build me a Local app: a 6-slide presentation for a fleet manager about moving a company car fleet to electric cars "
         "(costs, charging, range, incentives, a rollout plan, next steps). Next/previous buttons, arrow-key navigation, a "
         "slide counter and a clean modern look. Do not ask me anything first; open it when it is done."},
        {"app2-reading-list",
         "Build me a Local app to track my reading list. I add books with title, author, status (to read, reading, done) and "
         "a 1-5 rating; I can see the list, filter it by status, change a book's status and delete a book. Keep the books in "
         "the app's database so they survive a restart. Do not ask me anything first; open it when it is done."},
        {"app3-team-expenses",
         "Build me a Local app for my team's expenses. Each expense has a date, a person, a category, an amount in CZK and a "
         "note, stored in the app's database. I can add, edit and delete expenses; filter by person, category and month; see a "
         "summary with totals per category and per person and a bar chart of the category totals; and export the filtered "
         "list as CSV. Seed it with 12 realistic sample expenses for 4 people over the last 3 months. Do not ask me anything "
         "first; open it when it is done."}};
    // SKOWORK_E2E_ONLY: a comma-separated list of scenario names (for example app3-team-expenses) to run alone.
    const char* only = std::getenv("SKOWORK_E2E_ONLY");
    const auto wanted = [only](const std::string& name) { return !only || (std::string(",") + only + ",").find("," + name + ",") != std::string::npos; };
    for (const auto& [name, prompt] : apps_prompts) {
        if (!wanted(name)) continue;
        const auto started = std::chrono::steady_clock::now();
        auto sent = a.call("threads.send", {{"text", prompt}});
        ASSERT_TRUE(sent) << sent.error().message;
        const std::string thread = jstr(*sent, "thread_id");
        json log = {{"prompt", prompt}, {"approvals", json::array()}, {"questions", json::array()}};
        // One model turn of a large app can take 15 minutes on the dev gateway (about 25 tokens per second).
        const json t = drive(thread, 45, log);
        log["seconds"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        log["runs"] = json::array();
        for (const auto& r : t["runs"]) log["runs"].push_back({{"id", jstr(r, "id")}, {"state", jstr(r, "state")}, {"error", r.contains("error") ? r["error"] : json()}});
        log["tools"] = tools_of(t);
        log["answer"] = last_assistant(t);
        save(name + ".thread.json", t.dump(2));
        summary[name] = log;
        save("summary.json", summary.dump(2));
        EXPECT_TRUE(!t["runs"].empty() && jstr(t["runs"].back(), "state") == "succeeded") << name << ": " << log["runs"].dump();
        EXPECT_NE(log["tools"].dump().find("apps.open"), std::string::npos) << name << " was not opened";
    }
    auto listed = a.call("dev.call", {{"method", "apps.list"}});
    ASSERT_TRUE(listed) << listed.error().message;
    summary["apps"] = *listed;
    for (const auto& app : (*listed)["apps"]) {
        std::error_code ec;
        const auto dest = report / L"apps" / widen(jstr(app, "id"));
        std::filesystem::create_directories(dest, ec);
        std::filesystem::copy(widen(jstr(app, "folder")), dest,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (auto urls = a.call("dev.opened_urls")) summary["opened_urls"] = (*urls)["urls"];
    save("summary.json", summary.dump(2));
    if (!only) EXPECT_GE((*listed)["apps"].size(), 3u) << listed->dump(2);

    // ---- Generated surfaces ----
    const std::vector<std::pair<std::string, std::string>> ui_prompts = {
        {"ui1-chart-table",
         "Our support tickets per month: Jan 120, Feb 135, Mar 160, Apr 150, May 190, Jun 210. Resolved within SLA: Jan 95%, "
         "Feb 93%, Mar 88%, Apr 91%, May 85%, Jun 82%. Show me the trend and what stands out."},
        {"ui2-form",
         "I am planning a 2-day offsite for 12 people. Let me set the budget per person, the type of location and the "
         "activities I want, and then come back with a plan that fits my choices."},
        {"ui3-diagram",
         "Explain how a change gets from a developer's branch to production in a typical pipeline with code review, build, "
         "automated tests, a staging deployment, a manual approval and the production rollout. Visualize it."},
        {"ui4-question",
         "Help me choose a laptop for work. Ask me what matters to me before you recommend anything."},
        {"ui5-plain", "What is the capital of Austria?"},
        {"ui6-comparison",
         "Compare PostgreSQL, SQLite and MongoDB for a small internal tool on licensing, setup effort, concurrent writes and "
         "backups. I need to decide quickly."}};
    for (const auto& [name, prompt] : ui_prompts) {
        if (!wanted(name)) continue;
        const auto started = std::chrono::steady_clock::now();
        auto sent = a.call("threads.send", {{"text", prompt}});
        ASSERT_TRUE(sent) << sent.error().message;
        const std::string thread = jstr(*sent, "thread_id");
        json log = {{"prompt", prompt}, {"approvals", json::array()}, {"questions", json::array()}, {"generated_questions", json::array()}};
        json t = drive(thread, 10, log);
        log["seconds"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        log["answer"] = last_assistant(t);
        // Every assistant message's surfaces: which parse, which were dropped.
        json surfaces = json::array();
        long long seq = 0;
        std::string run_id;
        int block_count = 0;
        for (const auto& r : t["runs"]) {
            auto ev = a.call("runs.events", {{"run_id", jstr(r, "id")}});
            if (!ev) continue;
            for (const auto& e : (*ev)["events"]) {
                if (jstr(e, "kind") != "message" || jstr(e["payload"], "role") != "assistant") continue;
                const std::string content = jstr(e["payload"], "content");
                const auto blocks = ui::discover_generated_surfaces(content);
                size_t fences = 0;
                for (size_t p = content.find("```skowork-ui"); p != std::string::npos; p = content.find("```skowork-ui", p + 1)) ++fences;
                for (const auto& b : blocks) surfaces.push_back(b.surface);
                if (fences != blocks.size()) log["invalid_fences"] = static_cast<long long>(fences - blocks.size());
                if (!blocks.empty()) {
                    seq = jint(e, "seq");
                    run_id = jstr(r, "id");
                    block_count = static_cast<int>(blocks.size());
                }
            }
        }
        log["surfaces"] = surfaces;
        // The form: submit it with changed values, as the user would, and record the follow-up answer.
        if (name == "ui2-form" && !run_id.empty()) {
            for (int idx = 0; idx < block_count; ++idx) {
                const json& s = surfaces[surfaces.size() - static_cast<size_t>(block_count) + static_cast<size_t>(idx)];
                json draft = json::object();
                std::string button;
                std::function<void(const json&)> walk = [&](const json& cs) {
                    for (const auto& c : cs) {
                        const std::string type = jstr(c, "type"), id = jstr(c, "id");
                        if (type == "slider") draft[id] = c["max"].get<double>() * 0.6 + c["min"].get<double>() * 0.4;
                        else if (type == "select" && !c["options"].empty()) draft[id] = c["options"].back();
                        else if (type == "multiselect" && c["options"].size() >= 2) draft[id] = json::array({c["options"][0], c["options"][1]});
                        else if (type == "switch") draft[id] = !jbool(c, "value");
                        else if (type == "button" && button.empty()) button = id;
                        if (c.contains("components")) walk(c["components"]);
                    }
                };
                walk(s["components"]);
                if (button.empty()) continue;
                auto opened = a.call("dev.call", {{"method", "surfaces.open"},
                                                  {"params", {{"thread_id", thread}, {"run_id", run_id}, {"event_seq", seq}, {"block_index", idx}}}});
                if (!opened) { log["submit_error"] = opened.error().message; break; }
                auto submitted = a.call("dev.call", {{"method", "surfaces.submit"},
                                                     {"params", {{"thread_id", thread}, {"run_id", run_id}, {"event_seq", seq}, {"block_index", idx},
                                                                 {"revision", jstr(*opened, "revision")}, {"button_id", button}, {"draft", draft}}}});
                log["submitted"] = {{"draft", draft}, {"button", button}, {"ok", submitted.ok()}, {"error", submitted ? "" : submitted.error().message}};
                if (submitted) {
                    std::this_thread::sleep_for(2s);
                    t = drive(thread, 10, log);
                    log["follow_up"] = last_assistant(t);
                    const auto& msgs = t["messages"];
                    for (const auto& m : msgs) if (jstr(m, "role") == "user") log["submitted_message"] = jstr(m, "content");
                }
                break;
            }
        }
        save(name + ".thread.json", t.dump(2));
        summary[name] = log;
        save("summary.json", summary.dump(2));
    }

    if (std::getenv("SKOWORK_E2E_HOLD")) {
        std::printf("[ hold ] the client is running; create %s to finish\n", (report / L"release").string().c_str());
        for (int i = 0; i < 1800 && !std::filesystem::exists(report / L"release"); ++i) std::this_thread::sleep_for(1s);
    }
}
