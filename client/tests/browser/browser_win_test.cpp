// client/browser-use on Windows (add-browser-use): Chrome on the dedicated profile over the debugging pipe. Needs Google
// Chrome; skipped without it.
#include "base/encoding.h"
#include "browser/chrome.h"
#include "browser/service.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "support/http_server.h"

#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <fstream>
#include <set>
#include <thread>

using namespace wb;
using namespace wb::browser;
using namespace std::chrono_literals;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::path(local_appdata()) / L"workbench-test" / widen("browser-" + random_urlsafe(6));
    ~TempDir() {
        std::error_code ec;
        for (int i = 0; i < 20; ++i) {  // Chrome may hold profile files for a moment after exit
            std::filesystem::remove_all(path, ec);
            if (!ec) return;
            std::this_thread::sleep_for(100ms);
        }
    }
};

std::optional<ChromeInfo> chrome() {
    auto c = find_chrome();
    if (!c) return std::nullopt;
    return *c;
}

#define REQUIRE_CHROME()                                                \
    auto chrome_info = chrome();                                        \
    if (!chrome_info) GTEST_SKIP() << "Google Chrome is not installed"; \
    (void)0

// The process and all its descendants.
std::set<DWORD> process_tree(DWORD root) {
    std::set<DWORD> tree = {root};
    for (bool grew = true; grew;) {
        grew = false;
        unique_handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        PROCESSENTRY32W e{};
        e.dwSize = sizeof e;
        for (BOOL ok = ::Process32FirstW(snap.get(), &e); ok; ok = ::Process32NextW(snap.get(), &e))
            if (tree.count(e.th32ParentProcessID) && tree.insert(e.th32ProcessID).second) grew = true;
    }
    return tree;
}

int listening_sockets(const std::set<DWORD>& pids) {
    int n = 0;
    for (const ULONG family : {AF_INET, AF_INET6}) {
        ULONG size = 0;
        ::GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        std::vector<unsigned char> buf(size + 1024);
        size = static_cast<ULONG>(buf.size());
        if (::GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) continue;
        if (family == AF_INET) {
            auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) n += pids.count(t->table[i].dwOwningPid) ? 1 : 0;
        } else {
            auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) n += pids.count(t->table[i].dwOwningPid) ? 1 : 0;
        }
    }
    return n;
}

}  // namespace

TEST(BrowserChrome, FindsAChromeOfASupportedVersion) {
    REQUIRE_CHROME();
    EXPECT_GE(chrome_info->major, kMinChromeMajor) << chrome_info->version;
    std::printf("Chrome %s at %s\n", chrome_info->version.c_str(), narrow(chrome_info->exe.wstring()).c_str());
    auto missing = find_chrome(L"C:\\nonexistent\\chrome.exe");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "not_installed");
}

TEST(BrowserChrome, PolicyKeysBlockDebugging) {
    const std::wstring key = L"Software\\Workbench-test\\ChromePolicy-" + widen(random_urlsafe(6));
    EXPECT_TRUE(read_chrome_policy(key).allowed);
    HKEY h = nullptr;
    ASSERT_EQ(::RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &h, nullptr), ERROR_SUCCESS);
    DWORD zero = 0;
    ::RegSetValueExW(h, L"RemoteDebuggingAllowed", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof zero);
    auto st = read_chrome_policy(key);
    EXPECT_FALSE(st.allowed);
    EXPECT_EQ(st.policy, "RemoteDebuggingAllowed");
    DWORD one = 1, two = 2;
    ::RegSetValueExW(h, L"RemoteDebuggingAllowed", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one);
    EXPECT_TRUE(read_chrome_policy(key).allowed);
    ::RegSetValueExW(h, L"DeveloperToolsAvailability", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&two), sizeof two);
    st = read_chrome_policy(key);
    EXPECT_FALSE(st.allowed);
    EXPECT_EQ(st.policy, "DeveloperToolsAvailability");
    ::RegCloseKey(h);
    ::RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Workbench-test");
    // This machine's own Chrome policy, for the record (task 1.1).
    const auto real = read_chrome_policy();
    std::printf("machine Chrome policy: debugging %s %s\n", real.allowed ? "allowed" : "blocked", real.policy.c_str());
}

// Pipe-Only Control Channel: the handshake works over the pipe, a page loads, and no Chrome process listens on TCP.
TEST(BrowserChrome, PipeHandshakeLoadsAPageWithoutAListeningPort) {
    REQUIRE_CHROME();
    test::LoopbackHttpServer server([](const test::LoopbackHttpServer::Request&) {
        return test::LoopbackHttpServer::response(200, "OK", "<html><title>Hello pipe</title><body><h1>Hi</h1></body></html>",
                                                   "Content-Type: text/html\r\n");
    });
    TempDir dir;
    LaunchOptions o;
    o.exe = chrome_info->exe;
    o.profile_dir = dir.path / L"profile";
    o.headless = true;
    // This is the run's first Chrome start: on a fresh CI runner it is a cold start (no disk cache, the antivirus scans
    // Chrome, a new profile) that can exceed the product's 10 s. The test checks the pipe handshake, not start-up time.
    o.handshake_timeout = 60s;
    std::mutex mx;
    std::vector<std::string> events;
    auto p = launch_chrome(o, [&](const CdpMessage& m) {
        const std::lock_guard lock(mx);
        events.push_back(m.method);
    });
    ASSERT_TRUE(p) << p.error().message;
    auto& cdp = (*p)->cdp();
    auto target = cdp.call("Target.createTarget", {{"url", "about:blank"}});
    ASSERT_TRUE(target) << target.error().message;
    auto attached = cdp.call("Target.attachToTarget", {{"targetId", jstr(*target, "targetId")}, {"flatten", true}});
    ASSERT_TRUE(attached) << attached.error().message;
    const std::string session = jstr(*attached, "sessionId");
    ASSERT_TRUE(cdp.call("Page.enable", json::object(), session));
    auto nav = cdp.call("Page.navigate", {{"url", server.url("/hello")}}, session);
    ASSERT_TRUE(nav) << nav.error().message;
    std::string title;
    for (int i = 0; i < 100 && title != "Hello pipe"; ++i) {
        auto r = cdp.call("Runtime.evaluate", {{"expression", "document.title"}, {"returnByValue", true}}, session);
        if (r) title = jstr((*r)["result"], "value");
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(title, "Hello pipe");
    EXPECT_EQ(listening_sockets(process_tree((*p)->pid())), 0);
    (*p)->terminate();
    EXPECT_FALSE((*p)->running());
}

// ---- The browser tools against a local test site (client/browser-use Browser Actions and the rest) -------------------

namespace {

using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolOutput;
using test::LoopbackHttpServer;

constexpr char kFormPage[] = R"(<!doctype html><html><head><title>Form</title></head><body>
<h1>Orders</h1>
<p id="out">nothing yet</p>
<input aria-label="Query" id="q">
<button type="button" onclick="document.getElementById('out').textContent='searched '+document.getElementById('q').value">Search</button>
<select aria-label="Colour" onchange="document.getElementById('out').textContent='colour '+this.value"><option value="r">Red</option><option value="g">Green</option></select>
<form action="/submit" method="get"><input aria-label="Note" name="note"><button>Odeslat</button></form>
<input type="password" aria-label="Password">
<a href="/next">Next page</a>
<a href="/file">Get file</a>
<div style="height:3000px"></div><p>Bottom text</p>
</body></html>)";

struct Site {
    int pixel_port = 0;  // the port of the page's cross-host image
    LoopbackHttpServer server{[this](const LoopbackHttpServer::Request& r) { return answer(r); }};

    std::string answer(const LoopbackHttpServer::Request& r) {
        const auto html = [](const std::string& body) { return LoopbackHttpServer::response(200, "OK", body, "Content-Type: text/html\r\n"); };
        if (r.path == "/form") return html(kFormPage);
        if (r.path == "/hang") {  // like a navigation Chrome holds for a certificate choice
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return html("<html><head><title>Late</title></head><body>late</body></html>");
        }
        if (r.path == "/next") return html("<html><head><title>Next</title></head><body><h1>Second page</h1></body></html>");
        if (r.path.rfind("/submit", 0) == 0) return html("<html><head><title>Sent</title></head><body><h1>Thanks</h1></body></html>");
        if (r.path == "/file")
            return LoopbackHttpServer::response(200, "OK", "PK-not-really-a-zip",
                                                "Content-Type: application/octet-stream\r\nContent-Disposition: attachment; "
                                                "filename=\"report.bin\"\r\n");
        if (r.path == "/img-page")
            return html("<html><head><title>Images</title></head><body><img src=\"http://localhost:" + std::to_string(pixel_port) +
                        "/pixel.png\" alt=\"pixel\"><p>Images page</p></body></html>");
        return LoopbackHttpServer::response(404, "Not Found", "no");
    }
    bool requested(const std::string& path) const {
        for (const auto& r : server.requests())
            if (r.path.rfind(path, 0) == 0) return true;
        return false;
    }
};

std::string ref_of(const std::string& snapshot, const std::string& line_prefix) {
    const auto at = snapshot.find(line_prefix);
    if (at == std::string::npos) return {};
    const auto r = snapshot.find("[ref=", at);
    const auto end = snapshot.find(']', r);
    return r == std::string::npos || end == std::string::npos ? std::string{} : snapshot.substr(r + 5, end - r - 5);
}

struct BrowserFixture {
    TempDir dir;
    std::mutex mx;
    std::shared_ptr<const policy::EffectivePolicy> current;
    std::vector<std::string> denied;  // URLs refused by the egress check
    BrowserEnv env;
    std::shared_ptr<BrowserService> service;
    std::vector<runtime::ToolDef> tools;
    runtime::RunControl control;
    ToolContext ctx;

    explicit BrowserFixture(json browser_policy = json::object(), bool headless = true, json allow = {"127.0.0.1"}) {
        set_policy(std::move(browser_policy), std::move(allow));
        env.policy = [this] {
            const std::lock_guard lock(mx);
            return current;
        };
        env.data_root = dir.path;
        env.quarantine_dir = narrow((dir.path / L"quarantine").wstring());
        env.prepare_image = [](const std::vector<std::uint8_t>& png) -> Result<model::ImagePart> {
            return model::ImagePart{"image/png", base64_encode(png), true};
        };
        env.on_denied = [this](const std::string&, const std::string& url, const policy::RuleVerdict&) {
            const std::lock_guard lock(mx);
            denied.push_back(url);
        };
        env.force_headless = headless;
        if (!headless) env.extra_chrome_args = {L"--window-position=-2400,-2400", L"--window-size=800,600"};
        ctx.run_id = "run-1";
        ctx.call_id = "call-1";
        ctx.control = &control;
    }
    void make() {
        service = std::make_shared<BrowserService>(env);
        tools = browser_tools(service);
    }
    ~BrowserFixture() {
        tools.clear();
        if (service) service->shutdown();
    }
    void set_policy(json browser, json allow = {"127.0.0.1"}) {
        policy::PolicyBundle b;
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = {{"network", {{"egress_allow", std::move(allow)}}}, {"tools", {{"browser", std::move(browser)}}}};
        const std::lock_guard lock(mx);
        current = std::make_shared<policy::EffectivePolicy>(b);
    }
    const runtime::ToolDef& tool(const std::string& id) {
        for (const auto& t : tools)
            if (t.id == id) return t;
        throw std::runtime_error("no tool " + id);
    }
    ToolOutput call(const std::string& id, const json& args = json::object()) { return tool(id).run(ctx, args); }
    std::string snapshot() {
        auto s = call("browser.snapshot");
        EXPECT_TRUE(s.ok) << s.content;
        return s.content;
    }
    std::vector<std::string> denials() {
        const std::lock_guard lock(mx);
        return denied;
    }
};

#define REQUIRE_BROWSER(f)                                                                 \
    REQUIRE_CHROME();                                                                      \
    (f).make();                                                                            \
    if (!(f).service->offered()) GTEST_SKIP() << "browser use is unavailable: " << (f).service->state().reason; \
    (void)0

}  // namespace

TEST(BrowserTools, SnapshotClickTypeSelectScrollWaitAndBack) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    auto nav = f.call("browser.navigate", {{"url", site.server.url("/form")}});
    ASSERT_TRUE(nav.ok) << nav.content;
    ASSERT_TRUE(nav.provenance);
    EXPECT_EQ(nav.provenance->source_kind, "browser");
    EXPECT_FALSE(nav.provenance->trusted);
    EXPECT_NE(nav.content.find("\"title\": \"Form\""), std::string::npos) << nav.content;

    std::string snap = f.snapshot();
    EXPECT_NE(snap.find("heading \"Orders\" [level=1]"), std::string::npos) << snap;
    EXPECT_NE(snap.find("text: \"nothing yet\""), std::string::npos) << snap;
    const std::string query = ref_of(snap, "textbox \"Query\""), search = ref_of(snap, "button \"Search\""),
                      colour = ref_of(snap, "combobox \"Colour\"");
    ASSERT_FALSE(query.empty()) << snap;
    ASSERT_FALSE(search.empty()) << snap;
    ASSERT_FALSE(colour.empty()) << snap;

    auto typed = f.call("browser.type", {{"ref", query}, {"text", "brake pads"}});
    ASSERT_TRUE(typed.ok) << typed.content;
    auto clicked = f.call("browser.click", {{"ref", search}});
    ASSERT_TRUE(clicked.ok) << clicked.content;
    EXPECT_NE(f.snapshot().find("text: \"searched brake pads\""), std::string::npos);

    auto sel = f.call("browser.select", {{"ref", colour}, {"value", "Green"}});
    ASSERT_TRUE(sel.ok) << sel.content;
    EXPECT_NE(f.snapshot().find("text: \"colour g\""), std::string::npos);
    EXPECT_FALSE(f.call("browser.select", {{"ref", colour}, {"value", "Purple"}}).ok);

    auto scrolled = f.call("browser.scroll", {{"direction", "down"}, {"pages", 2}});
    ASSERT_TRUE(scrolled.ok) << scrolled.content;
    EXPECT_NE(scrolled.content.find("\"scroll_y\""), std::string::npos) << scrolled.content;
    auto waited = f.call("browser.wait", {{"text", "Bottom text"}, {"seconds", 5}});
    EXPECT_TRUE(waited.ok) << waited.content;
    auto missing = f.call("browser.wait", {{"text", "never there"}, {"seconds", 0.5}});
    EXPECT_FALSE(missing.ok);

    snap = f.snapshot();
    auto next = f.call("browser.click", {{"ref", ref_of(snap, "link \"Next page\"")}});
    ASSERT_TRUE(next.ok) << next.content;
    EXPECT_NE(next.content.find("\"navigated\": true"), std::string::npos) << next.content;
    EXPECT_NE(f.snapshot().find("Second page"), std::string::npos);
    auto back = f.call("browser.back");
    ASSERT_TRUE(back.ok) << back.content;
    EXPECT_NE(back.content.find("/form"), std::string::npos) << back.content;

    auto shot = f.call("browser.screenshot");
    ASSERT_TRUE(shot.ok) << shot.content;
    ASSERT_EQ(shot.images.size(), 1u);
    EXPECT_EQ(shot.images[0].mime, "image/png");
}

TEST(BrowserTools, StaleReferenceAsksForANewSnapshot) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/form")}}).ok);
    const std::string search = ref_of(f.snapshot(), "button \"Search\"");
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
    auto r = f.call("browser.click", {{"ref", search}});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.content.find("stale reference " + search), std::string::npos) << r.content;
    EXPECT_NE(r.content.find("new snapshot"), std::string::npos) << r.content;
    EXPECT_FALSE(f.call("browser.click", {{"ref", "e99999"}}).ok);
}

TEST(BrowserTools, EgressDeniesNavigationsAndSubresourcesBeforeAnyRequest) {
    Site site;
    site.pixel_port = std::stoi(site.server.url("").substr(17));
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    // A navigation to a host the policy does not allow.
    auto denied = f.call("browser.navigate", {{"url", site.server.url_as("localhost", "/next")}});
    EXPECT_FALSE(denied.ok);
    EXPECT_NE(denied.content.find("blocked by policy"), std::string::npos) << denied.content;
    EXPECT_FALSE(site.requested("/next"));
    // A page on an allowed host loading an image from a refused one.
    auto page = f.call("browser.navigate", {{"url", site.server.url("/img-page")}});
    ASSERT_TRUE(page.ok) << page.content;
    EXPECT_TRUE(site.requested("/img-page"));
    EXPECT_FALSE(site.requested("/pixel.png"));
    bool pixel_denied = false;
    for (const auto& u : f.denials()) pixel_denied |= u.find("/pixel.png") != std::string::npos;
    EXPECT_TRUE(pixel_denied);
    // Local schemes never load.
    EXPECT_FALSE(f.call("browser.navigate", {{"url", "file:///C:/Windows/win.ini"}}).ok);
    EXPECT_FALSE(browser_request_verdict(f.current.get(), "file:///C:/Windows/win.ini").allowed);
    EXPECT_TRUE(browser_request_verdict(f.current.get(), "data:text/plain,hi").allowed);
    EXPECT_FALSE(browser_request_verdict(nullptr, "http://127.0.0.1/").allowed);
}

TEST(BrowserTools, ChromeUsesTheEgressProxyWithItsCredential) {
    Site site;
    std::mutex mx;
    std::vector<std::pair<std::string, std::string>> proxy_denials;  // tool, host
    sandbox::EgressProxy proxy(
        [](const std::string& host) { return policy::check_egress_host(host, {"localhost"}, {}); },
        [&](const sandbox::EgressProxy::Owner& o, const std::string& host, const policy::RuleVerdict&) {
            const std::lock_guard lock(mx);
            proxy_denials.emplace_back(o.tool, host);
        },
        true);
    BrowserFixture f(json::object(), true, {"127.0.0.1", "localhost"});
    f.env.proxy = &proxy;
    f.env.extra_chrome_args = {L"--proxy-bypass-list=<-loopback>"};  // loopback goes through the proxy too
    REQUIRE_BROWSER(f);
    auto ok = f.call("browser.navigate", {{"url", site.server.url_as("localhost", "/next")}});
    ASSERT_TRUE(ok.ok) << ok.content;
    EXPECT_NE(ok.content.find("\"title\": \"Next\""), std::string::npos) << ok.content;
    // The proxy refuses 127.0.0.1 although the policy allows it: Chrome really goes through the proxy, authenticated.
    (void)f.call("browser.navigate", {{"url", site.server.url("/form")}});
    EXPECT_FALSE(site.requested("/form"));
    // Chrome's own requests (outside any page, so outside the request interception) meet the proxy as well.
    const std::lock_guard lock(mx);
    bool loopback_denied = false;
    for (const auto& [tool, host] : proxy_denials) {
        EXPECT_EQ(tool, "browser") << host;
        loopback_denied |= host == "127.0.0.1";
    }
    EXPECT_TRUE(loopback_denied);
}

TEST(BrowserTools, CredentialFieldsHandOverToTheUser) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/form")}}).ok);
    const std::string password = ref_of(f.snapshot(), "textbox \"Password\"");
    ASSERT_FALSE(password.empty());
    auto r = f.call("browser.type", {{"ref", password}, {"text", "hunter2"}});
    ASSERT_TRUE(r.question) << r.content;
    EXPECT_NE(r.question->prompt.find("sign in"), std::string::npos);
    EXPECT_EQ(r.question->choices, (std::vector<std::string>{"Continue", "Stop"}));
    auto cont = f.tool("browser.type").resume(f.ctx, json::object(), r.question->state, "Continue");
    EXPECT_TRUE(cont.ok);
    EXPECT_NE(cont.content.find("new snapshot"), std::string::npos);
    EXPECT_FALSE(f.control.cancelled());
    auto stop = f.tool("browser.type").resume(f.ctx, json::object(), r.question->state, "Stop");
    EXPECT_FALSE(stop.ok);
    EXPECT_TRUE(f.control.cancelled());
    // The model can also ask for the user explicitly.
    runtime::RunControl c2;
    f.ctx.control = &c2;
    auto w = f.call("browser.wait", {{"user", "a second factor is needed"}});
    EXPECT_TRUE(w.question);
}

TEST(BrowserTools, AHeldNavigationIsHandedOverInAWindowAndFailsHeadless) {
    Site site;
    {
        BrowserFixture f(json::object(), false);
        f.env.navigation_timeout = 1s;
        REQUIRE_BROWSER(f);
        auto r = f.call("browser.navigate", {{"url", site.server.url("/hang")}});
        ASSERT_TRUE(r.question) << r.content;
        EXPECT_NE(r.question->prompt.find("sign in"), std::string::npos);
    }
    {
        BrowserFixture f;
        f.env.navigation_timeout = 1s;
        REQUIRE_BROWSER(f);
        auto r = f.call("browser.navigate", {{"url", site.server.url("/hang")}});
        EXPECT_FALSE(r.question);
        EXPECT_FALSE(r.ok);
        EXPECT_NE(r.content.find("certificate"), std::string::npos) << r.content;
    }
}

TEST(BrowserTools, PasswordWasNotTyped) {
    Site site;
    BrowserFixture f({{"allow_eval", true}});
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/form")}}).ok);
    const std::string password = ref_of(f.snapshot(), "textbox \"Password\"");
    (void)f.call("browser.type", {{"ref", password}, {"text", "hunter2"}});
    auto v = f.call("browser.eval", {{"expression", "document.querySelector('input[type=password]').value"}});
    ASSERT_TRUE(v.ok) << v.content;
    EXPECT_EQ(v.content, "\"\"");
}

TEST(BrowserTools, DownloadsGoToTheQuarantineWithMarkOfTheWeb) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/form")}}).ok);
    auto r = f.call("browser.click", {{"ref", ref_of(f.snapshot(), "link \"Get file\"")}});
    ASSERT_TRUE(r.ok) << r.content;
    ASSERT_NE(r.content.find("\"downloads\""), std::string::npos) << r.content;
    const json body = json::parse(r.content);
    const std::string file = jstr(body["downloads"][0], "file");
    ASSERT_FALSE(file.empty()) << r.content;
    EXPECT_EQ(std::filesystem::path(widen(file)).parent_path(), std::filesystem::path(widen(f.env.quarantine_dir)));
    std::ifstream in(std::filesystem::path(widen(file)), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes, "PK-not-really-a-zip");
    std::ifstream zone(std::filesystem::path(widen(file + ":Zone.Identifier")), std::ios::binary);
    std::string z((std::istreambuf_iterator<char>(zone)), std::istreambuf_iterator<char>());
    EXPECT_NE(z.find("ZoneId=3"), std::string::npos) << z;
    // Nothing is left in the staging folder.
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_empty(f.dir.path / L"browser-downloads", ec));
}

TEST(BrowserTools, ScreenshotsAreNotRetainedUnlessPolicySaysSo) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
    ASSERT_EQ(f.call("browser.screenshot", {{"full_page", true}}).images.size(), 1u);
    EXPECT_FALSE(std::filesystem::exists(f.dir.path / L"screenshots"));
    f.set_policy({{"retain_screenshots", true}});
    ASSERT_EQ(f.call("browser.screenshot").images.size(), 1u);
    const auto dir = f.dir.path / L"screenshots" / L"run-1";
    ASSERT_TRUE(std::filesystem::exists(dir));
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        ++files;
        std::ifstream in(e.path(), std::ios::binary);
        char head[8] = {};
        in.read(head, 8);
        EXPECT_NE(std::string(head + 1, 3), "PNG");  // encrypted at rest
    }
    EXPECT_EQ(files, 1);
}

TEST(BrowserTools, EvalIsOfferedOnlyWhenPolicyAllowsIt) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    EXPECT_FALSE(runtime::tool_offered(f.tool("browser.eval")));
    EXPECT_TRUE(runtime::tool_offered(f.tool("browser.navigate")));
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
    EXPECT_FALSE(f.call("browser.eval", {{"expression", "1+1"}}).ok);
    f.set_policy({{"allow_eval", true}});
    EXPECT_TRUE(runtime::tool_offered(f.tool("browser.eval")));
    auto r = f.call("browser.eval", {{"expression", "document.title"}});
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_EQ(r.content, "\"Next\"");
    EXPECT_EQ(runtime::effective_risk(f.tool("browser.eval"), json::object(), "run-1"), Risk::destructive);
}

TEST(BrowserTools, RiskFollowsThePageAndTheRun) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/form")}}).ok);
    const std::string snap = f.snapshot();
    const auto risk = [&](const char* tool, const json& args) { return runtime::effective_risk(f.tool(tool), args, "run-1"); };
    EXPECT_EQ(risk("browser.snapshot", json::object()), Risk::read);
    EXPECT_EQ(risk("browser.scroll", json::object()), Risk::read);
    EXPECT_EQ(risk("browser.click", {{"ref", ref_of(snap, "button \"Search\"")}}), Risk::write_local);
    EXPECT_EQ(risk("browser.click", {{"ref", ref_of(snap, "button \"Odeslat\"")}}), Risk::external);  // submits a form
    EXPECT_EQ(risk("browser.type", {{"ref", ref_of(snap, "textbox \"Query\"")}, {"text", "x"}}), Risk::write_local);
    EXPECT_EQ(risk("browser.type", {{"ref", ref_of(snap, "textbox \"Query\"")}, {"text", "x"}, {"submit", true}}), Risk::external);
    EXPECT_EQ(risk("browser.navigate", {{"url", site.server.url("/next")}}), Risk::write_local);  // the start site
    EXPECT_EQ(risk("browser.navigate", {{"url", "https://elsewhere.example/"}}), Risk::external);
    EXPECT_EQ(risk("browser.tabs", {{"action", "list"}}), Risk::read);
    // Another Run has no start site yet.
    EXPECT_EQ(runtime::effective_risk(f.tool("browser.navigate"), {{"url", "https://elsewhere.example/"}}, "run-2"), Risk::write_local);
}

TEST(BrowserTools, TabsBelongToTheirRunAndCloseWithIt) {
    Site site;
    BrowserFixture f;
    REQUIRE_BROWSER(f);
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/form")}}).ok);
    auto opened = f.call("browser.tabs", {{"action", "open"}, {"url", site.server.url("/next")}});
    ASSERT_TRUE(opened.ok) << opened.content;
    auto list = f.call("browser.tabs");
    ASSERT_TRUE(list.ok) << list.content;
    const json tabs = json::parse(list.content);
    ASSERT_EQ(tabs.size(), 2u) << list.content;
    EXPECT_TRUE(tabs[1]["active"].get<bool>());
    ASSERT_TRUE(f.call("browser.tabs", {{"action", "switch"}, {"index", 0}}).ok);
    EXPECT_NE(f.snapshot().find("Orders"), std::string::npos);

    ToolContext other = f.ctx;
    other.run_id = "run-2";
    EXPECT_FALSE(f.tool("browser.snapshot").run(other, json::object()).ok);  // no page in that Run
    ASSERT_TRUE(f.tool("browser.navigate").run(other, {{"url", site.server.url("/next")}}).ok);
    const std::string t1 = f.service->active_target("run-1"), t2 = f.service->active_target("run-2");
    EXPECT_FALSE(t1.empty());
    EXPECT_NE(t1, t2);
    const unsigned long pid = f.service->chrome_pid();
    f.service->run_ended("run-1");
    EXPECT_TRUE(f.service->active_target("run-1").empty());
    EXPECT_FALSE(f.call("browser.snapshot").ok);
    EXPECT_TRUE(f.tool("browser.snapshot").run(other, json::object()).ok);  // the other Run keeps its tab
    EXPECT_EQ(f.service->chrome_pid(), pid);                                  // and the browser
}

TEST(BrowserTools, TheBrowserClosesWhenIdle) {
    Site site;
    BrowserFixture f;
    f.env.idle_timeout = 300ms;
    REQUIRE_BROWSER(f);
    EXPECT_EQ(f.service->chrome_pid(), 0u);  // starts on first use
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
    EXPECT_NE(f.service->chrome_pid(), 0u);
    EXPECT_TRUE(f.service->state().running);
    f.service->tick();
    EXPECT_NE(f.service->chrome_pid(), 0u);  // not idle yet
    std::this_thread::sleep_for(500ms);
    f.service->tick();
    EXPECT_EQ(f.service->chrome_pid(), 0u);
    EXPECT_FALSE(f.service->state().running);
    // The next call starts it again.
    ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
    EXPECT_NE(f.service->chrome_pid(), 0u);
}

TEST(BrowserTools, VisibleForTheUserHeadlessForUnattendedRunsWhenPolicyAllows) {
    Site site;
    {
        BrowserFixture f(json::object(), false);
        REQUIRE_BROWSER(f);
        ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
        auto st = f.service->state();
        EXPECT_TRUE(st.running);
        EXPECT_FALSE(st.headless);
        f.ctx.unattended = true;  // an unattended Run without the policy still gets a window
        f.service->run_ended("run-1");
        ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
        EXPECT_FALSE(f.service->state().headless);
    }
    {
        BrowserFixture f({{"headless_unattended", true}}, false);
        REQUIRE_BROWSER(f);
        f.ctx.unattended = true;
        ASSERT_TRUE(f.call("browser.navigate", {{"url", site.server.url("/next")}}).ok);
        EXPECT_TRUE(f.service->state().headless);
    }
}

TEST(BrowserTools, ChromePolicyWithdrawsTheTools) {
    REQUIRE_CHROME();
    const std::wstring key = L"Software\\Workbench-test\\ChromePolicy-" + widen(random_urlsafe(6));
    HKEY h = nullptr;
    ASSERT_EQ(::RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &h, nullptr), ERROR_SUCCESS);
    DWORD zero = 0;
    ::RegSetValueExW(h, L"RemoteDebuggingAllowed", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof zero);
    ::RegCloseKey(h);
    BrowserFixture f;
    f.env.chrome_policy_key = key;
    f.make();
    const auto st = f.service->state();
    ::RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Workbench-test");
    EXPECT_EQ(st.state, "blocked");
    EXPECT_EQ(st.policy, "RemoteDebuggingAllowed");
    EXPECT_EQ(st.reason, "Browser use blocked by Chrome policy RemoteDebuggingAllowed");
    for (const auto& t : f.tools) EXPECT_FALSE(runtime::tool_offered(t)) << t.id;
    EXPECT_EQ(f.service->chrome_pid(), 0u);

    BrowserFixture missing;
    missing.env.chrome_exe = L"C:\\nonexistent\\chrome.exe";
    missing.make();
    EXPECT_EQ(missing.service->state().state, "unavailable");
    EXPECT_FALSE(missing.service->offered());
}
