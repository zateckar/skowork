// client/mail-calendar (add-mail-calendar 3.x, 4.x, 5.x, 6.2): configuration, labels and Trigger conditions as unit
// tests, and the OAuth flow, the Graph layer, sync, the event feed and the tools against a dev/graph-mock process (node),
// on a real encrypted app.db. The mock tests skip when node or dev/graph-mock/node_modules is missing (npm ci there).
#include "support/dev_endpoints.h"
#include "support/graph_mock.h"

#include "extract/extract.h"
#include "identity/loopback_listener.h"
#include "mail/filter.h"
#include "mail/graph_parse.h"
#include "mail/mail_service.h"
#include "policy/core.h"
#include "policy/decide.h"
#include "scheduler/mail_sources.h"

#include <algorithm>
#include <climits>
#include <map>
#include <set>
#include <thread>

using namespace wb;
using namespace wb::mail;
using namespace std::chrono_literals;

namespace {

constexpr long long kHour = 3'600'000, kDay = 86'400'000;

// The mail service against a fresh mock, signed in as jan on Connect.
struct Fixture {
    explicit Fixture(const json& extra = json::object()) {
        policy = mock.policy();
        for (const auto& [k, v] : extra.items()) policy[k] = v;
        make();
    }
    ~Fixture() { svc.reset(); }

    // A new MailService on the database (after db.open(): a restarted application).
    void make() {
        svc.reset();
        MailEnv e;
        e.store = db.store.get();
        e.clock = &system_clock();
        e.launcher = &browser;
        e.policy = policy_from(&policy);
        e.dev_build = true;
        e.login_hint = "jan";
        e.background_sync = background;
        e.paused = [this] { return paused.load(); };
        e.interaction_required = [this] { ++questions; };
        e.extract = [](const std::string& kind, const std::string& bytes, runtime::ToolContext&) -> Result<std::string> {
            if (kind == "pdf") return extract::extract_pdf(bytes);
            return extract::extract_document(kind, bytes);
        };
        svc = std::make_unique<MailService>(std::move(e));
        svc->on_event([this](const MailEvent& ev) {
            const std::lock_guard lock(mx);
            events.push_back(ev);
        });
        svc->start();
        tools.clear();
        for (auto& t : svc->tools()) tools[t.id] = std::move(t);
    }
    void restart() {
        svc.reset();
        db.open();
        make();
    }
    bool connect() {
        auto s = svc->connect();
        EXPECT_TRUE(s) << (s ? "" : s.error().message);
        return svc->connected();
    }
    // Retry delays are recorded instead of waited for.
    void record_delays() {
        svc->connection()->graph->set_sleeper([this](std::chrono::milliseconds d, const std::shared_ptr<CancelToken>&) {
            slept.push_back(d.count());
            return true;
        });
    }
    json run(const std::string& tool, const json& args, bool* ok = nullptr) {
        runtime::ToolContext ctx;
        ctx.call_id = "call" + std::to_string(++calls);
        ctx.activity = [this](const std::string& t) { activity.push_back(t); };
        auto out = tools.at(tool).run(ctx, args);
        if (ok) *ok = out.ok;
        last_provenance = out.provenance;
        auto j = parse_json(out.content);
        return j ? *j : json(out.content);
    }
    std::vector<MailEvent> take_events() {
        const std::lock_guard lock(mx);
        return std::exchange(events, {});
    }
    // Midnight (UTC) of the mock's anchor day plus `days`.
    [[nodiscard]] long long day(int days) const { return mock.anchor / kDay * kDay + days * kDay; }

    GraphMockProcess mock;
    MailDb db;
    bool background = false;  // tests drive sync_now() unless they test the background loop
    std::atomic<bool> paused{false};
    FollowingBrowser browser;
    json policy;
    std::unique_ptr<MailService> svc;
    std::map<std::string, runtime::ToolDef> tools;
    std::atomic<int> questions{0};
    std::vector<std::string> activity;
    std::vector<long long> slept;
    std::optional<runtime::Provenance> last_provenance;
    int calls = 0;
    std::mutex mx;
    std::vector<MailEvent> events;
};

#define REQUIRE_MOCK()                                                                                    \
    do {                                                                                                  \
        if (!graph_mock_ready()) GTEST_SKIP() << "node or dev/graph-mock/node_modules missing (npm ci there)"; \
    } while (false)

int count_kind(const std::vector<MailEvent>& ev, const std::string& kind, bool include_backfill = true) {
    int n = 0;
    for (const auto& e : ev)
        if (e.kind == kind && (include_backfill || !jbool(e.detail, "backfill", false))) ++n;
    return n;
}

const json* find_by(const json& list, const char* key, const std::string& value) {
    for (const auto& x : list)
        if (jstr(x, key).find(value) != std::string::npos) return &x;
    return nullptr;
}

std::string iso_ms(long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); }

json week_from_now() { return {{"start", iso8601_utc_now()}, {"end", iso_ms(unix_millis_now() + 6 * kDay)}}; }

}  // namespace

// ---- configuration, labels, Trigger conditions (no mock) ------------------------------------------------------------------

TEST(MailConfig, ReleaseBuildRejectsMockAuthAndNonGraphHosts) {
    std::map<std::string, json> p = {{"mail.oauth.authority", "https://login.microsoftonline.com"},
                                     {"mail.oauth.tenant_id", "t"},
                                     {"mail.oauth.client_id", "c"}};
    const PolicyLookup look = [&p](const std::string& k) -> std::optional<json> {
        const auto it = p.find(k);
        return it == p.end() ? std::nullopt : std::optional<json>(it->second);
    };
    auto c = read_config(look, {}, false);
    EXPECT_TRUE(c.available);
    EXPECT_EQ(c.graph_base_url, "https://graph.microsoft.com/v1.0");

    // Mock Auth In Release Build
    p["mail.auth"] = "mock";
    c = read_config(look, {}, false);
    EXPECT_FALSE(c.available);
    EXPECT_EQ(c.unavailable_code, "policy_error");
    EXPECT_TRUE(read_config(look, {}, true).available);

    // Non-Graph Host In Release Build
    p.erase("mail.auth");
    p["mail.graph_base_url"] = "http://127.0.0.1:4810/v1.0";
    EXPECT_EQ(read_config(look, {}, false).unavailable_code, "policy_error");
    EXPECT_TRUE(read_config(look, {}, true).available);
    p["mail.graph_base_url"] = "https://graph.example.com/v1.0";
    EXPECT_EQ(read_config(look, {}, true).unavailable_code, "policy_error");
    p["mail.graph_base_url"] = "https://graph.microsoft.us/v1.0";
    EXPECT_TRUE(read_config(look, {}, false).available);

    // OAuth Configuration Missing
    p.erase("mail.graph_base_url");
    p["mail.oauth.client_id"] = "";
    c = read_config(look, {}, false);
    EXPECT_FALSE(c.available);
    EXPECT_EQ(c.unavailable_code, "not_configured");
}

TEST(MailConfig, ScopesFollowWhatPolicyEnables) {
    const PolicyLookup look = [](const std::string&) -> std::optional<json> { return std::nullopt; };
    auto c = read_config(look, [](const std::string&) { return true; }, true);
    EXPECT_EQ(c.scopes, (std::vector<std::string>{"User.Read", "Mail.ReadWrite", "Mail.Send", "Calendars.ReadWrite", "MailboxSettings.Read",
                                                  "User.Read.All"}));
    EXPECT_NE(c.scope_parameter().find("https://graph.microsoft.com/Mail.ReadWrite"), std::string::npos);
    EXPECT_NE(c.scope_parameter().find("offline_access"), std::string::npos);
    // Send Disabled By Policy: no Mail.Send.
    c = read_config(look, [](const std::string& t) { return t != "mail.send"; }, true);
    EXPECT_EQ(std::count(c.scopes.begin(), c.scopes.end(), "Mail.Send"), 0);
    // Directory enrichment off (client/knowledge-graph Enrichment disabled): no directory scope.
    const PolicyLookup no_dir = [](const std::string& k) -> std::optional<json> {
        if (k == "knowledge.directory_enrichment") return json("off");
        return std::nullopt;
    };
    c = read_config(no_dir, [](const std::string&) { return true; }, true);
    EXPECT_EQ(std::count(c.scopes.begin(), c.scopes.end(), "User.Read.All"), 0);
    EXPECT_FALSE(c.directory_enabled);
}

TEST(MailLabels, EnabledLabelFromMsipLabelsAndDispositions) {
    EXPECT_EQ(parse_msip_labels("MSIP_Label_0F1E7B2A-1C39-4A0D-9D41-7A3F00000004_Enabled=true; MSIP_Label_0f1e7b2a-1c39-4a0d-9d41-7a3f00000004_Method=Standard"),
              "0f1e7b2a-1c39-4a0d-9d41-7a3f00000004");
    EXPECT_EQ(parse_msip_labels("MSIP_Label_0f1e7b2a-1c39-4a0d-9d41-7a3f00000004_Enabled=false"), "");
    EXPECT_EQ(parse_msip_labels(""), "");
    const json labelled = {{"id", "m1"},
                           {"subject", "Board"},
                           {"singleValueExtendedProperties",
                            {{{"id", kLabelProperty}, {"value", "MSIP_Label_0f1e7b2a-1c39-4a0d-9d41-7a3f00000004_Enabled=true"}}}}};
    const json plain = {{"id", "m2"}, {"subject", "Hi"}, {"categories", {"HR"}}};
    Rules r;
    r.excluded_labels = {"0f1e7b2a-1c39-4a0d-9d41-7a3f00000004"};
    EXPECT_EQ(classify(r, labelled), Disposition::withheld);
    EXPECT_EQ(classify(r, plain), Disposition::include);
    r.exclude_unlabelled = true;  // Unlabelled Items Excluded
    EXPECT_EQ(classify(r, plain), Disposition::withheld);
    r.categories = {"hr"};
    EXPECT_EQ(classify(r, plain), Disposition::excluded);
    r.folders = {"osobní"};
    EXPECT_EQ(classify(Rules{}, plain, {"", "Osobní"}), Disposition::include);
    EXPECT_EQ(classify(r, json{{"id", "m3"}}, {"", "Osobní"}), Disposition::excluded);
    auto m = parse_message(labelled, r, {});
    ASSERT_TRUE(m);
    EXPECT_TRUE(m->withheld);
    EXPECT_TRUE(m->subject.empty());
    EXPECT_EQ(jstr(message_json(*m, true), "withheld"), "withheld (sensitivity label)");
    EXPECT_FALSE(message_json(*m, true).contains("subject"));
}

TEST(MailTime, Iso8601Parsing) {
    EXPECT_EQ(parse_iso8601_ms("1970-01-01T00:00:00Z"), 0);
    EXPECT_EQ(parse_iso8601_ms("2026-10-27T07:30:00.0000000"), parse_iso8601_ms("2026-10-27T07:30:00Z"));
    EXPECT_EQ(*parse_iso8601_ms("2026-10-27T08:30:00+01:00"), *parse_iso8601_ms("2026-10-27T07:30:00Z"));
    EXPECT_EQ(*parse_iso8601_ms("2026-10-27T07:30:00.250Z") - *parse_iso8601_ms("2026-10-27T07:30:00Z"), 250);
    EXPECT_FALSE(parse_iso8601_ms("27.10.2026"));
    EXPECT_FALSE(parse_iso8601_ms("2026-10-27T07:30:00Zjunk"));
    EXPECT_FALSE(parse_iso8601_ms("0001-01-01T00:00:00Z"));  // Graph's "no date"; beyond sys_clock (found by fuzz_graph_parse)
    EXPECT_EQ(iso8601_graph(*parse_iso8601_ms("2026-10-27T07:30:00Z")), "2026-10-27T07:30:00.0000000");
}

TEST(MailSources, ConditionsAndMatching) {
    using namespace wb::scheduler;
    EXPECT_TRUE(validate_mail_conditions({{"from", "eva"}, {"has_attachments", true}}));
    EXPECT_FALSE(validate_mail_conditions({{"sender", "eva"}}));
    EXPECT_FALSE(validate_calendar_conditions({{"at", "middle"}}));
    EXPECT_FALSE(validate_calendar_conditions({{"offset_minutes", 5000}}));
    EXPECT_TRUE(validate_calendar_conditions({{"at", "start"}, {"offset_minutes", -15}, {"external_attendees", true}}));
    const json msg = {{"subject", "Cenová nabídka"},
                      {"from", {{"name", "Petra Malá"}, {"address", "petra@dodavatel.example"}}},
                      {"has_attachments", true},
                      {"folder", "inbox"}};
    EXPECT_TRUE(mail_matches({{"from", "petra"}, {"subject_contains", "nabidka"}}, msg));  // diacritics folded
    EXPECT_FALSE(mail_matches({{"folder", "archive"}}, msg));
    const json ev = {{"subject", "Servis"}, {"attendees", {{{"address", "petra@dodavatel.example"}}, {{"address", "jan@acme-test.example"}}}}};
    EXPECT_TRUE(calendar_matches({{"external_attendees", true}}, ev, "acme-test.example"));
    EXPECT_FALSE(calendar_matches({{"external_attendees", false}}, ev, "acme-test.example"));
}

// ---- 3.1 OAuth against the mock ------------------------------------------------------------------------------------------

TEST(MailOAuth, SignInRestartInteractionRequiredAndDisconnect) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_FALSE(f.mock.graph.empty());
    EXPECT_EQ(jstr(f.svc->status(), "state"), "disconnected");
    EXPECT_EQ(f.browser.opened, 0);  // never a prompt on start
    ASSERT_TRUE(f.connect());
    EXPECT_EQ(f.browser.landing_status, 200);
    EXPECT_NE(f.browser.last_url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(f.browser.last_url.find("client_id=skowork-dev"), std::string::npos);
    EXPECT_EQ(f.browser.redirect().rfind("http://localhost:", 0), 0u) << f.browser.redirect();
    // The listener is gone after the redirect: its port can be bound again.
    EXPECT_FALSE(f.svc->connection()->auth->listener_active());
    EXPECT_TRUE(LoopbackListener::start(f.browser.redirect(), {}, 200ms)) << "the sign-in port is still held";
    // Access tokens stay in memory; the refresh token is stored encrypted.
    EXPECT_TRUE(f.db.store->secret("mail.refresh_token"));
    auto r = f.svc->sync_now();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(f.svc->account(), "jan.novak@acme-test.example");

    // Restart with the stored refresh token: connected without a prompt.
    f.browser.opened = 0;
    f.restart();
    EXPECT_TRUE(f.svc->connected());
    auto r2 = f.svc->sync_now();
    ASSERT_TRUE(r2) << r2.error().message;
    EXPECT_EQ(f.browser.opened, 0);

    // Interaction Required: the refresh needs the user. The Connect mailbox Question is raised once; no browser.
    f.mock.ctl("POST", "/oauth/faults", {{"kind", "interaction_required"}, {"user", "jan"}, {"step", "refresh"}, {"count", 1}});
    f.restart();
    bool ok = true;
    f.run("mail.search", {{"query", "Budget"}}, &ok);
    EXPECT_FALSE(ok);
    f.run("mail.search", {{"query", "Budget"}}, &ok);
    EXPECT_EQ(jstr(f.svc->status(), "state"), "interaction_required");
    EXPECT_EQ(f.questions, 1);
    EXPECT_EQ(f.browser.opened, 0);
    EXPECT_TRUE(f.db.store->secret("mail.refresh_token"));

    // Disconnect mailbox: tokens and the local copy are gone.
    f.svc->disconnect();
    EXPECT_FALSE(f.db.store->secret("mail.refresh_token"));
    EXPECT_EQ(f.svc->store().counts().messages, 0);
    EXPECT_FALSE(f.svc->connected());
}

TEST(MailOAuth, AbandonedSignInClosesTheListener) {
    REQUIRE_MOCK();
    Fixture f;
    f.browser.follow = false;  // the user closes the browser
    auto conn = f.svc->connection();
    conn->auth->set_sign_in_timeout(400ms);
    ASSERT_TRUE(f.svc->connect());
    EXPECT_TRUE(conn->auth->listener_active());
    EXPECT_EQ(jstr(f.svc->status(), "state"), "connecting");
    const std::string redirect = conn->auth->redirect_uri();
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (conn->auth->listener_active() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(conn->auth->listener_active());
    EXPECT_EQ(jstr(f.svc->status(), "state"), "disconnected");
    EXPECT_TRUE(LoopbackListener::start(redirect, {}, 200ms));
}

TEST(MailOAuth, MissingConfigurationDisablesMail) {
    REQUIRE_MOCK();
    Fixture f({{"mail.oauth.client_id", ""}});
    EXPECT_FALSE(f.svc->available());
    EXPECT_FALSE(f.tools.at("mail.search").offered());
    EXPECT_FALSE(f.svc->connect());
    EXPECT_EQ(f.browser.opened, 0);
}

// ---- 3.2 the Graph layer -------------------------------------------------------------------------------------------------

TEST(MailGraph, RetryAfterBackoffPagingAndHostChecks) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    f.record_delays();
    auto conn = f.svc->connection();
    // Throttled Request: Retry-After 10 s is honoured and reported in the Activity log.
    f.mock.ctl("POST", "/faults", {{"kind", "throttle"}, {"path", "/me/messages"}, {"method", "GET"}, {"retry_after", 10}, {"count", 1}});
    bool ok = false;
    f.run("mail.search", {{"query", "Budget"}}, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(f.slept.size(), 1u);
    EXPECT_GE(f.slept[0], 10'000);
    ASSERT_EQ(f.activity.size(), 1u);
    EXPECT_NE(f.activity[0].find("retrying in 10 s"), std::string::npos) << f.activity[0];
    // Transient server errors: capped exponential backoff, then success.
    f.slept.clear();
    f.mock.ctl("POST", "/faults", {{"kind", "server_error"}, {"path", "/me/mailFolders"}, {"count", 2}});
    GraphCall c;
    c.path = "/me/mailFolders";
    EXPECT_TRUE(conn->graph->call(c));
    ASSERT_EQ(f.slept.size(), 2u);
    EXPECT_GE(f.slept[0], 500);
    EXPECT_GT(f.slept[1], f.slept[0] / 2);
    EXPECT_LE(f.slept[1], 30'000);
    // Paging to the end, and up to a limit.
    GraphCall p;
    p.path = "/me/messages?$top=2";
    bool truncated = true;
    auto all = conn->graph->list(p, 1000, &truncated);
    ASSERT_TRUE(all);
    EXPECT_GE(all->size(), 9u);
    EXPECT_FALSE(truncated);
    auto few = conn->graph->list(p, 3, &truncated);
    ASSERT_TRUE(few);
    EXPECT_EQ(few->size(), 3u);
    EXPECT_TRUE(truncated);
    // A malformed body is an error, not a crash.
    f.mock.ctl("POST", "/faults", {{"kind", "malformed"}, {"path", "/me/mailFolders"}, {"count", 1}});
    auto bad = conn->graph->call(c);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, "malformed");
    // A link to another host is refused; a release build refuses the loopback mock itself.
    GraphCall evil;
    evil.path = "https://evil.example/v1.0/me/messages";
    auto e = conn->graph->call(evil);
    ASSERT_FALSE(e);
    EXPECT_EQ(e.error().code, "policy_error");
    GraphClient release(conn->cfg.graph_base_url, *conn->auth, /*dev_build=*/false);
    GraphCall me;
    me.path = "/me";
    auto rel = release.call(me);
    ASSERT_FALSE(rel);
    EXPECT_EQ(rel.error().code, "policy_error");
}

// ---- 5.1-5.3 sync, the event feed and offline search -----------------------------------------------------------------------

TEST(MailSync, DeltaSyncLabelsExpiryAndEventsWithoutDuplicates) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    // Strictly confidential is excluded; the Osobní folder and the HR category are excluded by policy.
    Fixture f({{"mail.excluded_labels", {"0f1e7b2a-1c39-4a0d-9d41-7a3f00000004"}},
               {"mail.exclusions.folders", {"Osobní"}},
               {"mail.exclusions.categories", {"HR"}}});
    ASSERT_TRUE(f.connect());
    auto r = f.svc->sync_now();
    ASSERT_TRUE(r) << r.error().message;
    auto ev = f.take_events();
    // The first sync fills the store; its mail is backfill, not new mail.
    EXPECT_EQ(count_kind(ev, "mail.received", false), 0);
    EXPECT_GE(count_kind(ev, "mail.received"), 5);
    EXPECT_GE(count_kind(ev, "event.created"), 3);
    EXPECT_FALSE(std::any_of(ev.begin(), ev.end(), [](const MailEvent& e) { return e.detail.contains("withheld") || jbool(e.detail, "private", false); }));
    // Withheld items keep no content; excluded ones and those outside the window are not stored.
    int withheld = 0;
    for (const auto& m : f.svc->store().search({})) {
        if (m.withheld) {
            ++withheld;
            EXPECT_TRUE(m.subject.empty());
            EXPECT_TRUE(m.body.empty());
        }
        EXPECT_NE(m.subject, "Mittagessen Freitag?");
        EXPECT_NE(m.subject, "Hodnocení zaměstnanců 2026");
        EXPECT_NE(m.subject, "Archiv: Jahresplanung 2025");
    }
    EXPECT_EQ(withheld, 1);
    // Private Calendar Item: only busy time is stored.
    bool masked = false;
    for (const auto& e : f.svc->store().events_between(0, LLONG_MAX))
        if (e.masked) {
            masked = true;
            EXPECT_TRUE(e.subject.empty());
            EXPECT_TRUE(e.body.empty());
        }
    EXPECT_TRUE(masked);

    // New Mail Arrives: one "received" in the next cycle.
    f.mock.ctl("POST", "/users/jan/messages",
               {{"from", "Petra Malá <petra.mala@dodavatel-test.example>"}, {"subject", "Doplnění nabídky"}, {"body", "<p>Doplňuji.</p>"}, {"label", "public"}});
    ASSERT_TRUE(f.svc->sync_now());
    ev = f.take_events();
    EXPECT_EQ(count_kind(ev, "mail.received", false), 1);
    EXPECT_EQ(ev.size(), 1u);

    // Event Rescheduled: one "updated" with the new times.
    bool ok = false;
    const json cal = f.run("calendar.list", week_from_now(), &ok);
    ASSERT_TRUE(ok) << cal.dump();
    const json* review = find_by(cal["events"], "subject", "Budget Q4 review");
    ASSERT_TRUE(review);
    const std::string review_id = jstr(*review, "id");
    f.mock.ctl("PATCH", "/users/jan/events/" + review_id, {{"start", "+5d 15:00"}});
    ASSERT_TRUE(f.svc->sync_now());
    ev = f.take_events();
    ASSERT_EQ(count_kind(ev, "event.updated"), 1);
    EXPECT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].id, review_id);
    EXPECT_EQ(ev[0].start, f.day(5) + 15 * kHour);
    EXPECT_EQ(ev[0].end - ev[0].start, kHour);

    // Delta Token Expired: a full resync, and nothing is announced again.
    f.mock.ctl("POST", "/delta/expire");
    auto again = f.svc->sync_now();
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_GE(again->resyncs, 1);
    ev = f.take_events();
    EXPECT_TRUE(ev.empty()) << ev.front().kind << " " << ev.front().id;

    // Cancellation and deletion are announced.
    f.mock.ctl("POST", "/users/jan/events/" + review_id + "/cancel");
    const json found = f.run("mail.search", {{"query", "Doplnění"}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(found["results"].empty());
    f.mock.ctl("DELETE", "/users/jan/messages/" + jstr(found["results"][0], "id"));
    ASSERT_TRUE(f.svc->sync_now());
    ev = f.take_events();
    EXPECT_EQ(count_kind(ev, "event.cancelled"), 1);
    EXPECT_EQ(count_kind(ev, "mail.deleted"), 1);
}

TEST(MailSync, FailedListenerIsNotReplayedAfterRestart) {
    // Characterizes the current in-memory listener; replace when page-atomic intake is implemented.
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    ASSERT_TRUE(f.svc->sync_now());
    (void)f.take_events();
    int attempts = 0;
    f.svc->on_event([&](const MailEvent& event) {
        if (event.kind == "mail.received" && !jbool(event.detail, "backfill", false)) {
            ++attempts;
            throw std::runtime_error("simulated subscriber storage failure");
        }
    });
    f.mock.ctl("POST", "/users/jan/messages",
               {{"from", "Petra Malá <petra.mala@dodavatel-test.example>"},
                {"subject", "Subscriber failure"}, {"body", "test"}, {"label", "public"}});
    ASSERT_TRUE(f.svc->sync_now());
    EXPECT_EQ(attempts, 1);
    f.restart();
    ASSERT_TRUE(f.svc->sync_now());
    const auto events = f.take_events();
    EXPECT_EQ(count_kind(events, "mail.received", false), 1);
}

TEST(MailSync, UnlabelledExcludedAndOfflineSearchFromTheLocalCopy) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f({{"mail.unlabelled", "exclude"}});
    ASSERT_TRUE(f.connect());
    ASSERT_TRUE(f.svc->sync_now());
    int unlabelled = 0;
    for (const auto& m : f.svc->store().search({}))
        if (m.label.empty()) {
            ++unlabelled;
            EXPECT_TRUE(m.withheld) << m.id;  // the newsletter
        }
    EXPECT_GE(unlabelled, 1);
    // Search While Offline: the mock stops; results come from the local copy with the time of the last sync.
    f.record_delays();
    f.mock.kill();
    bool ok = false;
    const json out = f.run("mail.search", {{"query", "Budget"}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_TRUE(jbool(out, "offline", false));
    EXPECT_NE(jstr(out, "last_sync"), "never");
    EXPECT_GE(out["results"].size(), 2u);
    ASSERT_TRUE(f.last_provenance);
    EXPECT_FALSE(f.last_provenance->trusted);
    // Changes fail with "offline".
    const json sent = f.run("mail.send", {{"to", {"eva.schneider@acme-test.example"}}, {"subject", "x"}, {"body", "y"}}, &ok);
    EXPECT_FALSE(ok);
    ASSERT_TRUE(sent.is_string());
    EXPECT_EQ(sent.get<std::string>().rfind("offline:", 0), 0u);
    const json del = f.run("mail.delete", {{"id", "x"}}, &ok);
    EXPECT_FALSE(ok);
    ASSERT_TRUE(del.is_string());
    EXPECT_EQ(del.get<std::string>().rfind("offline:", 0), 0u);
}

// Background sync is deferred while background work is paused (client/desktop-host), and runs when it resumes.
TEST(MailSync, BackgroundSyncDefersWhilePaused) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    f.background = true;
    f.paused = true;
    f.make();
    ASSERT_TRUE(f.connect());  // a connected mailbox asks for a sync at once
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(f.svc->store().last_sync(), 0);
    EXPECT_EQ(f.svc->store().counts().messages, 0);
    f.paused = false;
    f.svc->request_sync();
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (f.svc->store().last_sync() == 0 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(50ms);
    EXPECT_GT(f.svc->store().last_sync(), 0);
    EXPECT_GT(f.svc->store().counts().messages, 0);
}

// ---- 4.1-4.3 the tools ----------------------------------------------------------------------------------------------------

TEST(MailTools, SearchReadAttachmentsWithheldAndSyncWindow) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f({{"mail.excluded_labels", {"0f1e7b2a-1c39-4a0d-9d41-7a3f00000004"}}, {"mail.sync_window.mail_days", 30}});
    ASSERT_TRUE(f.connect());
    bool ok = false;
    // Strictly Confidential Message: listed as withheld, without subject, body or sender.
    json out = f.run("mail.search", {{"query", "plant"}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    ASSERT_EQ(out["results"].size(), 1u) << out.dump();
    EXPECT_EQ(jstr(out["results"][0], "withheld"), "withheld (sensitivity label)");
    EXPECT_FALSE(out["results"][0].contains("subject"));
    EXPECT_FALSE(out["results"][0].contains("from"));
    ASSERT_TRUE(f.last_provenance);
    EXPECT_FALSE(f.last_provenance->trusted);
    EXPECT_EQ(f.last_provenance->source_kind, "mail");
    out = f.run("mail.read", {{"id", jstr(out["results"][0], "id")}}, &ok);
    EXPECT_FALSE(out.contains("body"));

    // Read A Pdf Attachment: its text through Document Text Extraction, as untrusted content.
    out = f.run("mail.search", {{"query", "nabídka"}}, &ok);
    ASSERT_TRUE(ok);
    const json* offer = find_by(out["results"], "subject", "Cenová nabídka");
    ASSERT_TRUE(offer) << out.dump();
    const std::string offer_id = jstr(*offer, "id");
    out = f.run("mail.read", {{"id", offer_id}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_NE(jstr(out, "body").find("servis linky 4"), std::string::npos);
    ASSERT_EQ(out["attachments"].size(), 1u);
    out = f.run("mail.read", {{"id", offer_id}, {"attachment_id", jstr(out["attachments"][0], "id")}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_NE(jstr(out, "text").find("184 500 CZK"), std::string::npos) << out.dump();
    EXPECT_FALSE(f.last_provenance->trusted);

    // A docx attachment, and the whole conversation.
    out = f.run("mail.search", {{"query", "Entwurf"}, {"has_attachments", true}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out["results"].size(), 1u) << out.dump();
    const std::string budget = jstr(out["results"][0], "id");
    out = f.run("mail.read", {{"id", budget}}, &ok);
    ASSERT_EQ(out["attachments"].size(), 1u);
    out = f.run("mail.read", {{"id", budget}, {"attachment_id", jstr(out["attachments"][0], "id")}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_NE(jstr(out, "text").find("Lizenzen 48 000 EUR"), std::string::npos) << out.dump();
    out = f.run("mail.read", {{"id", budget}, {"thread", true}}, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(out["messages"].size(), 2u);

    // Old Message Requested: outside the 30-day window it is read live and not stored.
    ASSERT_TRUE(f.svc->sync_now());
    out = f.run("mail.search", {{"query", "Jahresplanung"}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out["results"].size(), 1u);
    const std::string old_id = jstr(out["results"][0], "id");
    out = f.run("mail.read", {{"id", old_id}}, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(jstr(out, "subject"), "Archiv: Jahresplanung 2025");
    EXPECT_FALSE(f.svc->store().message(old_id));
    EXPECT_TRUE(f.svc->store().message(offer_id));
}

TEST(MailTools, DraftSendApprovalFloorMoveAndDelete) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    ASSERT_TRUE(f.svc->sync_now());
    bool ok = false;
    json out = f.run("mail.search", {{"query", "licence"}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out["results"].size(), 1u) << out.dump();
    const std::string tom_msg = jstr(out["results"][0], "id");
    // Agent Drafts A Reply: saved in Drafts, nothing sent.
    out = f.run("mail.draft", {{"reply_to", tom_msg}, {"body", "Thursday 10:00 works for me."}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_FALSE(jstr(out, "draft_id").empty());
    EXPECT_NE(jstr(out, "subject").find("Budget Q4"), std::string::npos);
    EXPECT_TRUE(f.mock.ctl("GET", "/sends").empty());
    out = f.run("mail.search", {{"folder", "drafts"}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    EXPECT_EQ(out["results"].size(), 1u);

    // Send Always Requires Approval: the decision for mail.send's declared Risk class from the real decision function.
    const auto& send = f.tools.at("mail.send");
    ASSERT_TRUE(send.risk);
    const auto decide = [&](const json& pol, bool unattended) {
        policy::PolicyBundle b;
        b.sub = "dev";
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = pol;
        const policy::EffectivePolicy p(std::move(b));
        policy::CallFacts c;
        c.tool = send.id;
        c.risk = runtime::to_string(*send.risk);
        c.unattended = unattended;
        return policy::decide(&p, c).decision;
    };
    const json allow_all = {{"tools", {{"risk", {{"external", "allow"}}}, {"rules", {{"mail.send", "allow"}}}}}};
    EXPECT_EQ(decide(allow_all, false), policy::Decision::approve);  // Interactive Send
    EXPECT_EQ(decide(allow_all, true), policy::Decision::approve);   // Unattended Send without an unattended rule
    EXPECT_EQ(decide({{"tools", {{"risk", {{"external", "allow"}}}}}, {"unattended", {{"rules", {{"mail.send", "allow"}}}}}}, true),
              policy::Decision::allow);
    EXPECT_EQ(f.tools.at("mail.delete").risk, runtime::Risk::destructive);
    EXPECT_EQ(f.tools.at("mail.move").risk, runtime::Risk::destructive);
    EXPECT_EQ(f.tools.at("mail.draft").risk, runtime::Risk::external);
    EXPECT_EQ(f.tools.at("mail.search").risk, runtime::Risk::read);

    // After approval exactly the approved message is sent, as a reply in the conversation.
    out = f.run("mail.send",
                {{"to", {"tom.baker@acme-test.example"}},
                 {"cc", {"eva.schneider@acme-test.example"}},
                 {"subject", "RE: Budget Q4"},
                 {"body", "Thursday 10:00, Room 4.12."},
                 {"reply_to", tom_msg}},
                &ok);
    ASSERT_TRUE(ok) << out.dump();
    const json sends = f.mock.ctl("GET", "/sends?kind=mail");
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_EQ(sends[0]["to"], json::array({"tom.baker@acme-test.example"}));
    EXPECT_EQ(sends[0]["cc"], json::array({"eva.schneider@acme-test.example"}));
    EXPECT_EQ(jstr(sends[0], "subject"), "RE: Budget Q4");
    EXPECT_EQ(jstr(sends[0], "body"), "Thursday 10:00, Room 4.12.");

    // Agent Deletes A Message: it moves to Deleted Items; a move goes to a folder by name.
    out = f.run("mail.search", {{"query", "Webinar"}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(out["results"].empty());
    out = f.run("mail.delete", {{"id", jstr(out["results"][0], "id")}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    out = f.run("mail.search", {{"folder", "deleteditems"}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out["results"].size(), 1u);
    EXPECT_EQ(jstr(out["results"][0], "subject"), "Webinar: Digital twins in manufacturing");
    out = f.run("mail.move", {{"id", jstr(out["results"][0], "id")}, {"folder", "Projekty"}}, &ok);
    EXPECT_TRUE(ok) << out.dump();
    out = f.run("mail.search", {{"folder", "Projekty"}}, &ok);
    EXPECT_NE(find_by(out["results"], "subject", "Webinar"), nullptr);
}

TEST(MailTools, CalendarListPrivateMaskingFreeTimeCreateAndRespond) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    bool ok = false;
    json out = f.run("calendar.list", week_from_now(), &ok);
    ASSERT_TRUE(ok) << out.dump();
    // Private Calendar Item: only its time, as busy.
    bool found_private = false;
    for (const auto& e : out["events"])
        if (jbool(e, "private", false)) {
            found_private = true;
            EXPECT_FALSE(e.contains("subject"));
            EXPECT_FALSE(e.contains("attendees"));
            EXPECT_EQ(jstr(e, "show_as"), "oof");
        }
    EXPECT_TRUE(found_private);
    EXPECT_NE(find_by(out["events"], "subject", "Budget Q4 review"), nullptr);
    // The online meeting keeps its join link (client/meeting-capture matches meetings by it); the room meeting has none.
    const json* supplier = find_by(out["events"], "subject", "Servis linky 4 – jednání");
    ASSERT_TRUE(supplier);
    EXPECT_NE(jstr(*supplier, "online_meeting_url").find("teams"), std::string::npos) << supplier->dump();
    EXPECT_EQ(jstr(*find_by(out["events"], "subject", "Budget Q4 review"), "online_meeting_url"), "");

    // Find A Meeting Slot: tomorrow 06:00-18:00 UTC, jan, eva and tom, 30 minutes; no slot overlaps anyone's busy time.
    const long long d1 = f.day(1);
    out = f.run("calendar.find_free_time",
                {{"attendees", {"eva.schneider@acme-test.example", "tom.baker@acme-test.example"}},
                 {"duration_minutes", 30},
                 {"start", iso_ms(d1 + 6 * kHour)},
                 {"end", iso_ms(d1 + 18 * kHour)},
                 {"working_hours_only", false},
                 {"max_results", 20}},
                &ok);
    ASSERT_TRUE(ok) << out.dump();
    ASSERT_FALSE(out["slots"].empty());
    std::vector<std::pair<long long, long long>> busy = {{d1 + 8 * kHour, d1 + 11 * kHour},                 // eva
                                                         {d1 + 10 * kHour, d1 + 14 * kHour},                // tom
                                                         {d1 + 12 * kHour, d1 + 13 * kHour + kHour / 2}};  // jan (private)
    if (const long long weekday = (d1 / kDay + 4) % 7; weekday >= 1 && weekday <= 5)
        busy.emplace_back(d1 + 7 * kHour + kHour / 2, d1 + 7 * kHour + 3 * kHour / 4);  // the daily stand-up
    for (const auto& s : out["slots"]) {
        const std::string start = jstr(s, "start");
        const auto a = parse_iso8601_ms(start), b = parse_iso8601_ms(jstr(s, "end"));
        ASSERT_TRUE(a && b) << start;
        EXPECT_EQ(*b - *a, 30 * 60'000);
        for (const auto& [x, y] : busy) EXPECT_FALSE(*a < y && *b > x) << start;
        ASSERT_GT(start.size(), 19u);
        EXPECT_TRUE(start[19] == '+' || start[19] == '-') << start;  // the user's time zone, with its offset
    }

    // Create A Meeting: invitations are sent once, also when the call is retried with the same call id.
    const json meeting = {{"subject", "Atlas calibration"},
                          {"start", iso_ms(d1 + 14 * kHour)},
                          {"end", iso_ms(d1 + 14 * kHour + kHour / 2)},
                          {"attendees", {"tom.baker@acme-test.example"}},
                          {"location", "Teams"}};
    runtime::ToolContext ctx;
    ctx.call_id = "fixed-call";
    const auto r1 = f.tools.at("calendar.create").run(ctx, meeting);
    const auto r2 = f.tools.at("calendar.create").run(ctx, meeting);
    ASSERT_TRUE(r1.ok) << r1.content;
    ASSERT_TRUE(r2.ok) << r2.content;
    EXPECT_EQ(jstr(*parse_json(r1.content), "id"), jstr(*parse_json(r2.content), "id"));
    const json invites = f.mock.ctl("GET", "/sends?kind=invitation");
    ASSERT_EQ(invites.size(), 1u);
    EXPECT_EQ(invites[0]["to"], json::array({"tom.baker@acme-test.example"}));
    EXPECT_EQ(f.tools.at("calendar.create").risk, runtime::Risk::external);
    EXPECT_EQ(f.tools.at("calendar.find_free_time").risk, runtime::Risk::read);

    // Respond to the budget review, organised by eva.
    out = f.run("calendar.list", week_from_now(), &ok);
    const json* review = find_by(out["events"], "subject", "Budget Q4 review");
    ASSERT_TRUE(review);
    const std::string review_id = jstr(*review, "id");
    out = f.run("calendar.respond", {{"id", review_id}, {"response", "tentative"}, {"comment", "Might be late"}}, &ok);
    ASSERT_TRUE(ok) << out.dump();
    const json responses = f.mock.ctl("GET", "/sends?kind=response");
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(jstr(responses[0], "comment"), "Might be late");
    out = f.run("calendar.get", {{"id", review_id}}, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(jstr(out, "location"), "Springfield, Room 4.12");
}

// ---- 6.2 end to end: reply to a thread and book a meeting -------------------------------------------------------------------

TEST(MailEndToEnd, ReplyToAThreadAndBookAMeeting) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    ASSERT_TRUE(f.svc->sync_now());
    f.take_events();
    bool ok = false;
    // The agent finds the thread, drafts a reply, finds a slot; the user approves the send; the meeting is booked.
    const json found = f.run("mail.search", {{"query", "Budget"}, {"from", "tom"}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(found["results"].size(), 1u) << found.dump();
    const std::string id = jstr(found["results"][0], "id");
    const json thread = f.run("mail.read", {{"id", id}, {"thread", true}}, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(thread["messages"].size(), 2u);
    const json draft = f.run("mail.draft", {{"reply_to", id}, {"reply_all", true}, {"body", "Let's meet on Thursday."}}, &ok);
    ASSERT_TRUE(ok) << draft.dump();
    EXPECT_TRUE(f.mock.ctl("GET", "/sends").empty());
    const json slots = f.run("calendar.find_free_time",
                             {{"attendees", {"eva.schneider@acme-test.example", "tom.baker@acme-test.example"}}, {"duration_minutes", 30}}, &ok);
    ASSERT_TRUE(ok) << slots.dump();
    ASSERT_FALSE(slots["slots"].empty());
    const json slot = slots["slots"][0];
    const json sent = f.run("mail.send",
                            {{"to", {"tom.baker@acme-test.example", "eva.schneider@acme-test.example"}},
                             {"subject", "RE: Budget Q4 – Entwurf"},
                             {"body", "Let's meet " + jstr(slot, "start") + "."},
                             {"reply_to", id}},
                            &ok);
    ASSERT_TRUE(ok) << sent.dump();
    const json booked = f.run("calendar.create",
                              {{"subject", "Budget Q4 – licences"},
                               {"start", jstr(slot, "start")},
                               {"end", jstr(slot, "end")},
                               {"attendees", {"tom.baker@acme-test.example", "eva.schneider@acme-test.example"}}},
                              &ok);
    ASSERT_TRUE(ok) << booked.dump();
    EXPECT_EQ(f.mock.ctl("GET", "/sends?kind=mail").size(), 1u);
    const json inv = f.mock.ctl("GET", "/sends?kind=invitation");
    ASSERT_EQ(inv.size(), 1u);
    EXPECT_EQ(inv[0]["to"].size(), 2u);
    // The next sync sees the new meeting; the sent reply is the user's own mail, not received mail.
    ASSERT_TRUE(f.svc->sync_now());
    const auto ev = f.take_events();
    EXPECT_EQ(count_kind(ev, "event.created", false), 1);
    EXPECT_EQ(count_kind(ev, "mail.received"), 0);
}

// A store write nested in a page transaction is part of it: it commits with the cursor or rolls back with it
// (improve-proactive-durable-work D1; this was a silently failed nested BEGIN before).
TEST(TriageSourceSpike, NestedMailWriteCommitsAndRollsBackWithTheCursor) {
    MailDb fixture;
    auto& db = fixture.store->db();
    MailStore store(db);
    StoredMessage message;
    message.id = "spike-message";
    message.folder_id = "spike-folder";
    message.change_key = "revision-1";
    const auto rolled_back = db.transaction([&]() -> Status {
        EXPECT_EQ(store.upsert_message(message), MailStore::Change::created);
        EXPECT_TRUE(store.set_delta_link("spike", "advanced-cursor", 1));
        return make_error("injected_failure", "the page failed");
    });
    ASSERT_FALSE(rolled_back);
    EXPECT_FALSE(store.message(message.id));
    EXPECT_EQ(store.delta_link("spike"), "");
    const auto committed = db.transaction([&]() -> Status {
        Status st;
        EXPECT_EQ(store.upsert_message(message, &st), MailStore::Change::created);
        if (!st) return st;
        return store.set_delta_link("spike", "advanced-cursor", 1);
    });
    ASSERT_TRUE(committed);
    EXPECT_TRUE(store.message(message.id));
    EXPECT_EQ(store.delta_link("spike"), "advanced-cursor");
    // A failed nested write reaches the caller instead of vanishing.
    ASSERT_TRUE(db.exec("CREATE TRIGGER fail_mail BEFORE INSERT ON mail_messages BEGIN SELECT RAISE(ABORT,'disk full'); END;"));
    StoredMessage other = message;
    other.id = "spike-message-2";
    const auto failed = db.transaction([&]() -> Status {
        Status st;
        store.upsert_message(other, &st);
        if (!st) return st;
        return store.set_delta_link("spike", "later-cursor", 2);
    });
    EXPECT_FALSE(failed);
    EXPECT_EQ(store.delta_link("spike"), "advanced-cursor");
}

namespace {
Status spike_page(Database& db, int fail_after, int terminate_after = 0) {
    return db.transaction([&]() -> Status {
        const char* statements[] = {
            "INSERT OR REPLACE INTO spike_source VALUES('message-1','revision-1')",
            "INSERT OR IGNORE INTO spike_pending VALUES('message-1','revision-1')",
            "INSERT OR REPLACE INTO spike_cursor VALUES('folder-1','next-page')",
        };
        for (int i = 0; i < 3; ++i) {
            if (auto status = db.exec(statements[i]); !status) return status;
            if (fail_after == i + 1) return make_error("injected_failure", "page interrupted");
            if (terminate_after == i + 1) {
                ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(100 + terminate_after));
                return make_error("termination_failed", "could not terminate test child");
            }
        }
        return {};
    });
}
long long spike_count(Database& db, const char* table) {
    auto q = db.prepare(std::string("SELECT count(*) FROM ") + table);
    EXPECT_TRUE(q.step());
    return q.col_int(0);
}
}  // namespace

TEST(TriageSourceSpike, PageAndCursorRollBackTogetherAndReplayOnce) {
    MailDb fixture;
    ASSERT_TRUE(fixture.store->db().exec(
        "CREATE TABLE spike_source(id TEXT PRIMARY KEY, revision TEXT NOT NULL);"
        "CREATE TABLE spike_pending(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id, revision));"
        "CREATE TABLE spike_cursor(scope TEXT PRIMARY KEY, position TEXT NOT NULL);"));
    for (int fail_after = 1; fail_after <= 3; ++fail_after) {
        EXPECT_FALSE(spike_page(fixture.store->db(), fail_after));
        fixture.open();
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_source"), 0);
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_pending"), 0);
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_cursor"), 0);
    }
    ASSERT_TRUE(spike_page(fixture.store->db(), 0));
    fixture.open();
    ASSERT_TRUE(spike_page(fixture.store->db(), 0));
    EXPECT_EQ(spike_count(fixture.store->db(), "spike_source"), 1);
    EXPECT_EQ(spike_count(fixture.store->db(), "spike_pending"), 1);
    EXPECT_EQ(spike_count(fixture.store->db(), "spike_cursor"), 1);
}

// This child is selected explicitly by the parent and only uses its disposable encrypted profile.
TEST(TriageSpikeCrashChild, Execute) {
    wchar_t root_path[32768]{};
    wchar_t phase_text[16]{};
    if (!::GetEnvironmentVariableW(L"SKOWORK_TRIAGE_SPIKE_ROOT", root_path, 32768)) GTEST_SKIP();
    ASSERT_GT(::GetEnvironmentVariableW(L"SKOWORK_TRIAGE_SPIKE_PHASE", phase_text, 16), 0u);
    const int phase = _wtoi(phase_text);
    ASSERT_GE(phase, 1);
    ASSERT_LE(phase, 4);
    auto root = DataRoot::open(std::filesystem::path(root_path));
    ASSERT_TRUE(root);
    KeyStore keys(root->keys());
    ASSERT_TRUE(keys.load());
    auto key = keys.get("app.db");
    ASSERT_TRUE(key);
    auto opened = Database::open(root->file("app.db"), *key);
    ASSERT_TRUE(opened);
    ASSERT_TRUE(spike_page(**opened, 0, phase < 4 ? phase : 0));
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(100 + phase));
}

TEST(TriageSourceSpike, ProcessTerminationKeepsPageAtomicAndReplayable) {
    MailDb fixture;
    ASSERT_TRUE(fixture.store->db().exec(
        "CREATE TABLE spike_source(id TEXT PRIMARY KEY, revision TEXT NOT NULL);"
        "CREATE TABLE spike_pending(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id, revision));"
        "CREATE TABLE spike_cursor(scope TEXT PRIMARY KEY, position TEXT NOT NULL);"));
    wchar_t executable[32768]{};
    ASSERT_GT(::GetModuleFileNameW(nullptr, executable, 32768), 0u);
    for (int phase = 1; phase <= 4; ++phase) {
        ASSERT_TRUE(fixture.store->db().exec("DELETE FROM spike_source; DELETE FROM spike_pending; DELETE FROM spike_cursor;"));
        fixture.store.reset();
        ASSERT_TRUE(::SetEnvironmentVariableW(L"SKOWORK_TRIAGE_SPIKE_ROOT", fixture.path.c_str()));
        const std::wstring phase_text = std::to_wstring(phase);
        ASSERT_TRUE(::SetEnvironmentVariableW(L"SKOWORK_TRIAGE_SPIKE_PHASE", phase_text.c_str()));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --gtest_filter=TriageSpikeCrashChild.Execute";
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION process{};
        const BOOL started = ::CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                              nullptr, nullptr, &startup, &process);
        ::SetEnvironmentVariableW(L"SKOWORK_TRIAGE_SPIKE_ROOT", nullptr);
        ::SetEnvironmentVariableW(L"SKOWORK_TRIAGE_SPIKE_PHASE", nullptr);
        ASSERT_TRUE(started);
        const DWORD wait = ::WaitForSingleObject(process.hProcess, 10000);
        if (wait != WAIT_OBJECT_0) {
            ::TerminateProcess(process.hProcess, 1);
            ::WaitForSingleObject(process.hProcess, 5000);
        }
        DWORD exit_code = 0;
        ::GetExitCodeProcess(process.hProcess, &exit_code);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        ASSERT_EQ(wait, WAIT_OBJECT_0);
        ASSERT_EQ(exit_code, static_cast<DWORD>(100 + phase));
        fixture.open();
        const int committed = phase == 4 ? 1 : 0;
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_source"), committed) << phase;
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_pending"), committed) << phase;
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_cursor"), committed) << phase;
        ASSERT_TRUE(spike_page(fixture.store->db(), 0));
        ASSERT_TRUE(spike_page(fixture.store->db(), 0));
        EXPECT_EQ(spike_count(fixture.store->db(), "spike_pending"), 1) << phase;
    }
}

TEST(TriageSourceSpike, GraphPageCanResumeAfterRestartAndExpiredCursorFails) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    GraphCall call;
    call.path = "/me/mailFolders/inbox/messages/delta";
    call.page_size = 1;
    auto first = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first->items.size(), 1u);
    ASSERT_FALSE(jstr(first->items[0], "id").empty());
    ASSERT_FALSE(jstr(first->items[0], "changeKey").empty());
    ASSERT_FALSE(first->next_link.empty());
    call.path = first->next_link;
    auto before = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(before->items.size(), 1u);
    const auto expected = before->items[0];
    f.restart();
    ASSERT_TRUE(f.svc->connected());
    auto resumed = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(resumed) << resumed.error().message;
    ASSERT_EQ(resumed->items.size(), 1u);
    EXPECT_EQ(jstr(resumed->items[0], "id"), jstr(expected, "id"));
    EXPECT_EQ(jstr(resumed->items[0], "changeKey"), jstr(expected, "changeKey"));
    f.mock.ctl("POST", "/delta/expire");
    auto expired = f.svc->connection()->graph->page(call);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "sync_state_not_found");
}

TEST(TriageSourceSpike, MockPageKeepsUnchangedItemAfterConcurrentDeletion) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    GraphCall call;
    call.path = "/me/mailFolders/inbox/messages/delta";
    call.page_size = 1;
    auto first = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first->items.size(), 1u);
    const auto deleted_id = jstr(first->items[0], "id");
    ASSERT_FALSE(first->next_link.empty());
    call.path = first->next_link;
    auto before = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(before->items.size(), 1u);
    const auto skipped_id = jstr(before->items[0], "id");
    ASSERT_FALSE(skipped_id.empty());
    f.mock.ctl("DELETE", "/users/jan/messages/" + deleted_id);
    auto after = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(after) << after.error().message;
    ASSERT_EQ(after->items.size(), 1u);
    EXPECT_EQ(jstr(after->items[0], "id"), skipped_id);
    while (!after->next_link.empty()) {
        call.path = after->next_link;
        after = f.svc->connection()->graph->page(call);
        ASSERT_TRUE(after) << after.error().message;
        for (const auto& item : after->items) EXPECT_NE(jstr(item, "id"), skipped_id);
    }
    ASSERT_FALSE(after->delta_link.empty());
    call.path = after->delta_link;
    auto delta = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(delta) << delta.error().message;
    bool saw_tombstone = false;
    for (const auto& item : delta->items) {
        EXPECT_NE(jstr(item, "id"), skipped_id);
        if (jstr(item, "id") == deleted_id && item.contains("@removed")) saw_tombstone = true;
    }
    EXPECT_TRUE(saw_tombstone);
}

namespace {
Status graph_spike_page(Database& db, const GraphPage& page, const std::string& requested = {}) {
    const std::string position = page.next_link.empty() ? page.delta_link : page.next_link;
    if (position.empty()) return make_error("invalid_page", "page lacks a cursor");
    std::set<std::pair<std::string, std::string>> events;
    std::map<std::string, std::string> revisions;
    std::set<std::string> removed;
    for (const auto& item : page.items) {
        const auto id = jstr(item, "id"), revision = jstr(item, "changeKey");
        if (id.empty()) return make_error("invalid_page", "item has no id");
        if (item.contains("@removed")) { removed.insert(id); continue; }
        if (revision.empty()) return make_error("invalid_page", "live item has no revision");
        const auto [it, inserted] = revisions.emplace(id, revision);
        if (!inserted && it->second != revision) return make_error("invalid_page", "conflicting revisions");
        events.emplace(id, revision);
    }
    for (const auto& [id, revision] : events)
        if (removed.count(id)) return make_error("invalid_page", "ambiguous item and tombstone");
    return db.transaction([&]() -> Status {
        {
            auto scan = db.prepare("SELECT active FROM graph_spike_resync");
            if (!scan.step()) return scan.status() ? make_error("missing_resync", "no test state") : scan.status();
            if (scan.col_int(0)) return make_error("resync_active", "normal intake is paused during resync");
        }
        bool gap = false;
        {
            auto s = db.prepare("SELECT gap,cursor FROM graph_spike_state WHERE scope='inbox'");
            if (!s.step()) return s.status() ? make_error("missing_scope", "no test state") : s.status();
            gap = s.col_int(0) != 0;
            const auto current = s.col_text(1);
            if (current == position) return {};  // a committed page replay
            if (current != requested) return make_error("stale_page", "source cursor changed");
        }
        if (!gap) {
            auto count = db.prepare("SELECT COUNT(*) FROM graph_spike_pending");
            if (!count.step()) return count.status() ? make_error("missing_count", "no count") : count.status();
            long long pending = count.col_int(0);
            for (const auto& [id, revision] : events) {
                auto exists = db.prepare("SELECT 1 FROM graph_spike_pending WHERE id=?1 AND revision=?2");
                exists.bind(1, id).bind(2, revision);
                const bool found = exists.step();
                if (!exists.status()) return exists.status();
                if (!found) ++pending;
            }
            gap = pending > 1000;
        }
        for (const auto& id : removed) {
            for (const char* sql : {"DELETE FROM graph_spike_source WHERE id=?1", "DELETE FROM graph_spike_pending WHERE id=?1",
                                    "DELETE FROM graph_spike_suggestion WHERE id=?1", "INSERT OR IGNORE INTO graph_spike_tombstone VALUES(?1)"}) {
                auto s = db.prepare(sql);
                s.bind(1, id);
                if (auto status = s.run(); !status) return status;
            }
        }
        for (const auto& [id, revision] : events) {
            for (const char* sql : {"DELETE FROM graph_spike_pending WHERE id=?1 AND revision<>?2",
                                    "DELETE FROM graph_spike_suggestion WHERE id=?1 AND revision<>?2"}) {
                auto s = db.prepare(sql);
                s.bind(1, id).bind(2, revision);
                if (auto status = s.run(); !status) return status;
            }
            auto tombstone = db.prepare("DELETE FROM graph_spike_tombstone WHERE id=?1");
            tombstone.bind(1, id);
            if (auto s = tombstone.run(); !s) return s;
            auto source = db.prepare("INSERT OR REPLACE INTO graph_spike_source(id,revision) VALUES(?1,?2)");
            source.bind(1, id).bind(2, revision);
            if (auto s = source.run(); !s) return s;
            if (!gap) {
                auto pending = db.prepare("INSERT OR IGNORE INTO graph_spike_pending VALUES(?1,?2)");
                pending.bind(1, id).bind(2, revision);
                if (auto s = pending.run(); !s) return s;
            }
        }
        auto state = db.prepare("UPDATE graph_spike_state SET cursor=?1,gap=?2 WHERE scope='inbox'");
        state.bind(1, position).bind(2, gap ? 1 : 0);
        return state.run();
    });
}
long long graph_spike_scalar(Database& db, const char* sql) {
    auto q = db.prepare(sql);
    EXPECT_TRUE(q.step());
    EXPECT_TRUE(q.status());
    return q.col_int(0);
}
Status graph_spike_suggest(Database& db, const std::string& id, const std::string& revision) {
    return db.transaction([&]() -> Status {
        auto insert = db.prepare("INSERT OR IGNORE INTO graph_spike_suggestion SELECT p.id,p.revision FROM graph_spike_pending p "
                                 "JOIN graph_spike_source s ON s.id=p.id AND s.revision=p.revision "
                                 "WHERE p.id=?1 AND p.revision=?2");
        insert.bind(1, id).bind(2, revision);
        if (auto status = insert.run(); !status) return status;
        auto done = db.prepare("DELETE FROM graph_spike_pending WHERE id=?1 AND revision=?2");
        done.bind(1, id).bind(2, revision);
        return done.run();
    });
}
Status graph_spike_resync_begin(Database& db) {
    return db.transaction([&]() -> Status {
        auto active = db.prepare("SELECT active FROM graph_spike_resync");
        if (!active.step()) return active.status() ? make_error("missing_resync", "no test state") : active.status();
        if (active.col_int(0)) return make_error("resync_active", "scan already in progress");
        return db.exec("UPDATE graph_spike_source SET seen=0; UPDATE graph_spike_state SET cursor='',gap=1 WHERE scope='inbox';"
                       "UPDATE graph_spike_resync SET active=1;");
    });
}
Status graph_spike_resync_page(Database& db, const GraphPage& page, const std::string& requested) {
    const auto position = page.next_link.empty() ? page.delta_link : page.next_link;
    if (position.empty() || (!page.next_link.empty() && !page.delta_link.empty()))
        return make_error("invalid_page", "invalid resync cursor");
    std::map<std::string, std::string> items;
    for (const auto& item : page.items) {
        const auto id = jstr(item, "id"), revision = jstr(item, "changeKey");
        if (id.empty() || revision.empty() || item.contains("@removed")) return make_error("invalid_page", "invalid resync item");
        const auto [it, inserted] = items.emplace(id, revision);
        if (!inserted && it->second != revision) return make_error("invalid_page", "conflicting revisions");
    }
    return db.transaction([&]() -> Status {
        auto state = db.prepare("SELECT cursor,active FROM graph_spike_state,graph_spike_resync WHERE scope='inbox'");
        if (!state.step()) return state.status() ? make_error("missing_state", "no resync state") : state.status();
        if (state.col_text(0) == position) return {};  // a previously committed page
        if (!state.col_int(1) || state.col_text(0) != requested) return make_error("stale_page", "resync cursor changed");
        for (const auto& [id, revision] : items) {
            for (const char* sql : {"DELETE FROM graph_spike_pending WHERE id=?1 AND revision<>?2",
                                    "DELETE FROM graph_spike_suggestion WHERE id=?1 AND revision<>?2"}) {
                auto s = db.prepare(sql);
                s.bind(1, id).bind(2, revision);
                if (auto status = s.run(); !status) return status;
            }
            auto source = db.prepare("INSERT OR REPLACE INTO graph_spike_source(id,revision,seen) VALUES(?1,?2,1)");
            source.bind(1, id).bind(2, revision);
            if (auto status = source.run(); !status) return status;
            auto tombstone = db.prepare("DELETE FROM graph_spike_tombstone WHERE id=?1");
            tombstone.bind(1, id);
            if (auto status = tombstone.run(); !status) return status;
        }
        if (!page.delta_link.empty()) {
            if (auto status = db.exec(
                    "INSERT OR IGNORE INTO graph_spike_tombstone SELECT id FROM graph_spike_source WHERE seen=0;"
                    "DELETE FROM graph_spike_pending WHERE id IN (SELECT id FROM graph_spike_source WHERE seen=0);"
                    "DELETE FROM graph_spike_suggestion WHERE id IN (SELECT id FROM graph_spike_source WHERE seen=0);"
                    "DELETE FROM graph_spike_source WHERE seen=0;"
                    "UPDATE graph_spike_resync SET active=0;"); !status) return status;
        }
        auto cursor = db.prepare("UPDATE graph_spike_state SET cursor=?1 WHERE scope='inbox'");
        cursor.bind(1, position);
        return cursor.run();
    });
}
}  // namespace

TEST(TriageSourceSpike, GraphOverflowPersistsGapAndContinuesSourcePaging) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    auto& db = f.db.store->db();
    ASSERT_TRUE(db.exec(
        "CREATE TABLE graph_spike_source(id TEXT PRIMARY KEY, revision TEXT NOT NULL);"
        "CREATE TABLE graph_spike_pending(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id,revision));"
        "CREATE TABLE graph_spike_suggestion(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id,revision));"
        "CREATE TABLE graph_spike_tombstone(id TEXT PRIMARY KEY);"
        "CREATE TABLE graph_spike_state(scope TEXT PRIMARY KEY, cursor TEXT NOT NULL, gap INTEGER NOT NULL);"
        "CREATE TABLE graph_spike_resync(active INTEGER NOT NULL);"
        "INSERT INTO graph_spike_resync VALUES(0);"
        "INSERT INTO graph_spike_state VALUES('inbox','',0);"
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<998) "
        "INSERT INTO graph_spike_pending SELECT 'existing-'||x, 'r1' FROM n;"));
    GraphCall call;
    call.path = "/me/mailFolders/inbox/messages/delta";
    call.page_size = 2;
    auto first = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first->items.size(), 2u);
    ASSERT_FALSE(first->next_link.empty());
    ASSERT_TRUE(db.exec("CREATE TRIGGER graph_spike_fail BEFORE UPDATE ON graph_spike_state "
                        "BEGIN SELECT RAISE(ABORT,'injected cursor failure'); END;"));
    EXPECT_FALSE(graph_spike_page(db, *first));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_source"), 0);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_pending"), 998);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT gap FROM graph_spike_state WHERE scope='inbox'"), 0);
    ASSERT_TRUE(db.exec("DROP TRIGGER graph_spike_fail"));
    ASSERT_TRUE(graph_spike_page(db, *first));
    ASSERT_TRUE(graph_spike_page(db, *first));  // replay at capacity must not create a false gap
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_source"), 2);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_pending"), 1000);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT gap FROM graph_spike_state WHERE scope='inbox'"), 0);
    call.path = first->next_link;
    auto second = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_EQ(second->items.size(), 2u);
    ASSERT_FALSE(second->next_link.empty());
    ASSERT_TRUE(graph_spike_page(db, *second, call.path));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_source"), 4);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_pending"), 1000);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT gap FROM graph_spike_state WHERE scope='inbox'"), 1);
    f.restart();
    auto& reopened = f.db.store->db();
    auto cursor = reopened.prepare("SELECT cursor FROM graph_spike_state WHERE scope='inbox'");
    ASSERT_TRUE(cursor.step());
    call.path = cursor.col_text(0);
    EXPECT_EQ(call.path, second->next_link);
    auto third = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(third) << third.error().message;
    ASSERT_FALSE(third->items.empty());
    ASSERT_TRUE(graph_spike_page(reopened, *third, call.path));
    EXPECT_EQ(graph_spike_scalar(reopened, "SELECT COUNT(*) FROM graph_spike_source"), 4 + third->items.size());
    EXPECT_EQ(graph_spike_scalar(reopened, "SELECT COUNT(*) FROM graph_spike_pending"), 1000);
    EXPECT_EQ(graph_spike_scalar(reopened, "SELECT gap FROM graph_spike_state WHERE scope='inbox'"), 1);
    auto next_cursor = reopened.prepare("SELECT cursor FROM graph_spike_state WHERE scope='inbox'");
    ASSERT_TRUE(next_cursor.step());
    EXPECT_EQ(next_cursor.col_text(0), third->next_link.empty() ? third->delta_link : third->next_link);
}

TEST(TriageSourceSpike, ReplayAndTombstoneCannotDuplicateOrRestoreSuggestion) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    auto& db = f.db.store->db();
    ASSERT_TRUE(db.exec(
        "CREATE TABLE graph_spike_source(id TEXT PRIMARY KEY, revision TEXT NOT NULL);"
        "CREATE TABLE graph_spike_pending(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id,revision));"
        "CREATE TABLE graph_spike_suggestion(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id,revision));"
        "CREATE TABLE graph_spike_tombstone(id TEXT PRIMARY KEY);"
        "CREATE TABLE graph_spike_state(scope TEXT PRIMARY KEY, cursor TEXT NOT NULL, gap INTEGER NOT NULL);"
        "CREATE TABLE graph_spike_resync(active INTEGER NOT NULL);"
        "INSERT INTO graph_spike_resync VALUES(0);"
        "INSERT INTO graph_spike_state VALUES('inbox','',0);"));
    GraphCall call;
    call.path = "/me/mailFolders/inbox/messages/delta";
    call.page_size = 100;
    auto initial = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(initial) << initial.error().message;
    ASSERT_FALSE(initial->items.empty());
    ASSERT_FALSE(initial->delta_link.empty());
    const auto id = jstr(initial->items[0], "id");
    const auto revision = jstr(initial->items[0], "changeKey");
    GraphPage ambiguous = *initial;
    auto conflicting = initial->items[0];
    conflicting["changeKey"] = revision + "-conflict";
    ambiguous.items.push_back(conflicting);
    EXPECT_FALSE(graph_spike_page(db, ambiguous));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_source"), 0);
    ASSERT_TRUE(graph_spike_page(db, *initial));
    ASSERT_TRUE(graph_spike_suggest(db, id, revision));
    ASSERT_TRUE(graph_spike_suggest(db, id, revision));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 1);
    ASSERT_TRUE(graph_spike_page(db, *initial));
    ASSERT_TRUE(graph_spike_suggest(db, id, revision));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 1);
    GraphCall edit;
    edit.method = "PATCH";
    edit.path = "/me/messages/" + id;
    edit.body = json{{"isRead", !jbool(initial->items[0], "isRead", false)}};
    ASSERT_TRUE(f.svc->connection()->graph->call(edit));
    call.path = initial->delta_link;
    auto changed = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(changed) << changed.error().message;
    ASSERT_EQ(changed->items.size(), 1u);
    const auto new_revision = jstr(changed->items[0], "changeKey");
    ASSERT_FALSE(new_revision.empty());
    ASSERT_NE(new_revision, revision);
    ASSERT_TRUE(graph_spike_page(db, *changed, call.path));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 0);
    ASSERT_TRUE(graph_spike_suggest(db, id, revision));  // stale result for the previous revision
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 0);
    ASSERT_TRUE(graph_spike_suggest(db, id, new_revision));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 1);
    f.mock.ctl("DELETE", "/users/jan/messages/" + id);
    call.path = changed->delta_link;
    auto removed = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(removed) << removed.error().message;
    ASSERT_EQ(removed->items.size(), 1u);
    ASSERT_TRUE(removed->items[0].contains("@removed"));
    ASSERT_TRUE(graph_spike_page(db, *removed, call.path));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 0);
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_tombstone"), 1);
    EXPECT_FALSE(graph_spike_page(db, *initial));  // a stale page cannot resurrect the item
    ASSERT_TRUE(graph_spike_suggest(db, id, new_revision));  // stale model result
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 0);
}

TEST(TriageSourceSpike, ExpiredCursorResyncReconcilesOnlyAfterFinalPage) {
    WB_REQUIRES_DEV_ENDPOINTS();
    REQUIRE_MOCK();
    Fixture f;
    ASSERT_TRUE(f.connect());
    auto& db = f.db.store->db();
    ASSERT_TRUE(db.exec(
        "CREATE TABLE graph_spike_source(id TEXT PRIMARY KEY, revision TEXT NOT NULL, seen INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE graph_spike_pending(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id,revision));"
        "CREATE TABLE graph_spike_suggestion(id TEXT NOT NULL, revision TEXT NOT NULL, PRIMARY KEY(id,revision));"
        "CREATE TABLE graph_spike_tombstone(id TEXT PRIMARY KEY);"
        "CREATE TABLE graph_spike_state(scope TEXT PRIMARY KEY, cursor TEXT NOT NULL, gap INTEGER NOT NULL);"
        "CREATE TABLE graph_spike_resync(active INTEGER NOT NULL);"
        "INSERT INTO graph_spike_resync VALUES(0);"
        "INSERT INTO graph_spike_state VALUES('inbox','',0);"));
    GraphCall call;
    call.path = "/me/mailFolders/inbox/messages/delta";
    call.page_size = 100;
    auto initial = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(initial) << initial.error().message;
    ASSERT_GE(initial->items.size(), 2u);
    ASSERT_FALSE(initial->delta_link.empty());
    const auto deleted_id = jstr(initial->items[0], "id");
    const auto retained_id = jstr(initial->items[1], "id");
    ASSERT_TRUE(graph_spike_page(db, *initial));
    ASSERT_TRUE(graph_spike_suggest(db, deleted_id, jstr(initial->items[0], "changeKey")));
    ASSERT_TRUE(graph_spike_suggest(db, retained_id, jstr(initial->items[1], "changeKey")));
    f.mock.ctl("POST", "/delta/expire");
    f.mock.ctl("DELETE", "/users/jan/messages/" + deleted_id);
    const auto arrived = f.mock.ctl("POST", "/users/jan/messages", {{"subject", "during expired cursor"}, {"body", "test"}});
    const auto arrived_id = jstr(arrived, "id");
    ASSERT_FALSE(arrived_id.empty());
    call.path = initial->delta_link;
    auto expired = f.svc->connection()->graph->page(call);
    ASSERT_FALSE(expired);
    ASSERT_EQ(expired.error().code, "sync_state_not_found");
    ASSERT_TRUE(graph_spike_resync_begin(db));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT gap FROM graph_spike_state WHERE scope='inbox'"), 1);
    call.path = "/me/mailFolders/inbox/messages/delta";
    call.page_size = 2;
    auto page = f.svc->connection()->graph->page(call);
    ASSERT_TRUE(page) << page.error().message;
    ASSERT_FALSE(page->next_link.empty());
    EXPECT_FALSE(graph_spike_page(db, *page, ""));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_source WHERE seen=1"), 0);
    ASSERT_TRUE(graph_spike_resync_page(db, *page, ""));
    EXPECT_EQ(graph_spike_scalar(db, "SELECT COUNT(*) FROM graph_spike_suggestion"), 2);  // no sweep on partial page
    f.restart();
    auto& reopened = f.db.store->db();
    std::string cursor;
    {
        auto q = reopened.prepare("SELECT cursor FROM graph_spike_state WHERE scope='inbox'");
        ASSERT_TRUE(q.step());
        cursor = q.col_text(0);
    }
    ASSERT_EQ(cursor, page->next_link);
    for (int pages = 0; pages < 100; ++pages) {
        call.path = cursor;
        page = f.svc->connection()->graph->page(call);
        ASSERT_TRUE(page) << page.error().message;
        EXPECT_FALSE(graph_spike_page(reopened, *page, cursor));
        if (!page->delta_link.empty()) {
            ASSERT_TRUE(reopened.exec("CREATE TRIGGER graph_spike_fail_resync BEFORE UPDATE ON graph_spike_state "
                                      "BEGIN SELECT RAISE(ABORT,'injected final cursor failure'); END;"));
            EXPECT_FALSE(graph_spike_resync_page(reopened, *page, cursor));
            EXPECT_EQ(graph_spike_scalar(reopened, "SELECT COUNT(*) FROM graph_spike_suggestion"), 2);
            EXPECT_EQ(graph_spike_scalar(reopened, "SELECT COUNT(*) FROM graph_spike_tombstone"), 0);
            ASSERT_TRUE(reopened.exec("DROP TRIGGER graph_spike_fail_resync"));
        }
        ASSERT_TRUE(graph_spike_resync_page(reopened, *page, cursor));
        if (!page->delta_link.empty()) break;
        ASSERT_FALSE(page->next_link.empty());
        cursor = page->next_link;
    }
    ASSERT_FALSE(page->delta_link.empty());
    EXPECT_EQ(graph_spike_scalar(reopened, "SELECT COUNT(*) FROM graph_spike_suggestion"), 1);
    EXPECT_EQ(graph_spike_scalar(reopened, "SELECT COUNT(*) FROM graph_spike_tombstone"), 1);
    EXPECT_EQ(graph_spike_scalar(reopened, "SELECT gap FROM graph_spike_state WHERE scope='inbox'"), 1);
    auto present = reopened.prepare("SELECT 1 FROM graph_spike_source WHERE id=?1");
    present.bind(1, arrived_id);
    EXPECT_TRUE(present.step());
    auto pending = reopened.prepare("SELECT 1 FROM graph_spike_pending WHERE id=?1");
    pending.bind(1, arrived_id);
    EXPECT_FALSE(pending.step());
    auto retained = reopened.prepare("SELECT 1 FROM graph_spike_suggestion WHERE id=?1");
    retained.bind(1, retained_id);
    EXPECT_TRUE(retained.step());
    auto deleted = reopened.prepare("SELECT 1 FROM graph_spike_source WHERE id=?1");
    deleted.bind(1, deleted_id);
    EXPECT_FALSE(deleted.step());
}
