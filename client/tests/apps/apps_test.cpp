// client/local-apps (add-local-apps 2.1-2.4): the manifest and frames, the app manager on the pinned Node.js runtime in
// the tool sandbox, the loopback listener (launch token, session cookie, Host check), the MCP bridge, and the apps.*
// tools in a Run under client/policy-enforcement. Tests that run apps need client/runtime/node (fetched by
// dev/scripts/fetch-node-runtime.ps1) and skip without it.
#include "apps/apps.h"
#include "base/strings.h"
#include "sandbox/proxy.h"
#include "sandbox/sandbox.h"
#include "support/runtime_harness.h"
#include "tools/files.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

using namespace wb;
using namespace wb::apps;
namespace fs = std::filesystem;

namespace {

fs::path runtime_dir() { return fs::path(WORKBENCH_SOURCE_DIR) / "runtime" / "node"; }

bool node_present() {
    std::error_code ec;
    return fs::exists(runtime_dir() / "node.exe", ec);
}

std::shared_ptr<const policy::EffectivePolicy> policy_with(json apps) {
    policy::PolicyBundle b;
    b.sub = "user";
    b.ver = 1;
    b.exp = 4102444800;
    b.policy = {{"tools", {{"risk", {{"read", "allow"}, {"write-local", "allow"}, {"external", "allow"}, {"destructive", "allow"}}}}}};
    if (!apps.is_null()) b.policy["apps"] = std::move(apps);
    return std::make_shared<const policy::EffectivePolicy>(b);
}

// The app runtime of the tests: node.exe from client/runtime/node and the app host from the sources.
struct Rig {
    std::chrono::milliseconds start_timeout_;
    TempRoot tmp;
    fs::path rt = tmp.path / "runtime";
    sandbox::EgressProxy proxy{[](const std::string&) { return policy::RuleVerdict{false, "test", "no network in tests"}; },
                               [](const sandbox::EgressProxy::Owner&, const std::string&, const policy::RuleVerdict&) {}};
    sandbox::Sandbox sb{&proxy};
    std::shared_ptr<const policy::EffectivePolicy> pol = policy_with(nullptr);
    std::atomic<long long> clock{1'800'000'000'000};
    std::mutex mx;
    std::map<std::string, runtime::Risk> risks;
    std::vector<std::pair<std::string, json>> mcp_calls;
    std::unique_ptr<AppManager> apps;
    std::unique_ptr<AppServer> server;

    explicit Rig(std::chrono::milliseconds start_timeout = std::chrono::seconds(10)) : start_timeout_(start_timeout) {
        std::error_code ec;
        fs::create_directories(rt, ec);
        if (node_present()) fs::copy_file(runtime_dir() / "node.exe", rt / "node.exe", ec);
        fs::copy_file(fs::path(WORKBENCH_SOURCE_DIR) / "src" / "apps" / "app-host.js", rt / "app-host.js", ec);
        AppsEnv e;
        e.root = tmp.path / "apps";
        e.state_dir = tmp.path / "state";
        e.runtime_dir = rt;
        e.sandbox = &sb;
        e.policy = [this] { return pol; };
        e.now_ms = [this] { return clock.load(); };
        e.tool_risk = [this](const std::string& id) -> std::optional<runtime::Risk> {
            const std::lock_guard lock(mx);
            const auto it = risks.find(id);
            if (it == risks.end()) return std::nullopt;
            return it->second;
        };
        e.call_tool = [this](const std::string& app, const std::string& id, const json& args) -> Result<json> {
            const std::lock_guard lock(mx);
            mcp_calls.emplace_back(app + " " + id, args);
            return json{{"issues", {{{"key", "DEMO-1"}, {"query", jstr(args, "query")}}}}};
        };
        e.run_active = [](const std::string& run) { return run != "ended"; };
        e.start_timeout = start_timeout_;
        apps = std::make_unique<AppManager>(std::move(e));
        server = std::make_unique<AppServer>(*apps);
    }
    ~Rig() {
        server.reset();
        apps.reset();
    }
    void write(const std::string& id, const std::string& rel, const std::string& text) {
        std::ofstream f(tmp.path / "apps" / widen(id) / widen(rel), std::ios::binary | std::ios::trunc);
        f << text;
    }
};

// A raw HTTP request to the loopback listener; the whole response (the server closes the connection).
std::string http(int port, const std::string& request) {
    WSADATA w;
    ::WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        ::send(s, request.data(), static_cast<int>(request.size()), 0);
        char buf[8192];
        for (int n; (n = ::recv(s, buf, sizeof buf, 0)) > 0;) out.append(buf, static_cast<size_t>(n));
    }
    ::closesocket(s);
    ::WSACleanup();
    return out;
}

std::string get(int port, const std::string& path, const std::string& extra = {}, const std::string& host = {}) {
    return http(port, "GET " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) + "\r\n" + extra + "\r\n");
}

std::string post_json(int port, const std::string& path, const std::string& body, const std::string& extra) {
    return http(port, "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body);
}

int status_of(const std::string& response) { return response.size() > 12 ? std::atoi(response.substr(9, 3).c_str()) : 0; }

std::string header_of(const std::string& response, const std::string& name) {
    for (const auto& line : split(response.substr(0, response.find("\r\n\r\n")), '\n')) {
        const size_t c = line.find(':');
        if (c != std::string::npos && iequals(trim(line.substr(0, c)), name)) return trim(line.substr(c + 1));
    }
    return {};
}

std::string body_of(const std::string& response) {
    const size_t at = response.find("\r\n\r\n");
    return at == std::string::npos ? std::string{} : response.substr(at + 4);
}

// Opens the app's session: the launch URL's path, then the cookie from the redirect.
std::string session_cookie(Rig& r, const std::string& id) {
    auto url = r.server->launch_url(id);
    EXPECT_TRUE(url);
    const std::string path = url->substr(url->find('/', 7));
    const std::string resp = get(r.server->port(), path);
    EXPECT_EQ(status_of(resp), 303) << resp;
    const std::string set = header_of(resp, "Set-Cookie");
    return set.substr(0, set.find(';'));
}

Manifest manifest(const std::string& name, std::vector<std::string> tools = {}) {
    Manifest m;
    m.name = name;
    m.description = "A test app";
    m.mcp_tools = std::move(tools);
    return m;
}

}  // namespace

// ---- 2.1 Manifest, frames, dependency refusal ---------------------------------------------------------------------------

TEST(AppsCore, ManifestSlugHashFramesAndDependencies) {
    EXPECT_EQ(slug("Q3 Slides!"), "q3-slides");
    EXPECT_EQ(slug("Sprint Board – Übersicht"), "sprint-board-ubersicht");
    EXPECT_EQ(slug("***"), "app");
    EXPECT_LE(slug(std::string(100, 'a')).size(), 40u);
    EXPECT_TRUE(parse_manifest({{"name", "x"}, {"entry", "server.js"}, {"mcp_tools", {"mcp:jira/search_issues"}}}));
    EXPECT_FALSE(parse_manifest({{"name", ""}}));
    EXPECT_FALSE(parse_manifest({{"name", "x"}, {"entry", "..\\evil.js"}}));
    EXPECT_FALSE(parse_manifest({{"name", "x"}, {"entry", "C:\\x.js"}}));
    EXPECT_FALSE(parse_manifest({{"name", "x"}, {"entry", "index.html"}}));
    EXPECT_FALSE(parse_manifest({{"name", "x"}, {"mcp_tools", {"files.read"}}}));
    EXPECT_EQ(parse_manifest({{"name", "x"}, {"dependencies", {{"express", "4"}}}}).error().code, "dependencies");
    EXPECT_EQ(tools_hash({"mcp:a/x", "mcp:b/y"}), tools_hash({"mcp:b/y", "mcp:a/x", "mcp:a/x"}));
    EXPECT_NE(tools_hash({"mcp:a/x"}), tools_hash({"mcp:a/x", "mcp:b/y"}));

    FrameReader fr;
    const std::string bytes = encode_frame("data", 7, std::string("a\0b", 3)) + encode_frame("end", 7);
    fr.feed(bytes.substr(0, 5));
    EXPECT_FALSE(fr.next());
    fr.feed(bytes.substr(5));
    auto f = fr.next();
    ASSERT_TRUE(f);
    EXPECT_EQ(f->type, "data");
    EXPECT_EQ(f->id, 7);
    EXPECT_EQ(f->body, std::string("a\0b", 3));
    f = fr.next();
    ASSERT_TRUE(f);
    EXPECT_EQ(f->type, "end");
    FrameReader bad;
    bad.feed(std::string("\xff\xff\xff\x7f\0\0\0\0", 8));
    EXPECT_FALSE(bad.next());
    EXPECT_TRUE(bad.bad());

    TempRoot t;
    fs::create_directories(t.path);
    EXPECT_FALSE(dependency_problem(t.path));
    { std::ofstream(t.path / "package.json") << R"({"name":"x","dependencies":{}})"; }
    EXPECT_FALSE(dependency_problem(t.path));
    { std::ofstream(t.path / "package.json") << R"({"name":"x","devDependencies":{"jest":"1"}})"; }
    EXPECT_TRUE(dependency_problem(t.path));
    fs::remove(t.path / "package.json");
    fs::create_directories(t.path / "node_modules");
    EXPECT_TRUE(dependency_problem(t.path));
}

TEST(AppsManager, FoldersStarterFilesGrantsConfirmationAndDelete) {
    Rig r;
    auto a = r.apps->create(manifest("Sprint board", {"mcp:jira/search_issues"}), "run-1");
    ASSERT_TRUE(a) << a.error().message;
    auto b = r.apps->create(manifest("Sprint board"), "run-2");
    ASSERT_TRUE(b);
    EXPECT_EQ(a->id, "sprint-board");
    EXPECT_EQ(b->id, "sprint-board-2");
    EXPECT_NE(a->folder, b->folder);
    for (const char* f : {"app.json", "server.js", "public\\index.html"}) EXPECT_TRUE(fs::exists(fs::path(widen(a->folder)) / f)) << f;
    EXPECT_TRUE(a->needs_confirmation);
    EXPECT_FALSE(b->needs_confirmation);
    EXPECT_EQ(r.apps->list().size(), 2u);

    // Each Run may write only the folder it created; an ended Run none.
    EXPECT_EQ(r.apps->folders_for("run-1"), std::vector<std::string>{a->folder});
    EXPECT_EQ(r.apps->folders_for("run-2"), std::vector<std::string>{b->folder});
    EXPECT_EQ(r.apps->folders_for("").size(), 2u);
    ASSERT_TRUE(r.apps->update(a->id, {{"description", "Board"}}, "ended"));
    EXPECT_EQ(r.apps->folders_for("ended"), std::vector<std::string>{});

    // Confirmation, and again when the list changes.
    EXPECT_EQ(r.apps->start(a->id).error().code, node_present() ? "confirmation_needed" : "unavailable");
    ASSERT_TRUE(r.apps->confirm(a->id));
    EXPECT_FALSE(r.apps->get(a->id)->needs_confirmation);
    ASSERT_TRUE(r.apps->update(a->id, {{"mcp_tools", {"mcp:jira/search_issues", "mcp:jira/get_issue"}}}, "run-1"));
    EXPECT_TRUE(r.apps->get(a->id)->needs_confirmation);

    // Packages are refused before anything starts.
    r.write(b->id, "package.json", R"({"dependencies":{"express":"^4"}})");
    const auto info = r.apps->get(b->id);
    EXPECT_NE(info->problem.find("cannot use packages"), std::string::npos);
    if (node_present()) EXPECT_EQ(r.apps->start(b->id).error().code, "dependencies");

    ASSERT_TRUE(r.apps->remove(b->id));
    EXPECT_FALSE(fs::exists(fs::path(widen(b->folder))));
    EXPECT_FALSE(r.apps->get(b->id));
    EXPECT_EQ(r.apps->folders_for("run-2"), std::vector<std::string>{});
}

TEST(AppsManager, RunsContainedServesOverFramesStopsWhenIdleAndKeepsThree) {
    if (!node_present()) GTEST_SKIP() << "client/runtime/node is missing (dev/scripts/fetch-node-runtime.ps1)";
    Rig r;
    auto a = r.apps->create(manifest("Probe"), "run-1");
    ASSERT_TRUE(a);
    // A server that reports what the runtime allows: sqlite in its folder, a file outside, a child process, the network.
    r.write(a->id, "server.js", R"JS(
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');
const db = new DatabaseSync(path.join(__dirname, 'data.db'));
db.exec('CREATE TABLE IF NOT EXISTS hits (n INTEGER)');
const tryIt = (f) => { try { f(); return 'allowed'; } catch (e) { return 'denied ' + e.code; } };
http.createServer(async (req, res) => {
  db.prepare('INSERT INTO hits VALUES (1)').run();
  const out = { url: req.url, cookie: req.headers.cookie || '', hits: db.prepare('SELECT count(*) AS n FROM hits').get().n };
  if (req.url === '/probe') {
    out.read_outside = tryIt(() => fs.readFileSync('C:\\Windows\\win.ini'));
    out.child = tryIt(() => require('node:child_process').execSync('cmd /c echo hi'));
    out.fetch = await fetch('https://example.com/').then(() => 'reached', (e) => 'failed ' + (e.cause?.code || e.code));
    out.tcp = await new Promise((done) => { try { const s = require('node:net').connect({ host: '127.0.0.1', port: 9 });
      s.on('connect', () => done('connected')); s.on('error', (e) => done('failed ' + e.code)); } catch (e) { done('failed ' + e.code); } });
    out.rss_mb = Math.round(process.memoryUsage().rss / 1048576);
    console.log('probe done');
  }
  res.setHeader('content-type', 'application/json');
  res.end(JSON.stringify(out));
}).listen(process.env.WORKBENCH_APP_PIPE);
)JS");
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(r.apps->start(a->id)) << r.apps->logs(a->id);
    const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "[ measure  ] app start in the sandbox: " << start_ms << " ms\n";
    EXPECT_TRUE(r.apps->get(a->id)->running);

    const std::string cookie = session_cookie(r, a->id);
    const std::string resp = get(r.server->port(), "/" + a->id + "/probe", "Cookie: " + cookie + "; theme=dark\r\n");
    ASSERT_EQ(status_of(resp), 200) << resp;
    const auto j = parse_json(body_of(resp));
    ASSERT_TRUE(j) << resp;
    std::cout << "[ measure  ] app resident memory in the sandbox: " << jint(*j, "rss_mb") << " MB\n";
    EXPECT_EQ(jstr(*j, "url"), "/probe");                  // the /<app-id> prefix is removed
    EXPECT_EQ(jstr(*j, "cookie"), "theme=dark");           // the session cookie never reaches the app
    EXPECT_EQ(jint(*j, "hits"), 1);                        // node:sqlite in its own folder
    EXPECT_EQ(jstr(*j, "read_outside"), "denied ERR_ACCESS_DENIED");
    EXPECT_EQ(jstr(*j, "child"), "denied ERR_ACCESS_DENIED");
    EXPECT_EQ(jstr(*j, "fetch"), "failed ERR_ACCESS_DENIED");  // App tries the network: the request fails
    EXPECT_EQ(jstr(*j, "tcp"), "failed ERR_ACCESS_DENIED");
    // ...and the app keeps running.
    EXPECT_EQ(status_of(get(r.server->port(), "/" + a->id + "/again", "Cookie: " + cookie + "\r\n")), 200);
    EXPECT_NE(r.apps->logs(a->id).find("probe done"), std::string::npos);

    // Idle stop after 15 minutes without requests; the next request starts it again.
    r.clock += 14 * 60 * 1000;
    r.apps->tick();
    EXPECT_EQ(r.apps->running_count(), 1u);
    r.clock += 2 * 60 * 1000;
    r.apps->tick();
    EXPECT_EQ(r.apps->running_count(), 0u);
    EXPECT_NE(r.apps->logs(a->id).find("no request for 15 minutes"), std::string::npos);
    const std::string again = get(r.server->port(), "/" + a->id + "/", "Cookie: " + cookie + "\r\n");
    EXPECT_EQ(status_of(again), 200);
    EXPECT_EQ(jint(*parse_json(body_of(again)), "hits"), 3);  // the database outlived the process
    EXPECT_EQ(r.apps->running_count(), 1u);

    // At most three: starting a fourth stops the least recently used.
    std::vector<std::string> ids{a->id};
    for (const char* n : {"Two", "Three", "Four"}) {
        r.clock += 1000;
        auto x = r.apps->create(manifest(n), "run-1");
        ASSERT_TRUE(x);
        ASSERT_TRUE(r.apps->start(x->id)) << r.apps->logs(x->id);
        ids.push_back(x->id);
    }
    EXPECT_EQ(r.apps->running_count(), 3u);
    EXPECT_FALSE(r.apps->get(ids[0])->running);
    for (size_t i = 1; i < ids.size(); ++i) EXPECT_TRUE(r.apps->get(ids[i])->running) << ids[i];

    // An app that never listens is stopped with a hint.
    auto bad = r.apps->create(manifest("Silent"), "run-1");
    r.write(bad->id, "server.js", "console.log('no server here');");
    auto s = r.apps->start(bad->id);
    ASSERT_FALSE(s);
    EXPECT_NE(s.error().message.find("WORKBENCH_APP_PIPE"), std::string::npos);
    EXPECT_NE(s.error().message.find("no server here"), std::string::npos);
}

// ---- 2.2 Owner-only serving ----------------------------------------------------------------------------------------------

TEST(AppsServer, LaunchTokenIsSingleUseCookieIsScopedAndWrongHostRefused) {
    Rig r;
    auto a = r.apps->create(manifest("One"), "run-1");
    auto b = r.apps->create(manifest("Two"), "run-1");
    ASSERT_TRUE(a && b);
    auto url = r.server->launch_url(a->id);
    ASSERT_TRUE(url);
    EXPECT_EQ(url->rfind("http://127.0.0.1:" + std::to_string(r.server->port()) + "/one/?launch=", 0), 0u) << *url;
    const std::string path = url->substr(url->find('/', 7));
    const int port = r.server->port();

    // Wrong Host (DNS rebinding) is refused before anything else, and keeps the token.
    EXPECT_EQ(status_of(get(port, path, {}, "evil.example:" + std::to_string(port))), 403);
    // No cookie: refused.
    EXPECT_EQ(status_of(get(port, "/one/")), 403);
    // The token becomes a session cookie for /one/ only.
    const std::string first = get(port, path);
    ASSERT_EQ(status_of(first), 303) << first;
    EXPECT_EQ(header_of(first, "Location"), "/one/");
    const std::string set = header_of(first, "Set-Cookie");
    EXPECT_NE(set.find("Path=/one/"), std::string::npos);
    EXPECT_NE(set.find("HttpOnly"), std::string::npos);
    EXPECT_NE(set.find("SameSite=Strict"), std::string::npos);
    // Single use.
    EXPECT_EQ(status_of(get(port, path)), 403);
    // The cookie does not open another app, and a forged one opens nothing.
    const std::string cookie = set.substr(0, set.find(';'));
    const std::string value = cookie.substr(cookie.find('=') + 1);
    EXPECT_EQ(status_of(get(port, "/two/", "Cookie: workbench_app_two=" + value + "\r\n")), 403);
    EXPECT_EQ(status_of(get(port, "/one/", "Cookie: workbench_app_one=forged\r\n")), 403);
    // A token of one app does not open another.
    auto url2 = r.server->launch_url(a->id);
    const std::string p2 = url2->substr(url2->find('/', 7));
    EXPECT_EQ(status_of(get(port, "/two/?" + p2.substr(p2.find('?') + 1))), 403);
    // An expired token (two minutes) is refused.
    auto url3 = r.server->launch_url(b->id);
    EXPECT_TRUE(url3);
}

// ---- 2.3 MCP bridge ----------------------------------------------------------------------------------------------------

TEST(AppsBridge, ListedConfirmedReadToolsOnlyUnderPolicy) {
    Rig r;
    r.risks["mcp:jira/search_issues"] = runtime::Risk::read;
    r.risks["mcp:jira/create_issue"] = runtime::Risk::external;
    auto a = r.apps->create(manifest("Jira view", {"mcp:jira/search_issues", "mcp:jira/create_issue"}), "run-1");
    ASSERT_TRUE(a);
    const std::string cookie = session_cookie(r, a->id);
    const int port = r.server->port();
    const auto call = [&](const std::string& tool, const std::string& extra = {}) {
        return post_json(port, "/" + a->id + "/_workbench/mcp/" + tool, R"({"query":"project = DEMO"})", "Cookie: " + cookie + "\r\n" + extra);
    };
    const auto code = [](const std::string& resp) { return jstr(*jobj(*parse_json(body_of(resp)), "error"), "code"); };

    // Not confirmed yet.
    std::string resp = call("jira/search_issues");
    EXPECT_EQ(status_of(resp), 403);
    EXPECT_EQ(code(resp), "not_confirmed");
    ASSERT_TRUE(r.apps->confirm(a->id));
    // Live MCP view: called as the user with the app as origin, the result as JSON.
    resp = call("jira/search_issues");
    ASSERT_EQ(status_of(resp), 200) << resp;
    EXPECT_EQ((*parse_json(body_of(resp)))["result"]["issues"][0]["query"], "project = DEMO");
    ASSERT_EQ(r.mcp_calls.size(), 1u);
    EXPECT_EQ(r.mcp_calls[0].first, "jira-view mcp:jira/search_issues");
    // Unlisted tool, and a listed tool that is not read-risk.
    resp = call("jira/get_issue");
    EXPECT_EQ(status_of(resp), 403);
    EXPECT_EQ(code(resp), "not_listed");
    resp = call("jira/create_issue");
    EXPECT_EQ(status_of(resp), 403);
    EXPECT_EQ(code(resp), "not_read");
    // Another origin, a form post, no cookie.
    EXPECT_EQ(status_of(call("jira/search_issues", "Origin: https://evil.example\r\n")), 403);
    EXPECT_EQ(status_of(http(port, "POST /" + a->id + "/_workbench/mcp/jira/search_issues HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                       "\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nCookie: " + cookie + "\r\n\r\n{}")),
              403);
    EXPECT_EQ(status_of(post_json(port, "/" + a->id + "/_workbench/mcp/jira/search_issues", "{}", "")), 403);
    // policy.apps.mcp off refuses every call.
    r.pol = policy_with({{"mcp", "off"}});
    resp = call("jira/search_issues");
    EXPECT_EQ(status_of(resp), 403);
    EXPECT_EQ(code(resp), "policy_denied");
    EXPECT_EQ(r.mcp_calls.size(), 1u);
}

// ---- 2.4 Tools in a Run ------------------------------------------------------------------------------------------------

template <typename A, typename B>
void expect_same_message(const A& a, const B& b) {
    EXPECT_EQ(a.role, b.role);
    EXPECT_EQ(a.content, b.content);
    EXPECT_EQ(a.tool_call_id, b.tool_call_id);
    ASSERT_EQ(a.tool_calls.size(), b.tool_calls.size());
    for (size_t i = 0; i < a.tool_calls.size(); ++i) {
        EXPECT_EQ(a.tool_calls[i].id, b.tool_calls[i].id);
        EXPECT_EQ(a.tool_calls[i].tool, b.tool_calls[i].tool);
        EXPECT_EQ(a.tool_calls[i].arguments, b.tool_calls[i].arguments);
    }
    ASSERT_EQ(a.images.size(), b.images.size());
    for (size_t i = 0; i < a.images.size(); ++i) {
        EXPECT_EQ(a.images[i].mime, b.images[i].mime);
        EXPECT_EQ(a.images[i].data_base64, b.images[i].data_base64);
        EXPECT_EQ(a.images[i].prepared, b.images[i].prepared);
    }
}

// Changing the app/file constructors so these five schemas are not baseline definitions must make this test fail.
// It catches first-call discovery detours, schema churn and changes to source-bearing message history.
TEST(AppsTools, LocalAppBuildNeedsNoToolDiscovery) {
    if (!node_present()) GTEST_SKIP() << "client/runtime/node is missing (dev/scripts/fetch-node-runtime.ps1)";
    Rig r;
    std::vector<std::string> opened;
    AppToolsEnv te;
    te.apps = r.apps.get();
    te.server = r.server.get();
    te.open_url = [&](const std::string& u) { opened.push_back(u); };
    const auto defs = app_tools(te);

    Env env;
    const std::string data_root = narrow(r.tmp.path.wstring());
    fs::create_directories(r.tmp.path / "workspace");
    policy::EnforcerEnv ee;
    ee.policy = [&r] { return r.pol; };
    ee.workspace = narrow((r.tmp.path / "workspace").wstring());
    ee.data_root = data_root;
    ee.app_folders = [&r](const std::string& run) { return r.apps->folders_for(run); };
    auto enforcer = std::make_shared<policy::PolicyEnforcer>(ee);
    env.policy_hook = enforcer;
    env.make_runtime();
    for (auto d : defs) env.add_tool(d);
    tools::FileToolsEnv fe;
    fe.rules = [enforcer] { return enforcer->path_rules_now(); };
    fe.workspace = ee.workspace;
    for (auto& d : tools::file_tools(fe)) env.add_tool(d);
    env.start();

    const std::string folder = narrow((r.tmp.path / "apps" / "no-discovery").wstring());
    const std::string page = "<!doctype html><title>No discovery</title><h1>First-call source</h1>";
    env.model.push_calls({{"c1", "apps.create", {{"name", "No discovery"}, {"description", "First-call app"}}}});
    env.model.push_calls({{"c2", "files.write", {{"path", folder + "\\public\\index.html"}, {"content", page}}}});
    env.model.push_calls({{"c3", "apps.open", {{"app", "no-discovery"}}}});
    env.model.push_text("Opened.");
    const auto thread = env.thread();
    const auto run = env.run(thread, "Make a Local app without discovery detours");
    ASSERT_TRUE(env.wait(run, RunState::succeeded)) << env.rec(run).reason << "\n" << env.tool_content(run, "c1") << "\n"
                                                      << env.tool_content(run, "c2") << "\n" << env.tool_content(run, "c3");

    const auto requests = env.model.requests();
    ASSERT_EQ(requests.size(), 4u);
    const std::set<std::string> baseline{"apps.create", "apps.open", "files.read", "files.write", "files.edit"};
    std::set<std::string> first_ids;
    for (const auto& spec : requests.front().tools) first_ids.insert(spec.id);
    for (const auto& id : baseline) EXPECT_TRUE(first_ids.count(id)) << "missing baseline definition " << id;
    for (size_t n = 1; n < requests.size(); ++n) {
        ASSERT_EQ(requests[n].tools.size(), requests[n - 1].tools.size()) << "catalog changed at model call " << n;
        for (size_t t = 0; t < requests[n].tools.size(); ++t) {
            EXPECT_EQ(requests[n].tools[t].id, requests[n - 1].tools[t].id);
            EXPECT_EQ(requests[n].tools[t].description, requests[n - 1].tools[t].description);
            EXPECT_EQ(requests[n].tools[t].parameters.dump(), requests[n - 1].tools[t].parameters.dump());
        }
        ASSERT_GT(requests[n].messages.size(), requests[n - 1].messages.size());
        for (size_t m = 0; m < requests[n - 1].messages.size(); ++m)
            expect_same_message(requests[n - 1].messages[m], requests[n].messages[m]);
    }
    for (const auto& e : env.rt->events(run)) EXPECT_NE(e.kind, "tools_loaded");
    for (const auto& m : requests.back().messages)
        for (const auto& call : m.tool_calls) EXPECT_NE(call.tool, "find_tools");

    EXPECT_NE(env.tool_content(run, "c1").find("WORKBENCH_APP_PIPE"), std::string::npos);
    EXPECT_EQ(env.tool_content(run, "c2").find("denied"), std::string::npos);
    std::ifstream written(r.tmp.path / "apps" / "no-discovery" / "public" / "index.html");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(written), {}), page);
    ASSERT_EQ(opened.size(), 1u) << env.tool_content(run, "c3");
    EXPECT_EQ(opened.front().rfind("http://127.0.0.1:", 0), 0u);
    EXPECT_EQ(r.apps->running_count(), 1u);
}

TEST(AppsTools, CreateWriteOpenWithConfirmationUnderPolicyAndModeOff) {
    // apps.create and apps.open are not offered without the Node runtime (a checkout that has not fetched it).
    if (!node_present()) GTEST_SKIP() << "client/runtime/node is missing (dev/scripts/fetch-node-runtime.ps1)";
    Rig r;
    std::vector<std::string> opened;
    AppToolsEnv te;
    te.apps = r.apps.get();
    te.server = r.server.get();
    te.open_url = [&](const std::string& u) { opened.push_back(u); };
    const auto defs = app_tools(te);
    ASSERT_EQ(defs.size(), 6u);
    std::map<std::string, runtime::Risk> risks;
    for (const auto& d : defs) risks[d.id] = *d.risk;
    EXPECT_EQ(risks["apps.create"], runtime::Risk::write_local);
    EXPECT_EQ(risks["apps.list"], runtime::Risk::read);
    EXPECT_EQ(risks["apps.logs"], runtime::Risk::read);
    EXPECT_EQ(risks["apps.open"], runtime::Risk::write_local);
    EXPECT_EQ(risks["apps.stop"], runtime::Risk::write_local);
    EXPECT_EQ(risks["apps.delete"], runtime::Risk::destructive);

    // The Run's policy handler is the real enforcer; the apps folder lies under the data root.
    Env env;
    const std::string data_root = narrow(r.tmp.path.wstring());
    fs::create_directories(r.tmp.path / "workspace");
    policy::EnforcerEnv ee;
    ee.policy = [&r] { return r.pol; };
    ee.workspace = narrow((r.tmp.path / "workspace").wstring());
    ee.data_root = data_root;
    ee.app_folders = [&r](const std::string& run) { return r.apps->folders_for(run); };
    auto enforcer = std::make_shared<policy::PolicyEnforcer>(ee);
    env.policy_hook = enforcer;
    env.make_runtime();
    for (auto d : defs) env.add_tool(d);
    tools::FileToolsEnv fe;
    fe.rules = [enforcer] { return enforcer->path_rules_now(); };
    fe.workspace = ee.workspace;
    for (auto& d : tools::file_tools(fe)) env.add_tool(d);
    env.start();

    const std::string folder = narrow((r.tmp.path / "apps" / "q3-slides").wstring());
    const std::string page = "<!doctype html><title>Q3</title><h1>Q3 results</h1>";
    env.model.push_calls({{"c1", "apps.create", {{"name", "Q3 slides"}, {"description", "Q3 results"}, {"mcp_tools", {"mcp:jira/search_issues"}}}}});
    env.model.push_calls({{"c2", "files.write", {{"path", folder + "\\public\\index.html"}, {"content", page}}}});
    env.model.push_calls({{"c3", "apps.open", {{"app", "q3-slides"}}}});
    env.model.push_text("Opened.");
    const auto t = env.thread();
    const auto run = env.run(t, "Make a presentation of the Q3 results as an app");
    ASSERT_TRUE(env.wait_for([&] { return !env.rt->pending_actions(t).empty(); }))
        << env.tool_content(run, "c1") << "\n" << env.tool_content(run, "c2") << "\n" << env.tool_content(run, "c3") << "\n" << to_string(env.rec(run).state) << " " << env.rec(run).reason;
    EXPECT_NE(env.tool_content(run, "c1").find("WORKBENCH_APP_PIPE"), std::string::npos);
    EXPECT_EQ(env.tool_content(run, "c2").find("denied"), std::string::npos) << env.tool_content(run, "c2");
    {
        std::ifstream written(r.tmp.path / "apps" / "q3-slides" / "public" / "index.html");
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(written), {}), page);
    }
    // apps.open asks the user to confirm the MCP tools.
    const auto actions = env.rt->pending_actions(t);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_NE(jstr(actions[0].payload, "prompt").find("mcp:jira/search_issues"), std::string::npos) << actions[0].payload.dump();
    ASSERT_TRUE(env.rt->answer_question(actions[0].id, "Allow"));
    ASSERT_TRUE(env.wait(run, RunState::succeeded));
    EXPECT_FALSE(r.apps->get("q3-slides")->needs_confirmation);
    if (node_present()) {
        ASSERT_EQ(opened.size(), 1u) << env.tool_content(run, "c3");
        EXPECT_EQ(opened[0].rfind("http://127.0.0.1:", 0), 0u);
        EXPECT_EQ(r.apps->running_count(), 1u);
    } else {
        EXPECT_NE(env.tool_content(run, "c3").find("runtime package"), std::string::npos);
    }

    // Another Run may not write the folder; the agent workspace stays writable.
    env.model.push_calls({{"d1", "files.write", {{"path", folder + "\\server.js"}, {"content", "x"}}}});
    env.model.push_text("Done.");
    const auto t2 = env.thread();
    const auto run2 = env.run(t2, "change it");
    ASSERT_TRUE(env.wait(run2, RunState::succeeded));
    EXPECT_NE(env.tool_content(run2, "d1").find("data folder"), std::string::npos) << env.tool_content(run2, "d1");

    // policy.apps.mode off: the tools are not offered.
    r.pol = policy_with({{"mode", "off"}});
    for (const auto& d : defs) EXPECT_FALSE(runtime::tool_offered(d)) << d.id;
    r.pol = policy_with(nullptr);
    // apps.delete removes the folder.
    runtime::ToolContext ctx;
    const auto del = std::find_if(defs.begin(), defs.end(), [](const auto& d) { return d.id == "apps.delete"; });
    const auto deleted = del->run(ctx, {{"app", "q3-slides"}});
    EXPECT_TRUE(deleted.ok) << deleted.content;
    EXPECT_FALSE(fs::exists(r.tmp.path / "apps" / "q3-slides"));
    EXPECT_EQ(r.apps->running_count(), 0u);
}

// ---- fix-tool-waits 3.1-3.3: a start in progress does not hold the manager ---------------------------------------------

namespace {

long long ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

TEST(AppsManager, FolderAccessAndAppChangesDoNotWaitForAPendingStart) {
    if (!node_present()) GTEST_SKIP() << "client/runtime/node is missing (dev/scripts/fetch-node-runtime.ps1)";
    Rig r(std::chrono::milliseconds(500));
    auto a = r.apps->create(manifest("Stuck"), "run-1");
    ASSERT_TRUE(a);
    r.write(a->id, "server.js", "setInterval(() => {}, 1000);\n");  // never listens, so it never reports ready
    std::atomic<bool> start_done{false};
    Status start_result;
    std::thread starter([&] {
        start_result = r.apps->start(a->id);
        start_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_FALSE(start_done.load());  // the start is still pending

    auto t = std::chrono::steady_clock::now();
    EXPECT_EQ(r.apps->folders_for("run-1"), std::vector<std::string>{a->folder});
    EXPECT_LT(ms_since(t), 200);
    EXPECT_FALSE(start_done.load());

    t = std::chrono::steady_clock::now();
    auto b = r.apps->create(manifest("Other"), "run-2");
    EXPECT_TRUE(b);
    EXPECT_LT(ms_since(t), 200);
    EXPECT_FALSE(start_done.load());

    t = std::chrono::steady_clock::now();
    EXPECT_TRUE(r.apps->update(a->id, {{"description", "Still starting"}}, "run-1"));
    EXPECT_LT(ms_since(t), 200);
    EXPECT_FALSE(start_done.load());

    starter.join();
    EXPECT_EQ(start_result.error().code, "start");  // a failed start returns its error within the start timeout
}

TEST(AppsManager, TwoRequestsForAStoppedAppStartOneProcess) {
    if (!node_present()) GTEST_SKIP() << "client/runtime/node is missing (dev/scripts/fetch-node-runtime.ps1)";
    Rig r;
    auto a = r.apps->create(manifest("Quick"), "run-1");
    ASSERT_TRUE(a);
    r.write(a->id, "server.js", "require('node:http').createServer((q, s) => s.end('ok')).listen(process.env.WORKBENCH_APP_PIPE);\n");
    std::atomic<int> connected{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 2; ++i)
        callers.emplace_back([&] {
            if (r.apps->connect(a->id)) ++connected;
        });
    for (auto& c : callers) c.join();
    EXPECT_EQ(connected.load(), 2);
    EXPECT_EQ(r.apps->running_count(), 1u);
    const std::string log = r.apps->logs(a->id);
    size_t starts = 0;
    for (size_t p = log.find("started"); p != std::string::npos; p = log.find("started", p + 1)) ++starts;
    EXPECT_EQ(starts, 1u) << log;
}

TEST(AppsTools, WriteToAnAppFolderCompletesWhileAnotherAppStarts) {
    if (!node_present()) GTEST_SKIP() << "client/runtime/node is missing (dev/scripts/fetch-node-runtime.ps1)";
    Rig r(std::chrono::seconds(3));  // the other app's start is pending for three seconds
    auto stuck = r.apps->create(manifest("Stuck"), "run-0");
    ASSERT_TRUE(stuck);
    r.write(stuck->id, "server.js", "setInterval(() => {}, 1000);\n");
    auto target = r.apps->create(manifest("Target"), "run-1");
    ASSERT_TRUE(target);
    std::thread starter([&] { (void)r.apps->start(stuck->id); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Env env;
    const std::string data_root = narrow(r.tmp.path.wstring());
    fs::create_directories(r.tmp.path / "workspace");
    policy::EnforcerEnv ee;
    ee.policy = [&r] { return r.pol; };
    ee.workspace = narrow((r.tmp.path / "workspace").wstring());
    ee.data_root = data_root;
    ee.app_folders = [&r](const std::string& run) { return r.apps->folders_for(run); };
    auto enforcer = std::make_shared<policy::PolicyEnforcer>(ee);
    env.policy_hook = enforcer;
    env.make_runtime();
    tools::FileToolsEnv fe;
    fe.rules = [enforcer] { return enforcer->path_rules_now(); };
    fe.workspace = ee.workspace;
    for (auto& d : tools::file_tools(fe)) env.add_tool(d);
    env.start();
    // The first model step waits until the test has granted the Run access to the target folder (the Run's id is known
    // only once it starts), then the write follows.
    Gate granted;
    env.model.push_blocking(granted, calls_response({{"d1", "files.write", {{"path", target->folder + "\\public\\index.html"}, {"content", "<h1>Updated</h1>"}}}}));
    env.model.push_text("Done.");
    const auto t = env.thread();
    const auto began = std::chrono::steady_clock::now();
    const auto run = env.run(t, "update the page of the target app");
    ASSERT_TRUE(r.apps->update(target->id, {{"description", "Updated by the agent"}}, run));
    granted.release();
    ASSERT_TRUE(env.wait(run, RunState::succeeded, std::chrono::seconds(10)));
    EXPECT_LT(ms_since(began), 2000) << env.tool_content(run, "d1");
    EXPECT_EQ(env.tool_content(run, "d1").find("denied"), std::string::npos) << env.tool_content(run, "d1");
    std::ifstream written(r.tmp.path / "apps" / "target" / "public" / "index.html");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(written), {}), "<h1>Updated</h1>");
    starter.join();
}
