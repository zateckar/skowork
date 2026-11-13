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
