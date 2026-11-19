// client/event-triage (improve-proactive-durable-work 2.2-2.5): opt-in and baseline, page intake with content revisions,
// bounded classification with backoff and generation checks, suggestions (dismiss, start, withdraw), overflow gaps and
// deletion, on a real encrypted app.db with a fake clock and model. The last test runs the intake through the mail sync's
// page transaction against dev/graph-mock (skips without node).
#include "support/dev_endpoints.h"
#include "support/graph_mock.h"

#include "mail/mail_service.h"
#include "triage/triage.h"

#include <map>
#include <set>

using namespace wb;
using namespace wb::mail;
using namespace std::chrono_literals;

namespace {

class TriageClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override {
        return sys_clock::time_point(std::chrono::duration_cast<sys_clock::duration>(std::chrono::milliseconds(base_ + ms_.load())));
    }
    void advance(std::chrono::milliseconds d) { ms_ += d.count(); }

private:
    static constexpr long long base_ = 1'790'000'000'000LL;
    std::atomic<long long> ms_{0};
};

// Answers every event with the outcome its text asks for ("[not]" not relevant, "[unsure]" uncertain, else relevant).
class TriageModel : public model::IChatModel {
public:
    std::optional<model::RoleInfo> role_info(model::Role role) override {
        if (role != model::Role::small) return std::nullopt;
        model::RoleInfo info;
        info.role = role;
        info.kind = "chat";
        return info;
    }
    model::ModelResult<model::ChatResponse> chat(const model::ChatRequest& r, const model::ChatSink&) override {
        ++requests;
        if (during) during();
        if (fail) return model::ModelFailure{"TIMEOUT", "timed out", "", 0, "", 0, true, false};
        const auto data = parse_json(r.messages.at(1).content);
        json results = json::array();
        last_batch = 0;
        for (const auto& e : (*data)["events"]) {
            ++last_batch;
            const std::string text = jstr(e, "text");
            const std::string outcome = text.find("[not]") != std::string::npos     ? "not_relevant"
                                        : text.find("[unsure]") != std::string::npos ? "uncertain"
                                                                                     : "relevant";
            results.push_back({{"id", jstr(e, "id")}, {"revision", jstr(e, "revision")}, {"outcome", outcome},
                               {"reason", outcome == "relevant" ? "Matches your budget work" : ""}});
        }
        model::ChatResponse out;
        out.finish_reason = "stop";
        out.text = "```json\n" + json{{"results", results}}.dump() + "\n```";
        return out;
    }
    int requests = 0;
    size_t last_batch = 0;
    bool fail = false;
    std::function<void()> during;
};

struct Harness {
    Harness() {
        mail = std::make_unique<MailStore>(db.store->db());
        EXPECT_TRUE(mail->set_folders({{"f-inbox", "Inbox", "inbox"}, {"f-other", "Projekty", ""}, {"f-sent", "Sent Items", "sentitems"}}));
        triage::TriageEnv e;
        e.db = &db.store->db();
        e.clock = &clock;
        e.mail = mail.get();
        e.model = &model;
        e.background_paused = [this] { return paused; };
        e.create_thread = [this](const std::string& title, const json& ref) -> Result<std::string> {
            const std::string id = "t" + std::to_string(threads.size() + 1);
            threads[id] = title;
            refs[id] = ref;
            return id;
        };
        e.rename_thread = [this](const std::string& id, const std::string& title) {
            threads[id] = title;
            return Status{};
        };
        e.mark_unread = [this](const std::string& id) {
            unread.insert(id);
            return Status{};
        };
        e.delete_thread = [this](const std::string& id) {
            threads.erase(id);
            deleted.insert(id);
            return Status{};
        };
        e.set_origin_ref = [this](const std::string& id, const json& ref) {
            refs[id] = ref;
            return Status{};
        };
        e.start_run = [this](const std::string& id, const std::string& text, const runtime::StartOptions& o) -> Result<std::string> {
            if (fail_start) return make_error("busy", "a Run is active");
            starts.push_back({id, text, o});
            return std::string("run-") + std::to_string(starts.size());
        };
        svc = std::make_unique<triage::TriageService>(std::move(e));
    }
    // A message in a folder; returns the change the page sink would see.
    PageChange message(const std::string& id, const std::string& subject, const std::string& body, const std::string& folder = "f-inbox",
                       bool withheld = false) {
        StoredMessage m;
        m.id = id;
        m.folder_id = folder;
        m.change_key = std::to_string(++keys);
        m.subject = subject;
        m.body = body;
        m.from = {"Petra Malá", "petra.mala@dodavatel-test.example"};
        m.received_at = clock.unix_millis();
        m.withheld = withheld;
        mail->upsert_message(m);
        PageChange c;
        c.scope = "mail";
        c.id = id;
        c.fresh = true;
        c.folder_id = folder;
        return c;
    }
    Status take(const std::vector<PageChange>& changes) {
        return db.store->db().transaction([&] { return svc->take_in(changes); });
    }
    void enable(json extra = json::object()) {
        extra["enabled"] = true;
        ASSERT_TRUE(svc->configure(extra));
        svc->cycle_completed();  // the baseline: what is there now is not suggested
    }
    // One classification after the gather time and the request interval.
    bool classify() {
        clock.advance(61s);
        return svc->step();
    }
    std::string thread_of(const std::string& item) {
        for (const auto& [id, ref] : refs)
            if (jstr(ref, "item_id") == item) return id;
        return {};
    }

    MailDb db;
    TriageClock clock;
    TriageModel model;
    std::unique_ptr<MailStore> mail;
    std::unique_ptr<triage::TriageService> svc;
    bool paused = false, fail_start = false;
    int keys = 0;
    std::map<std::string, std::string> threads;
    std::map<std::string, json> refs;
    std::set<std::string> unread, deleted;
    struct Start {
        std::string thread, text;
        runtime::StartOptions options;
    };
    std::vector<Start> starts;
};

}  // namespace

TEST(EventTriage, OffByDefaultAndTheBaselineIsNotSuggested) {
    Harness h;
    EXPECT_FALSE(jbool(h.svc->status(), "enabled"));
    ASSERT_TRUE(h.take({h.message("m1", "Budget", "Please review the budget")}));
    EXPECT_FALSE(h.classify());
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);

    ASSERT_TRUE(h.svc->configure({{"enabled", true}}));
    EXPECT_EQ(jstr(h.svc->status(), "baseline"), "pending");
    // Changes during the baseline are what is there already: not suggested.
    ASSERT_TRUE(h.take({h.message("m2", "Budget", "Please review the budget")}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);
    h.svc->cycle_completed();
    EXPECT_EQ(jstr(h.svc->status(), "baseline"), "done");
    ASSERT_TRUE(h.take({h.message("m3", "Budget Q4", "Please review the budget")}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    EXPECT_EQ(h.model.requests, 0);
}

TEST(EventTriage, RelevantItemBecomesAnUnreadSuggestionThreadThatNeverStartsItself) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget"), h.message("m2", "Lunch", "[not] lunch on Friday?")}));
    // Gathered for 30 seconds before the first request.
    EXPECT_FALSE(h.svc->step());
    EXPECT_TRUE(h.classify());
    EXPECT_EQ(h.model.requests, 1);
    EXPECT_EQ(h.model.last_batch, 2u);
    ASSERT_EQ(h.threads.size(), 1u);
    const std::string t = h.thread_of("m1");
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(h.threads[t], "Suggestion: Budget Q4");
    EXPECT_EQ(jstr(h.refs[t], "kind"), "triage");
    EXPECT_EQ(jstr(h.refs[t], "scope"), "mail");
    EXPECT_TRUE(h.starts.empty());
    const json s = h.svc->suggestion(t);
    EXPECT_EQ(jstr(s, "status"), "open");
    EXPECT_EQ(jstr(s, "reason"), "Matches your budget work");
    EXPECT_EQ(jstr(s["source"], "subject"), "Budget Q4");
    EXPECT_EQ(jstr(s["source"], "folder"), "inbox");
    EXPECT_TRUE(h.svc->suggestion("no-such-thread").is_null());
    // A replay of the same page and content is not evaluated again.
    ASSERT_TRUE(h.take({PageChange{"mail", "m1", false, true, "f-inbox", ""}}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);
    EXPECT_FALSE(h.classify());
}

TEST(EventTriage, ChangedContentIsANewRevisionThatCanWithdrawOrReopen) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    const std::string t = h.thread_of("m1");
    const std::string first = h.svc->revision("mail", "m1");
    // The sender rewrites it: no longer relevant, the suggestion is withdrawn and keeps no source details.
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "[not] never mind")}));
    EXPECT_NE(h.svc->revision("mail", "m1"), first);
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "withdrawn");
    EXPECT_FALSE(h.svc->suggestion(t).contains("source"));
    EXPECT_EQ(h.threads[t], "Withdrawn suggestion");
    // Relevant again: the same Thread reopens, unread.
    h.unread.clear();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4 again", "Please review the budget today")}));
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(h.threads.size(), 1u);
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "open");
    EXPECT_EQ(h.threads[t], "Suggestion: Budget Q4 again");
    EXPECT_TRUE(h.unread.contains(t));
}

TEST(EventTriage, DismissHoldsForTheRevisionAndStartClaimsOnce) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    const std::string t = h.thread_of("m1");
    ASSERT_TRUE(h.svc->dismiss(t));
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "dismissed");
    EXPECT_FALSE(h.svc->dismiss(t));
    auto r = h.svc->start_work(t);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "not_open");
    // A new revision that is relevant reopens it.
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the updated budget")}));
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "open");

    // A failed start leaves the suggestion open; a successful one claims it.
    h.fail_start = true;
    EXPECT_FALSE(h.svc->start_work(t));
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "open");
    h.fail_start = false;
    auto run = h.svc->start_work(t);
    ASSERT_TRUE(run) << run.error().message;
    ASSERT_EQ(h.starts.size(), 1u);
    EXPECT_EQ(h.starts[0].thread, t);
    ASSERT_TRUE(h.starts[0].options.untrusted.has_value());
    EXPECT_EQ(h.starts[0].options.untrusted->source_kind, "mail");
    EXPECT_FALSE(h.starts[0].options.untrusted->trusted);
    EXPECT_NE(h.starts[0].text.find("updated budget"), std::string::npos);
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "started");
    EXPECT_FALSE(h.svc->start_work(t));
    // Later revisions do not rewrite started work.
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "[not] changed again")}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);
}

TEST(EventTriage, OnlyEligibleItemsAndRemovalWithdraws) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Projekt", "Please review the budget", "f-other"),
                        h.message("m2", "Sent", "Please review the budget", "f-sent"),
                        h.message("m3", "", "", "f-inbox", /*withheld=*/true),
                        h.message("m4", "Budget Q4", "Please review the budget")}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    // Calendar events are off unless chosen.
    StoredEvent ev;
    ev.id = "e1";
    ev.change_key = "1";
    ev.subject = "Budget review";
    ev.start_at = h.clock.unix_millis() + 3'600'000;
    ev.end_at = ev.start_at + 3'600'000;
    h.mail->upsert_event(ev);
    ASSERT_TRUE(h.take({PageChange{"calendar", "e1", false, true, "", ""}}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    ASSERT_TRUE(h.svc->configure({{"calendar", true}, {"folders", {"inbox", "Projekty"}}}));
    h.svc->cycle_completed();
    ASSERT_TRUE(h.take({PageChange{"calendar", "e1", false, true, "", ""}, h.message("m1", "Projekt", "Please review the plan", "f-other")}));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 3);  // m4 still waits

    ASSERT_TRUE(h.classify());
    const std::string t = h.thread_of("m1");
    ASSERT_FALSE(t.empty());
    // Deleted at the source: withdrawn in the same page transaction.
    h.mail->remove_message("m1");
    ASSERT_TRUE(h.take({PageChange{"mail", "m1", true, false, "", ""}}));
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "withdrawn");
    // Moved out of scope by a settings change: withdrawn by the sweep.
    const std::string te = h.thread_of("e1");
    ASSERT_FALSE(te.empty());
    ASSERT_TRUE(h.svc->configure({{"calendar", false}}));
    EXPECT_EQ(jstr(h.svc->suggestion(te), "status"), "withdrawn");
}

TEST(EventTriage, DeleteOrEditDuringClassificationCannotUseTheOldResult) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget"), h.message("m2", "Budget Q1", "Please review it")}));
    h.model.during = [&h] {
        (void)h.mail->remove_message("m1");
        (void)h.take({PageChange{"mail", "m1", true, false, "", ""}, h.message("m2", "Budget Q1", "[not] Please ignore it")});
    };
    ASSERT_TRUE(h.classify());
    EXPECT_TRUE(h.threads.empty());
    // The edit is a new revision, evaluated on its own.
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    h.model.during = nullptr;
    ASSERT_TRUE(h.classify());
    EXPECT_TRUE(h.threads.empty());
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);
}

TEST(EventTriage, FailureBacksOffAndChangedSettingsDiscardResults) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    h.model.fail = true;
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    EXPECT_FALSE(jstr(h.svc->status(), "last_error").empty());
    EXPECT_EQ(h.svc->status()["last_error"], "Triage is unavailable.");
    // The second failure waits twice as long.
    ASSERT_TRUE(h.classify());
    h.clock.advance(61s);
    EXPECT_FALSE(h.svc->step());
    h.clock.advance(60s);
    h.model.fail = false;
    // Responsibilities change while the request is out: its results are discarded and the item stays pending.
    h.model.during = [&h] { (void)h.svc->configure({{"responsibilities", "Budgets and travel"}}); };
    ASSERT_TRUE(h.svc->step());
    EXPECT_TRUE(h.threads.empty());
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    h.model.during = nullptr;
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(h.threads.size(), 1u);
    EXPECT_TRUE(jstr(h.svc->status(), "last_error").empty());
    // Paused background work makes no requests.
    ASSERT_TRUE(h.take({h.message("m2", "Budget Q1", "Please review the budget")}));
    h.paused = true;
    EXPECT_FALSE(h.classify());
    h.paused = false;
    EXPECT_TRUE(h.classify());
}

TEST(EventTriage, BatchesOfTwentyAndUncertainIsCounted) {
    Harness h;
    h.enable();
    std::vector<PageChange> page;
    for (int i = 0; i < 25; ++i) page.push_back(h.message("m" + std::to_string(i), "Item " + std::to_string(i), i == 0 ? "[unsure] maybe" : "[not] no"));
    ASSERT_TRUE(h.take(page));
    // A full batch goes without waiting for the gather time.
    EXPECT_TRUE(h.svc->step());
    EXPECT_EQ(h.model.last_batch, 20u);
    EXPECT_FALSE(h.svc->step());  // a minute between requests
    EXPECT_TRUE(h.classify());
    EXPECT_EQ(h.model.last_batch, 5u);
    EXPECT_EQ(jint(h.svc->status(), "uncertain"), 1);
    EXPECT_TRUE(h.threads.empty());
}

TEST(EventTriage, OverflowPausesTheScopeWithAVisibleGapUntilAcknowledged) {
    Harness h;
    h.enable();
    std::vector<PageChange> page;
    for (int i = 0; i < triage::kMaxPending + 2; ++i) page.push_back(h.message("m" + std::to_string(i), "Item", "[not] body " + std::to_string(i)));
    ASSERT_TRUE(h.take(page));
    json s = h.svc->status();
    EXPECT_EQ(jint(s, "pending"), triage::kMaxPending);
    ASSERT_TRUE(s["gaps"].contains("mail"));
    EXPECT_EQ(jint(s["gaps"]["mail"], "missed"), 2);
    EXPECT_FALSE(s["gaps"].contains("calendar"));
    // While the scope is paused, new changes are only counted.
    ASSERT_TRUE(h.take({h.message("late", "Budget", "Please review")}));
    EXPECT_EQ(jint(h.svc->status()["gaps"]["mail"], "missed"), 3);
    auto full = h.svc->acknowledge_gap();
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, "full");
    for (int i = 0; i < 60; ++i) (void)h.classify();
    EXPECT_LT(jint(h.svc->status(), "pending"), triage::kMaxPending);
    ASSERT_TRUE(h.svc->acknowledge_gap());
    EXPECT_TRUE(h.svc->status()["gaps"].empty());
    EXPECT_EQ(jstr(h.svc->status(), "baseline"), "pending");
}

TEST(EventTriage, OldPendingItemsExpire) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget", "Please review")}));
    h.model.fail = true;
    h.clock.advance(std::chrono::milliseconds(triage::kExpiryMs + 1000));
    (void)h.svc->step();
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);
    EXPECT_EQ(jint(h.svc->status(), "expired"), 1);
}

TEST(EventTriage, DeleteTriageDataKeepsStartedWorkAndThreadDeletionDismisses) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget"), h.message("m2", "Budget Q1", "Please review it"),
                        h.message("m3", "Budget Q2", "Please review this")}));
    ASSERT_TRUE(h.classify());
    const std::string t1 = h.thread_of("m1"), t2 = h.thread_of("m2"), t3 = h.thread_of("m3");
    ASSERT_TRUE(h.svc->start_work(t1));
    // The user deletes a suggestion Thread: dismissed for this revision, not raised again for the same content.
    ASSERT_TRUE(h.svc->thread_deleted(t3));
    EXPECT_TRUE(h.svc->suggestion(t3).is_null());
    ASSERT_TRUE(h.svc->delete_data());
    EXPECT_TRUE(h.deleted.contains(t2));
    EXPECT_FALSE(h.deleted.contains(t1));
    EXPECT_TRUE(h.refs[t1].empty());
    EXPECT_TRUE(h.svc->suggestion(t1).is_null());
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);
}

TEST(EventTriage, SettingsAreValidated) {
    Harness h;
    EXPECT_FALSE(h.svc->configure({{"enabled", "yes"}}));
    EXPECT_FALSE(h.svc->configure({{"folders", "inbox"}}));
    EXPECT_FALSE(h.svc->configure({{"responsibilities", std::string(triage::kMaxResponsibilities + 1, 'x')}}));
    EXPECT_TRUE(h.svc->configure({{"responsibilities", std::string(triage::kMaxResponsibilities, 'x')}}));
    const json s = h.svc->status();
    EXPECT_FALSE(jbool(s, "enabled"));
    EXPECT_EQ(s["folders"], json::array({"inbox"}));
}

// The intake rides the mail sync's page transaction (Graph mock): new mail after the baseline is pending, a deletion
// withdraws its suggestion, and a page that fails to commit leaves nothing behind.
TEST(EventTriage, GraphPagesFeedTriageInTheirTransaction) {
    WB_REQUIRES_DEV_ENDPOINTS();
    if (!graph_mock_ready()) GTEST_SKIP() << "node or dev/graph-mock/node_modules missing (npm ci there)";
    GraphMockProcess mock;
    MailDb db;
    FollowingBrowser browser;
    json policy = mock.policy();
    MailEnv e;
    e.store = db.store.get();
    e.clock = &system_clock();
    e.launcher = &browser;
    e.policy = policy_from(&policy);
    e.dev_build = true;
    e.login_hint = "jan";
    e.background_sync = false;
    MailService svc(std::move(e));
    TriageModel model;
    std::map<std::string, json> refs;
    triage::TriageEnv te;
    te.db = &db.store->db();
    te.clock = &system_clock();
    te.mail = &svc.store();
    te.model = &model;
    te.create_thread = [&refs](const std::string&, const json& ref) -> Result<std::string> {
        const std::string id = "t" + std::to_string(refs.size() + 1);
        refs[id] = ref;
        return id;
    };
    te.rename_thread = [](const std::string&, const std::string&) { return Status{}; };
    triage::TriageService triage(std::move(te));
    svc.set_page_sink([&triage](const std::vector<PageChange>& c) { return triage.take_in(c); });
    svc.on_cycle([&triage] { triage.cycle_completed(); });
    svc.start();
    ASSERT_TRUE(triage.configure({{"enabled", true}}));
    ASSERT_TRUE(svc.connect());
    ASSERT_TRUE(svc.sync_now());
    EXPECT_EQ(jstr(triage.status(), "baseline"), "done");
    EXPECT_EQ(jint(triage.status(), "pending"), 0);  // the mailbox as it was is the baseline

    mock.ctl("POST", "/users/jan/messages",
             {{"from", "Petra Malá <petra.mala@dodavatel-test.example>"}, {"subject", "Rozpočet Q4"}, {"body", "<p>Prosím o kontrolu.</p>"}, {"label", "public"}});
    ASSERT_TRUE(svc.sync_now());
    EXPECT_EQ(jint(triage.status(), "pending"), 1);
    std::string id;
    for (const auto& m : svc.store().search({}))
        if (m.subject == "Rozpočet Q4") id = m.id;
    ASSERT_FALSE(id.empty());
    // Deleted at the source before or after classification: nothing stays outstanding.
    mock.ctl("DELETE", "/users/jan/messages/" + id);
    ASSERT_TRUE(svc.sync_now());
    EXPECT_EQ(jint(triage.status(), "pending"), 0);
    EXPECT_EQ(jint(triage.status(), "suggestions"), 0);
    svc.stop();
}
