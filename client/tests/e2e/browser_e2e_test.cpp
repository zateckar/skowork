// End-to-end browser use (add-browser-use tasks 1.1, 1.2, 5.1): the agent runtime on the real main model, through the
// local control-plane gateway, drives the agent browser (Google Chrome) on public sites, under the real PolicyEnforcer
// and with Chrome's traffic going through the egress proxy. Local only, never in CI; needs Chrome and the dev stack.
#include "e2e_support.h"

#include "base/strings.h"
#include "browser/service.h"
#include "model/model_client.h"
#include "platform/win.h"
#include "policy/enforcer.h"
#include "runtime/runtime.h"
#include "sandbox/proxy.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace wb;
using namespace std::chrono_literals;

namespace {

std::string env_var(const char* name) {
    wchar_t buf[1024];
    const DWORD n = ::GetEnvironmentVariableW(widen(name).c_str(), buf, 1024);
    return n > 0 && n < 1024 ? narrow(std::wstring(buf, n)) : std::string{};
}

std::shared_ptr<const policy::EffectivePolicy> make_policy(const std::vector<std::string>& allow) {
    policy::PolicyBundle b;
    b.sub = "user";
    b.ver = 1;
    b.exp = 4102444800;
    // Browsing and typing on the page run; submitting and leaving the site need the user's approval.
    b.policy = {{"network", {{"egress_allow", allow}}},
                {"tools", {{"risk", {{"read", "allow"}, {"write-local", "allow"}, {"external", "approve"}, {"destructive", "deny"}}}}}};
    return std::make_shared<const policy::EffectivePolicy>(b);
}

struct Outcome {
    runtime::RunState state = runtime::RunState::queued;
    std::string reason, answer;
    std::vector<json> approvals;          // approval payloads
    std::vector<std::string> questions;   // question prompts
    std::vector<std::string> calls;       // tool ids in order
    std::vector<size_t> snapshot_chars;   // size of every browser.snapshot result
};

struct BrowserE2E : ::testing::Test {
    void SetUp() override {
        WORKBENCH_REQUIRE_STACK();
        if (!browser::find_chrome()) GTEST_SKIP() << "Google Chrome is not installed";
        auto s = e2e::shared_session();
        ASSERT_TRUE(s) << s.error().message;
        auth = std::make_unique<model::TokenManagerAuth>((*s)->tm);
        model::ModelClientConfig cfg;
        cfg.gateway_url = [] { return e2e::env()->cp + "/llm/v1"; };
        cfg.idle_timeout = 180s;
        client = std::make_unique<model::ModelClient>(cfg, *auth, transport);
        auto roles = client->refresh_roles();
        if (!roles.ok()) GTEST_SKIP() << "the gateway has no model: " << roles.failure().message;
    }

    // One Run of `prompt` with the browser tools, approving every Approval and stopping at every hand-over.
    Outcome run_task(const std::string& name, const std::vector<std::string>& allow, const std::string& prompt) {
        Outcome out;
        e2e::TempStore ts;
        const auto p = make_policy(allow);
        sandbox::EgressProxy proxy(
            [p](const std::string& host) {
                return policy::check_egress_host(host, p->strings("network.egress_allow"), p->strings("network.egress_deny"));
            },
            [](const sandbox::EgressProxy::Owner&, const std::string&, const policy::RuleVerdict&) {});
        browser::BrowserEnv be;
        be.policy = [p] { return p; };
        be.data_root = ts.root();
        be.quarantine_dir = narrow((ts.root() / L"quarantine").wstring());
        be.proxy = &proxy;
        be.prepare_image = [this](const std::vector<std::uint8_t>& png) { return client->prepare_image(png); };
        be.idp_hosts = {browser::url_host(e2e::env()->issuer)};
        // A real window, off-screen, so sites see an ordinary browser.
        be.extra_chrome_args = {L"--window-position=-2400,-2400", L"--window-size=1280,900"};
        auto service = std::make_shared<browser::BrowserService>(be);

        const auto ws = ts.root() / L"workspace";
        std::filesystem::create_directories(ws);
        policy::EnforcerEnv e;
        e.policy = [p] { return p; };
        e.workspace = narrow(ws.wstring());
        e.data_root = narrow(ts.root().wstring());
        runtime::RuntimeConfig cfg;
        auto rt = runtime::AgentRuntime::create(cfg, ts.store().db(), *client, system_clock(),
                                                runtime::HookRegistry(std::make_shared<policy::PolicyEnforcer>(e)));
        if (!rt) {
            ADD_FAILURE() << rt.error().message;
            return out;
        }
        std::mutex mx;
        for (auto t : browser::browser_tools(service)) {
            if (t.id == "browser.snapshot") {
                t.run = [inner = t.run, &out, &mx](runtime::ToolContext& ctx, const json& a) {
                    auto r = inner(ctx, a);
                    const std::lock_guard lock(mx);
                    out.snapshot_chars.push_back(r.content.size());
                    return r;
                };
            }
            EXPECT_TRUE((*rt)->add_tool(std::move(t)));
        }
        EXPECT_TRUE((*rt)->start());
        auto thread = (*rt)->create_thread(name);
        auto id = (*rt)->start_run(*thread, prompt);
        if (!id) {
            ADD_FAILURE() << id.error().message;
            return out;
        }
        std::set<std::string> handled;
        const auto deadline = std::chrono::steady_clock::now() + 8min;
        while (std::chrono::steady_clock::now() < deadline) {
            out.state = (*rt)->run(*id)->state;
            if (out.state == runtime::RunState::succeeded || out.state == runtime::RunState::failed ||
                out.state == runtime::RunState::cancelled)
                break;
            for (const auto& a : (*rt)->pending_actions(*thread)) {
                if (!handled.insert(a.id).second) continue;
                if (a.kind == "approval") {
                    out.approvals.push_back(a.payload);
                    (void)(*rt)->resolve_approval(a.id, runtime::ApprovalResolution::approve());
                } else if (a.kind == "question") {
                    out.questions.push_back(jstr(a.payload, "prompt"));
                    (void)(*rt)->answer_question(a.id, "Stop");  // nobody is at the agent browser to sign in
                }
            }
            std::this_thread::sleep_for(200ms);
        }
        out.reason = (*rt)->run(*id)->reason;
        for (const auto& ev : (*rt)->events(*id))
            if (ev.kind == "tool_call") out.calls.push_back(jstr(ev.payload, "tool"));
        const auto conv = (*rt)->conversation(*thread);
        if (!conv.empty() && conv.back().role == "assistant") out.answer = conv.back().content;
        (*rt)->shutdown();
        service->shutdown();

        // For the record (design.md, task 1.2).
        std::vector<size_t> sizes = out.snapshot_chars;
        std::sort(sizes.begin(), sizes.end());
        const size_t median = sizes.empty() ? 0 : sizes[sizes.size() / 2];
        std::printf("[ browser %s ] state=%s approvals=%zu questions=%zu snapshots=%zu median=%zu chars (~%zu tokens) max=%zu\n",
                    name.c_str(), runtime::to_string(out.state), out.approvals.size(), out.questions.size(), sizes.size(), median,
                    median / 4, sizes.empty() ? 0 : sizes.back());
        std::printf("  calls:");
        for (const auto& c : out.calls) std::printf(" %s", c.c_str());
        std::printf("\n  answer: %s\n", utf8_truncate(out.answer, 400).c_str());
        if (!out.reason.empty()) std::printf("  reason: %s\n", out.reason.c_str());
        return out;
    }

    std::unique_ptr<model::TokenManagerAuth> auth;
    model::WinHttpTransport transport;
    std::unique_ptr<model::ModelClient> client;
};

bool mentions_browser_page(const json& approval, const std::string& host_suffix) {
    const json* ctx = approval.contains("untrusted_context") ? &approval["untrusted_context"] : nullptr;
    if (!ctx || !ctx->is_array()) return false;
    for (const auto& p : *ctx)
        if (jstr(p, "source_kind") == "browser" && ends_with(browser::url_host(jstr(p, "source_id")), host_suffix)) return true;
    return false;
}

}  // namespace

// Task 5.1: read a table, fill the search form, and ask for approval on submit with the page as provenance.
TEST_F(BrowserE2E, WikipediaTableAndSearchFormWithApprovalOnSubmit) {
    const Outcome o = run_task(
        "wikipedia", {"wikipedia.org", "*.wikipedia.org", "*.wikimedia.org"},
        "Use the agent browser. Open https://en.wikipedia.org/wiki/%C5%A0koda_Auto and tell me, from the infobox table on "
        "that page, the year the organization was founded. Then type 'Acme Falcon' into the Wikipedia search box on that page "
        "and submit the search, and tell me the first sentence of the article you land on.");
    EXPECT_EQ(o.state, runtime::RunState::succeeded) << o.reason;
    // The infobox's founding year (1895 for Laurin & Klement, 1896 in the current article).
    EXPECT_TRUE(o.answer.find("1895") != std::string::npos || o.answer.find("1896") != std::string::npos) << o.answer;
    EXPECT_NE(to_lower(o.answer).find("falcon"), std::string::npos) << o.answer;
    EXPECT_FALSE(o.snapshot_chars.empty());
    ASSERT_FALSE(o.approvals.empty()) << "submitting the search form must need the user's approval";
    bool page_shown = false;
    for (const auto& a : o.approvals) {
        EXPECT_TRUE(starts_with(jstr(a, "tool"), "browser.")) << a.dump();
        page_shown |= mentions_browser_page(a, "wikipedia.org");
    }
    EXPECT_TRUE(page_shown) << "the Approval shows the Wikipedia page as provenance";
}

// Task 1.2: everyday public sites. Bot checks and consent walls are acceptable outcomes when the agent hands them over.
TEST_F(BrowserE2E, GoogleSearch) {
    const Outcome o = run_task(
        "google", {"google.com", "*.google.com", "*.gstatic.com", "*.googleapis.com", "google.cz", "*.google.cz"},
        "Use the agent browser. Open https://www.google.com, search for 'Acme Auto Springfield plant' and tell me the "
        "title of the first search result. If a cookie consent dialog appears, choose the option that rejects all cookies.");
    EXPECT_TRUE(o.state == runtime::RunState::succeeded || !o.questions.empty()) << o.reason;
}

TEST_F(BrowserE2E, AmazonSearch) {
    const Outcome o = run_task(
        "amazon", {"amazon.com", "*.amazon.com", "*.media-amazon.com", "*.ssl-images-amazon.com"},
        "Use the agent browser. Open https://www.amazon.com, search for 'usb-c cable' and tell me the name and price of the "
        "first result. Do not add anything to the cart and do not buy anything.");
    EXPECT_TRUE(o.state == runtime::RunState::succeeded || !o.questions.empty()) << o.reason;
    for (const auto& c : o.calls) EXPECT_NE(c, "browser.eval");
}

// Task 1.1: a page behind the organization identity provider (OAuth) raises the sign-in hand-over. Chrome reaches the
// organization host through WORKBENCH_DEV_PROXY here, because this workstation blocks unsigned processes (the egress proxy)
// from organization hosts; the request interception still applies the egress rules.
TEST_F(BrowserE2E, CompanyIdentityProviderSignInIsHandedOver) {
    const std::string dev_proxy = env_var("WORKBENCH_DEV_PROXY");
    if (dev_proxy.empty()) GTEST_SKIP() << "set WORKBENCH_DEV_PROXY";
    const std::string issuer = e2e::env()->issuer;
    const std::string idp = browser::url_host(issuer);
    e2e::TempStore ts;
    const auto p = make_policy({idp});
    browser::BrowserEnv be;
    be.policy = [p] { return p; };
    be.data_root = ts.root();
    be.quarantine_dir = narrow((ts.root() / L"quarantine").wstring());
    be.idp_hosts = {idp};
    // A window (off-screen): on a workstation with organization certificates Chrome asks which one to send to the
    // identity provider, and that wait is handed over as well.
    be.extra_chrome_args = {L"--proxy-server=" + widen(dev_proxy), L"--window-position=-2400,-2400", L"--window-size=1280,900"};
    auto service = std::make_shared<browser::BrowserService>(be);
    ASSERT_TRUE(service->offered()) << service->state().reason;
    std::printf("Chrome %s; machine Chrome policy allows debugging\n", service->state().version.c_str());
    runtime::RunControl control;
    runtime::ToolContext ctx;
    ctx.run_id = "idp";
    ctx.control = &control;
    // The account console of the realm redirects to the identity provider's sign-in page.
    auto r = service->navigate(ctx, {{"url", issuer + "/account"}});
    ASSERT_TRUE(r.question) << r.content;
    EXPECT_NE(r.question->prompt.find("sign in"), std::string::npos);
    EXPECT_EQ(r.question->choices, (std::vector<std::string>{"Continue", "Stop"}));
    auto stop = service->resume_handover(ctx, json::object(), r.question->state, "Stop");
    EXPECT_FALSE(stop.ok);
    EXPECT_TRUE(control.cancelled());
    service->shutdown();
}
