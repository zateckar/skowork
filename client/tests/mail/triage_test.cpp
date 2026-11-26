// client/event-triage (improve-proactive-durable-work 2.2-2.5): opt-in and baseline, page intake with content revisions,
// bounded classification with backoff and generation checks, suggestions (dismiss, start, withdraw), overflow gaps and
// deletion, on a real encrypted app.db with a fake clock and model. The last test runs the intake through the mail sync's
// page transaction against dev/graph-mock (skips without node).
#include "support/dev_endpoints.h"
#include "support/graph_mock.h"

#include "mail/mail_service.h"
#include "triage/triage.h"
#include "eval/decision_log.h"
#include "eval/registry.h"

#include <algorithm>
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
            (void)e;
            ++last_batch;
        }
        batches.push_back(last_batch);
        for (const auto& e : (*data)["events"]) {
            const std::string text = jstr(e, "text");
            const std::string outcome = text.find("[not]") != std::string::npos     ? "not_relevant"
                                        : text.find("[unsure]") != std::string::npos ? "uncertain"
                                                                                     : "relevant";
            json r = {{"id", jstr(e, "id")}, {"revision", jstr(e, "revision")}, {"outcome", outcome},
                      {"reason", outcome == "relevant" ? "Matches your budget work" : ""}};
            if (outcome == "relevant") {
                // "[info]" for information, else action required; "[approval]" picks that category.
                r["action"] = text.find("[info]") != std::string::npos ? "info" : "required";
                r["category"] = text.find("[approval]") != std::string::npos ? "approval" : "request";
                // "[bad]": a category outside the list, which makes the whole batch invalid.
                if (text.find("[bad]") != std::string::npos) r["category"] = "other";
            }
            results.push_back(std::move(r));
        }
        model::ChatResponse out;
        out.finish_reason = "stop";
        out.text = "```json\n" + json{{"results", results}}.dump() + "\n```";
        return out;
    }
    int requests = 0;
    size_t last_batch = 0;
    std::vector<size_t> batches;
    bool fail = false;
    std::function<void()> during;
};

struct Harness {
    Harness() {
        mail = std::make_unique<MailStore>(db.store->db());
        EXPECT_TRUE(mail->set_folders({{"f-inbox", "Inbox", "inbox"}, {"f-other", "Projekty", ""}, {"f-sent", "Sent Items", "sentitems"}}));
        log = std::make_unique<eval::DecisionLog>(db.store->db());
        triage::TriageEnv e;
        e.db = &db.store->db();
        e.decisions = log.get();
        e.quality = [this](const std::string& c) { quality.push_back(c); };
        e.clock = &clock;
        e.mail = mail.get();
        e.model = &model;
        e.teams = [this](const std::string& conversation, const std::string& id) -> std::optional<triage::TriageChat> {
            const auto it = chats.find(conversation + "|" + id);
            if (it == chats.end()) return std::nullopt;
            return it->second;
        };
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
        e.unattended_may_read = [this] { return may_read; };
        e.cancel_runs = [this](const std::string& id) { cancelled.insert(id); };
        e.archive_thread = [this](const std::string& id) {
            archived.insert(id);
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
    // Automatic preparation is off unless a test asks for it: most tests follow the suggestion itself.
    void enable(json extra = json::object()) {
        extra["enabled"] = true;
        if (!extra.contains("auto_prepare")) extra["auto_prepare"] = false;
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
    std::unique_ptr<eval::DecisionLog> log;
    std::vector<std::string> quality;  // quality counters raised
    std::unique_ptr<triage::TriageService> svc;
    bool paused = false, fail_start = false, may_read = true;
    int keys = 0;
    std::map<std::string, triage::TriageChat> chats;  // "<conversation>|<message>"
    std::map<std::string, std::string> threads;
    std::map<std::string, json> refs;
    std::set<std::string> unread, deleted, cancelled, archived;
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
    EXPECT_EQ(h.threads[t], "Budget Q4");  // no prefix (improve-triage-results D5)
    EXPECT_EQ(jstr(h.refs[t], "action"), "required");
    EXPECT_EQ(jstr(h.refs[t], "category"), "request");
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
    EXPECT_EQ(h.threads[t], "Budget Q4 again");
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

// add-teams-triage: Teams messages addressed to the user become suggestion Threads like mail.
TEST(EventTriage, TeamsMessagesAddressedToTheUserBecomeSuggestions) {
    Harness h;
    h.chats["19:a@unq.gbl.spaces|1"] = {"Petra Malá", "Petra Malá", "Please review the budget by Friday", 1, true, false, false};
    h.chats["19:g@thread.v2|2"] = {"Projekt X", "Jan Novák", "Please review the budget too", 2, false, false, false};
    h.chats["19:a@unq.gbl.spaces|3"] = {"Petra Malá", "Me", "Please review the budget, I said", 3, true, true, false};
    h.enable();
    // Teams off in triage: nothing is taken in.
    ASSERT_TRUE(h.svc->take_in_teams("teams.message", "19:a@unq.gbl.spaces", "1"));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 0);

    ASSERT_TRUE(h.svc->configure({{"teams", true}}));
    EXPECT_TRUE(jbool(h.svc->status(), "teams"));
    h.svc->cycle_completed();
    ASSERT_TRUE(h.svc->take_in_teams("teams.message", "19:a@unq.gbl.spaces", "1"));  // one-to-one: addressed
    ASSERT_TRUE(h.svc->take_in_teams("teams.message", "19:g@thread.v2", "2"));        // group chat without a mention
    ASSERT_TRUE(h.svc->take_in_teams("teams.message", "19:a@unq.gbl.spaces", "3"));  // the user's own message
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);

    EXPECT_FALSE(h.svc->step());
    EXPECT_TRUE(h.classify());
    const std::string t = h.thread_of("19:a@unq.gbl.spaces|1");
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(h.threads[t], "Petra Malá: Please review the budget by Friday");
    EXPECT_EQ(jstr(h.refs[t], "scope"), "teams");
    const json s = h.svc->suggestion(t);
    EXPECT_EQ(jstr(s, "status"), "open");
    EXPECT_EQ(jstr(s["source"], "from"), "Petra Malá");

    auto run = h.svc->start_work(t);
    ASSERT_TRUE(run);
    ASSERT_EQ(h.starts.size(), 1u);
    EXPECT_NE(h.starts[0].text.find("Teams message\nFrom: Petra Malá"), std::string::npos);
    EXPECT_EQ(h.starts[0].options.untrusted->source_kind, "teams");

    // A message Teams deletes is withdrawn.
    h.chats["19:g@thread.v2|4"] = {"Projekt X", "Jan Novák", "@Me please send the figures", 4, true, false, false};
    ASSERT_TRUE(h.svc->take_in_teams("teams.message", "19:g@thread.v2", "4"));
    EXPECT_EQ(jint(h.svc->status(), "pending"), 1);
    h.chats.erase("19:g@thread.v2|4");
    ASSERT_TRUE(h.svc->take_in_teams("teams.deleted", "19:g@thread.v2", "4"));
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

// ---- improve-triage-results ----

TEST(EventTriage, AnActionItemIsPreparedAutomaticallyAsAReadOnlyUnattendedRun) {
    Harness h;
    h.enable({{"auto_prepare", true}});
    EXPECT_TRUE(jbool(h.svc->status(), "auto_prepare"));
    ASSERT_TRUE(h.take({h.message("m1", "B2B Portal: Schválení žádosti 805875", "[approval] Please approve request 805875")}));
    ASSERT_TRUE(h.classify());
    const std::string t = h.thread_of("m1");
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(h.threads[t], "B2B Portal: Schválení žádosti 805875");
    EXPECT_EQ(jstr(h.refs[t], "category"), "approval");
    ASSERT_EQ(h.starts.size(), 1u);
    EXPECT_EQ(h.starts[0].thread, t);
    EXPECT_TRUE(h.starts[0].options.unattended);
    ASSERT_TRUE(h.starts[0].options.untrusted.has_value());
    EXPECT_EQ(h.starts[0].options.untrusted->source_kind, "mail");
    // D6: options on a Question card, then the reply through an approved mail.send to the source.
    EXPECT_NE(h.starts[0].options.preamble.find("ask_user"), std::string::npos);
    EXPECT_NE(h.starts[0].options.preamble.find("mail.send, with reply_to set to m1"), std::string::npos);
    EXPECT_NE(h.starts[0].text.find("805875"), std::string::npos);
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "started");
    EXPECT_EQ(jint(h.svc->status(), "prepared_today"), 1);
    // A prepared item is started: a new revision is not evaluated again and is not prepared twice.
    ASSERT_TRUE(h.take({h.message("m1", "B2B Portal: Schválení žádosti 805875", "[approval] Reminder: please approve request 805875")}));
    EXPECT_FALSE(h.classify());
    EXPECT_EQ(h.starts.size(), 1u);
}

TEST(EventTriage, PreparationWaitsForStartWorkWhenOffForbiddenOrCapped) {
    Harness h;
    h.enable({{"auto_prepare", true}});
    h.may_read = false;  // policy.unattended.allowed_risk without read
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    EXPECT_TRUE(h.starts.empty());
    EXPECT_EQ(jstr(h.svc->suggestion(h.thread_of("m1")), "status"), "open");
    h.may_read = true;
    // A failed start keeps the suggestion and does not count against the cap.
    h.fail_start = true;
    ASSERT_TRUE(h.take({h.message("m2", "Budget Q1", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(jstr(h.svc->suggestion(h.thread_of("m2")), "status"), "open");
    EXPECT_EQ(jint(h.svc->status(), "prepared_today"), 0);
    h.fail_start = false;
    // The daily cap: 30 prepared, the next ones wait.
    for (int batch = 0; batch < 2; ++batch) {
        std::vector<PageChange> changes;
        for (int i = 0; i < 16; ++i) {
            const std::string id = "c" + std::to_string(batch * 16 + i);
            changes.push_back(h.message(id, "Request " + id, "Please handle " + id));
        }
        ASSERT_TRUE(h.take(changes));
        ASSERT_TRUE(h.classify());
    }
    EXPECT_EQ(h.starts.size(), 30u);
    EXPECT_EQ(jint(h.svc->status(), "prepared_today"), 30);
    EXPECT_EQ(jint(h.svc->status(), "suggestions"), 4);  // m1, m2 and two above the cap keep Start work
    // Turned off: nothing is prepared.
    ASSERT_TRUE(h.svc->configure({{"auto_prepare", false}}));
    EXPECT_FALSE(jbool(h.svc->status(), "auto_prepare"));
}

TEST(EventTriage, InformationItemsGetNoThreadAndAreReportedOnce) {
    Harness h;
    h.enable({{"auto_prepare", true}});
    ASSERT_TRUE(h.take({h.message("m1", "XELERATE Release", "[info] The release went out today"),
                        h.message("m2", "Budget Q4", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    EXPECT_TRUE(h.thread_of("m1").empty());
    EXPECT_EQ(h.starts.size(), 1u);
    EXPECT_EQ(jint(h.svc->status(), "info_waiting"), 1);
    auto d = h.svc->digest();
    ASSERT_EQ(d.info.size(), 1u);
    EXPECT_EQ(d.info[0].title, "XELERATE Release");
    EXPECT_EQ(d.info[0].reason, "Matches your budget work");
    EXPECT_EQ(d.info[0].from, "Petra Malá <petra.mala@dodavatel-test.example>");
    ASSERT_EQ(d.action.size(), 1u);
    EXPECT_EQ(d.action[0].thread_id, h.thread_of("m2"));
    h.svc->reported(d.info);
    d = h.svc->digest();
    EXPECT_TRUE(d.info.empty());
    EXPECT_EQ(d.action.size(), 1u);  // open action items stay until they are done
    EXPECT_EQ(jint(h.svc->status(), "info_waiting"), 0);
}

TEST(EventTriage, ThePreparationOfATeamsItemNeverSendsMail) {
    const std::string p = triage::prepare_preamble("teams", "conv|msg");
    EXPECT_NE(p.find("ask_user"), std::string::npos);
    EXPECT_NE(p.find("cannot send"), std::string::npos);
    EXPECT_EQ(p.find("mail.send"), std::string::npos);
    EXPECT_EQ(triage::prepare_preamble("calendar", "e1").find("mail.send"), std::string::npos);
}

TEST(EventTriage, NoActionNeededIsRecordedCancelsTheRunAndArchives) {
    Harness h;
    h.enable({{"auto_prepare", true}});
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget"), h.message("m2", "Budget Q1", "Please review it")}));
    ASSERT_TRUE(h.classify());
    const std::string t = h.thread_of("m1");
    ASSERT_EQ(jstr(h.svc->suggestion(t), "status"), "started");  // prepared, waiting on its Question card
    const Status decided = h.svc->no_action(t);
    ASSERT_TRUE(decided) << decided.error().code << ": " << decided.error().message;
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "no_action");
    EXPECT_TRUE(h.cancelled.contains(t));
    EXPECT_TRUE(h.archived.contains(t));
    EXPECT_EQ(h.svc->no_action(t).error().code, "not_open");  // once
    // The Daily report no longer lists it; the other item is still open work.
    auto d = h.svc->digest();
    ASSERT_EQ(d.action.size(), 1u);
    EXPECT_EQ(d.action[0].thread_id, h.thread_of("m2"));
    // A new revision keeps the decision and starts nothing.
    const size_t starts = h.starts.size();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Reminder: please review the budget")}));
    (void)h.classify();
    EXPECT_EQ(jstr(h.svc->suggestion(t), "status"), "no_action");
    EXPECT_EQ(h.starts.size(), starts);
}

// improve-triage-results D10: one event that always gets an invalid response no longer blocks the queue.
TEST(EventTriage, AnInvalidResponseShrinksTheBatchUntilTheBadEventIsSetAside) {
    Harness h;
    h.enable();
    std::vector<PageChange> changes;
    for (int i = 0; i < 20; ++i)
        changes.push_back(h.message("m" + std::to_string(i), "Budget " + std::to_string(i),
                                    i == 7 ? "Please review the budget [bad]" : "Please review the budget"));
    ASSERT_TRUE(h.take(changes));
    for (int attempt = 0; attempt < 20 && h.svc->status()["pending"].get<long long>() > 0; ++attempt) (void)h.classify();
    EXPECT_EQ(h.svc->status()["pending"].get<long long>(), 0);
    ASSERT_GE(h.model.batches.size(), 2u);
    EXPECT_EQ(h.model.batches[0], 20u);
    EXPECT_EQ(h.model.batches[1], 10u);  // halved after the invalid response, with no doubling wait
    EXPECT_LE(h.model.requests, 12);
    EXPECT_NE(std::find(h.model.batches.begin(), h.model.batches.end(), 1u), h.model.batches.end());
    // The 19 good events have their Threads; the bad one is set aside as uncertain without one.
    for (int i = 0; i < 20; ++i) EXPECT_EQ(h.thread_of("m" + std::to_string(i)).empty(), i == 7) << i;
    EXPECT_EQ(h.svc->status()["uncertain"].get<long long>(), 1);
    // Successful batches double the size again, up to the full batch.
    const size_t before = h.model.batches.size();
    std::vector<PageChange> more;
    for (int i = 20; i < 80; ++i) more.push_back(h.message("m" + std::to_string(i), "Budget " + std::to_string(i), "Please review [not]"));
    ASSERT_TRUE(h.take(more));
    for (int attempt = 0; attempt < 12 && h.svc->status()["pending"].get<long long>() > 0; ++attempt) ASSERT_TRUE(h.classify());
    EXPECT_EQ(h.svc->status()["pending"].get<long long>(), 0);
    const std::vector<size_t> later(h.model.batches.begin() + static_cast<long>(before), h.model.batches.end());
    for (size_t i = 1; i < later.size(); ++i)
        if (later[i - 1] < 20u && later[i] != std::min<size_t>(20u, later[i - 1] * 2)) {
            // Only the last batch may be smaller: it takes what is left.
            EXPECT_EQ(i, later.size() - 1) << i;
        }
    EXPECT_NE(std::find(later.begin(), later.end(), 20u), later.end());
}

// An unavailable gateway keeps the batch size and the doubling wait.
TEST(EventTriage, AnUnavailableGatewayKeepsTheBatchAndBacksOff) {
    Harness h;
    h.enable();
    std::vector<PageChange> changes;
    for (int i = 0; i < 20; ++i) changes.push_back(h.message("m" + std::to_string(i), "Budget", "Please review"));
    ASSERT_TRUE(h.take(changes));
    h.model.fail = true;
    ASSERT_TRUE(h.classify());
    ASSERT_TRUE(h.classify());  // 61 s later: the first wait was one minute
    EXPECT_FALSE(h.classify());  // the second wait is two minutes
    h.model.fail = false;
    ASSERT_TRUE(h.classify());
    EXPECT_EQ(h.model.last_batch, 20u);
}

// add-evaluation-export (client/evaluation User Decision Log): Dismiss, No action needed and Start work are logged with
// the result they decided on, and a later decision keeps the earlier one.
TEST(EventTriage, DecisionsAreLogged) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    const std::string t = h.thread_of("m1");
    ASSERT_TRUE(h.svc->dismiss(t));
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the updated budget")}));
    ASSERT_TRUE(h.classify());
    ASSERT_TRUE(h.svc->start_work(t));
    const auto log = h.log->for_item("mail:m1");
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].kind, "dismiss");
    EXPECT_EQ(log[0].step, "triage");
    EXPECT_EQ(log[0].result_ref, "triage:mail/m1");
    EXPECT_EQ(log[0].before["action"], "required");
    EXPECT_EQ(log[0].before["suggestion"], "open");
    EXPECT_EQ(log[0].after["suggestion"], "dismissed");
    EXPECT_EQ(log[0].instruction_id, eval::Variant::defaults(eval::Step::triage).instruction_id());
    EXPECT_EQ(log[1].kind, "start");
    EXPECT_EQ(log[1].after["suggestion"], "started");

    ASSERT_TRUE(h.take({h.message("m2", "Invoice", "Please approve the invoice")}));
    ASSERT_TRUE(h.classify());
    ASSERT_TRUE(h.svc->no_action(h.thread_of("m2")));
    const auto second = h.log->for_item("mail:m2");
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].kind, "no_action");
    EXPECT_EQ(second[0].after["suggestion"], "no_action");
}

// add-evaluation-export task 3.3: a new triage result records the instructions that produced it; older rows read unknown.
TEST(InstructionId, StoredOnNewTriageResults) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    ASSERT_TRUE(h.classify());
    auto q = h.db.store->db().prepare("SELECT instruction_id FROM triage_items WHERE item_id='m1'");
    ASSERT_TRUE(q.step());
    EXPECT_EQ(q.col_text(0), eval::Variant::defaults(eval::Step::triage).instruction_id());
}

TEST(InstructionId, OldTriageRowsReadUnknown) {
    Harness h;
    ASSERT_TRUE(h.db.store->db().exec("INSERT INTO triage_items(scope, item_id, revision, state, queued_at, updated_at) "
                                      "VALUES('mail', 'old', 'r1', 'evaluated', 1, 1)"));
    auto q = h.db.store->db().prepare("SELECT instruction_id FROM triage_items WHERE item_id='old'");
    ASSERT_TRUE(q.step());
    EXPECT_EQ(q.col_text(0), "unknown");
}

// client/telemetry Quality Counters: a failed batch raises batch_failed_<kind>.
TEST(EventTriage, FailedBatchesAreCounted) {
    Harness h;
    h.enable();
    ASSERT_TRUE(h.take({h.message("m1", "Budget Q4", "Please review the budget")}));
    h.model.fail = true;
    EXPECT_TRUE(h.classify());
    ASSERT_EQ(h.quality.size(), 1u);
    EXPECT_EQ(h.quality[0], "batch_failed_unavailable");
}
