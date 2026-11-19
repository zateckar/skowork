// client/agenda service (add-agenda tasks 2.4, 3.2, 4.1-4.4, 5.1, 6.2): opt-in and the enablement baseline, detection
// through a fake small model, admission counters, merging, dismissal, withdrawal, user edits, preparation, reminders
// and deletion, on a real encrypted app.db.
#include "agenda/service.h"
#include "app/agenda_intake.h"
#include "base/time.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/database.h"
#include "storage/keystore.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace wb;

namespace {

constexpr int kCet = 60;
long long ms(const char* iso) { return *parse_iso8601_ms(iso); }

// Tuesday 17 November 2026, 11:00 local.
const long long kEnabled = ms("2026-11-17T09:00:00Z");
const long long kNow = ms("2026-11-17T10:00:00Z");

class FakeModel final : public model::IChatModel {
public:
    bool fail = false;
    int requests = 0;
    std::string text;
    std::string merge_answer = R"({"same":null})";
    bool no_candidates = false;          // answer every detected item with no candidates
    std::vector<size_t> batch_sizes;     // items per detection request
    model::ChatRequest last;
    std::optional<model::RoleInfo> role_info(model::Role role) override {
        if (role != model::Role::small) return std::nullopt;
        model::RoleInfo info;
        info.role = role;
        info.kind = "chat";
        return info;
    }
    model::ModelResult<model::ChatResponse> chat(const model::ChatRequest& r, const model::ChatSink&) override {
        ++requests;
        last = r;
        if (fail) return model::ModelFailure{"TIMEOUT", "timeout", "", 0, "", 0, true, false};
        model::ChatResponse resp;
        resp.finish_reason = "stop";
        resp.text = r.feature == "agenda.merge" ? merge_answer : text;
        if (r.feature == "agenda.detect") {
            const json data = json::parse(r.messages.back().content);
            batch_sizes.push_back(data["items"].size());
            if (no_candidates) {
                json items = json::array();
                for (const auto& it : data["items"]) items.push_back({{"id", it["id"]}, {"candidates", json::array()}});
                resp.text = json{{"items", items}}.dump();
            }
        }
        return resp;
    }
};

struct Rig {
    std::filesystem::path dir = std::filesystem::path(local_appdata()) / L"skowork-test" / widen(random_urlsafe(6));
    std::unique_ptr<Database> db;
    FakeModel model;
    long long now = kNow;
    std::map<std::string, json> policy;
    bool presentation = false;
    std::function<int(long long)> tz = [](long long) { return kCet; };
    std::vector<std::tuple<std::string, std::string, std::string>> notes;  // title, body, route
    std::vector<std::string> prepared;                                        // messages of started preparations
    std::string preamble;
    bool unattended = false;
    int runs = 0;
    std::unique_ptr<agenda::Service> svc;

    Rig() {
        auto root = DataRoot::open(dir);
        EXPECT_TRUE(root);
        KeyStore keys(root->keys());
        EXPECT_TRUE(keys.load());
        auto k = keys.get_or_create("app.db");
        MigrationOutcome o;
        auto opened = open_and_migrate(root->file("app.db"), *k, app_migrations(), o);
        EXPECT_TRUE(opened);
        db = std::move(*opened);
        make();
    }
    ~Rig() {
        svc.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void make() {
        agenda::Env e;
        e.policy = [this](const std::string& key) -> std::optional<json> {
            const auto it = policy.find(key);
            return it == policy.end() ? std::nullopt : std::optional<json>(it->second);
        };
        e.model = &model;
        e.now = [this] { return now; };
        e.tz_offset_min = [this](long long t) { return tz(t); };
        e.identity = [] { return agenda::Identity{"jana novakova", {"jana.novakova@example.cz"}}; };
        e.start_preparation = [this](const agenda::Item&, const std::string& thread, const std::string& message, const std::string& pre,
                                     bool un) -> Result<agenda::Preparation> {
            prepared.push_back(message);
            preamble = pre;
            unattended = un;
            ++runs;
            return agenda::Preparation{thread.empty() ? "t-" + std::to_string(runs) : thread, "run-" + std::to_string(runs)};
        };
        e.notify = [this](const std::string& t, const std::string& b, const std::string& r) { notes.emplace_back(t, b, r); };
        e.hold_reminders = [this] { return presentation; };
        svc = std::make_unique<agenda::Service>(*db, std::move(e));
    }
    void enable() {
        const long long keep = now;
        now = kEnabled;
        ASSERT_TRUE(svc->set({{"enabled", true}}));
        now = keep;
    }
};

agenda::SourceItem mail(const std::string& id, const std::string& text, const std::string& conversation = "conv-1") {
    agenda::SourceItem it;
    it.kind = "mail";
    it.id = id;
    it.conversation = conversation;
    it.title = "Q4 slides";
    it.text = text;
    it.author = "Petr Svoboda";
    it.user_addressed = true;
    it.recipients = 1;
    it.at_ms = ms("2026-11-17T09:30:00Z");
    it.link = "https://outlook.example/" + id;
    return it;
}

std::string candidate(const std::string& quote, const std::string& due = "2026-11-20", const std::string& title = "Send the Q4 slides",
                      double confidence = 0.9) {
    // "by Friday" only with Friday 20 November; other dates carry no phrase to check against.
    const std::string phrase = due == "2026-11-20" ? "by Friday" : "";
    return json{{"title", title},          {"action", "Send Petr the Q4 slides"}, {"due", due},
                {"due_phrase", phrase},    {"due_kind", "inferred"},           {"responsibility", "requested"},
                {"quote", quote},          {"confidence", confidence},          {"draft_kind", "reply"}}
        .dump();
}

std::string answer(const std::string& id, const std::string& candidates) {
    return R"({"items":[{"id":")" + id + R"(","candidates":[)" + candidates + "]}]}";
}

const char* kAsk = "Could you send me the Q4 slides by Friday?";

}  // namespace

TEST(AgendaService, IsOffByDefaultAndScansNothingBeforeEnablement) {
    Rig r;
    EXPECT_TRUE(r.svc->available());
    EXPECT_FALSE(r.svc->active());
    r.svc->offer(mail("m1", kAsk));
    EXPECT_EQ(jint(r.svc->status(), "pending"), 0);
    r.enable();
    auto old = mail("m0", kAsk);
    old.at_ms = kEnabled - 1;  // arrived before the Agenda was enabled
    r.svc->offer(old);
    EXPECT_EQ(jint(r.svc->status(), "pending"), 0);
    r.svc->offer(mail("m1", kAsk));
    EXPECT_EQ(jint(r.svc->status(), "pending"), 1);
    // A source kind the user did not select is not queued.
    ASSERT_TRUE(r.svc->set({{"chats", false}}));
    auto chat = mail("c1", kAsk);
    chat.kind = "chat";
    r.svc->offer(chat);
    EXPECT_EQ(jint(r.svc->status(), "pending"), 1);
}

TEST(AgendaService, PolicyOffMakesItUnavailable) {
    Rig r;
    r.policy["agenda.mode"] = "off";
    EXPECT_FALSE(r.svc->available());
    EXPECT_FALSE(r.svc->set({{"enabled", true}}));
    EXPECT_FALSE(r.svc->active());
}

TEST(AgendaService, DetectsAdmitsAndMergesWithOneSmallToolFreeRequest) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides") + "," + candidate("an invented quote", "2026-11-21", "Book a room"));
    r.svc->detect_now();
    EXPECT_EQ(r.model.requests, 1);
    EXPECT_EQ(r.model.last.role, model::Role::small);
    EXPECT_TRUE(r.model.last.tools.empty());
    EXPECT_EQ(r.model.last.feature, "agenda.detect");
    const json list = r.svc->list();
    ASSERT_EQ(list["items"].size(), 1u);
    const json& item = list["items"][0];
    EXPECT_EQ(jstr(item, "title"), "Send the Q4 slides");
    EXPECT_TRUE(jbool(item, "inferred"));
    EXPECT_FALSE(jbool(item, "has_time"));
    EXPECT_EQ(jint(item, "due_at"), ms("2026-11-20T22:59:00Z"));  // the end of Friday, local time
    const json st = r.svc->status();
    EXPECT_EQ(jint(st, "pending"), 0);
    EXPECT_EQ(jint(st["dropped"], "quote"), 1);  // counted, not stored
    // A follow-up in the same conversation about the same commitment merges, keeping the earlier date.
    r.svc->offer(mail("m2", "Reminder: the Q4 slides, please send them by Thursday."));
    r.model.text = answer("m2", candidate("the Q4 slides, please send them by Thursday", "2026-11-19"));
    r.svc->detect_now();
    const json merged = r.svc->list();
    ASSERT_EQ(merged["items"].size(), 1u);
    EXPECT_EQ(merged["items"][0]["sources"].size(), 2u);
    EXPECT_EQ(jint(merged["items"][0], "due_at"), ms("2026-11-19T22:59:00Z"));
}

TEST(AgendaService, AFailedOrMalformedDetectionKeepsTheItemsForLater) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.fail = true;
    r.svc->detect_now();
    EXPECT_EQ(jint(r.svc->status(), "pending"), 1);
    EXPECT_EQ(jstr(r.svc->status(), "last_error"), "TIMEOUT");
    r.model.fail = false;
    r.model.text = "Sure! Here are the tasks: send slides";
    r.now += 3'600'000;  // past the retry delay
    r.svc->detect_now();
    EXPECT_EQ(jint(r.svc->status(), "pending"), 1);
    EXPECT_EQ(jstr(r.svc->status(), "last_error"), "invalid_response");
    EXPECT_TRUE(r.svc->list()["items"].empty());
}

TEST(AgendaService, NoiseLowConfidenceAndPastDatesAreDropped) {
    Rig r;
    r.enable();
    auto memo = mail("m1", "All staff: the canteen menu changes. Please read the new hygiene rules by Friday.");
    memo.user_addressed = false;
    memo.recipients = 900;
    r.svc->offer(memo);
    r.svc->offer(mail("m2", kAsk, "conv-2"));
    r.svc->offer(mail("m3", "Can you send the report by Monday?", "conv-3"));
    r.model.text = R"({"items":[{"id":"m1","candidates":[)" + candidate("read the new hygiene rules by Friday") + R"(]},)" +
                   R"({"id":"m2","candidates":[)" + candidate("send me the Q4 slides", "2026-11-20", "Send the Q4 slides", 0.5) + R"(]},)" +
                   R"({"id":"m3","candidates":[)" + candidate("send the report by Monday", "2026-11-16", "Send the report") + "]}]}";
    r.svc->detect_now();
    EXPECT_TRUE(r.svc->list()["items"].empty());
    const json d = r.svc->status()["dropped"];
    EXPECT_EQ(jint(d, "broadcast"), 1);
    EXPECT_EQ(jint(d, "confidence"), 1);
    EXPECT_EQ(jint(d, "date") + jint(d, "past"), 1);
}

TEST(AgendaService, DismissedCommitmentsAreNotAddedAgainAndWithdrawalRemovesItems) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    const std::string id = jstr(r.svc->list()["items"][0], "id");
    ASSERT_TRUE(r.svc->dismiss(id));
    r.svc->offer(mail("m2", "Did you send me the Q4 slides yet?"));
    r.model.text = answer("m2", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    EXPECT_TRUE(r.svc->list()["items"].empty());
    EXPECT_EQ(jint(r.svc->status()["dropped"], "dismissed"), 1);

    r.svc->offer(mail("m9", "Please review the budget by 2026-11-24.", "conv-9"));
    r.model.text = answer("m9", candidate("review the budget by 2026-11-24", "2026-11-24", "Review the budget"));
    r.svc->detect_now();
    ASSERT_EQ(r.svc->list()["items"].size(), 1u);
    r.svc->withdraw("mail", "m9");  // the mail was deleted
    EXPECT_TRUE(r.svc->list()["items"].empty());
}

TEST(AgendaService, UserEditsWinOverLaterSourceRevisions) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    const std::string id = jstr(r.svc->list()["items"][0], "id");
    auto edited = r.svc->edit(id, {{"due", "2026-11-23T14:00"}, {"title", "Q4 slides to Petr"}});
    ASSERT_TRUE(edited) << edited.error().message;
    EXPECT_TRUE(jbool(*edited, "has_time"));
    EXPECT_EQ(jint(*edited, "due_at"), ms("2026-11-23T13:00:00Z"));
    EXPECT_FALSE(r.svc->edit(id, {{"due", "next week"}}));
    // A revision of the same mail with another date keeps the user's date and says so.
    auto rev = mail("m1", "Could you send me the Q4 slides by Thursday?");
    rev.at_ms += 60'000;
    r.svc->offer(rev);
    r.model.text = answer("m1", candidate("send me the Q4 slides", "2026-11-19", "Q4 slides to Petr"));
    r.svc->detect_now();
    const json item = r.svc->list()["items"][0];
    EXPECT_EQ(jint(item, "due_at"), ms("2026-11-23T13:00:00Z"));
    EXPECT_EQ(jstr(item, "notice"), "source_date_differs");
    EXPECT_EQ(jstr(item, "title"), "Q4 slides to Petr");
}

TEST(AgendaService, PreparationStartsOnceWithAReadOnlyPreambleAndUntrustedData) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    const std::string id = jstr(r.svc->list()["items"][0], "id");
    auto p = r.svc->prepare(id);
    ASSERT_TRUE(p) << p.error().message;
    EXPECT_EQ(r.runs, 1);
    EXPECT_FALSE(r.unattended);
    EXPECT_NE(r.preamble.find("Do not send"), std::string::npos);
    EXPECT_NE(r.preamble.find("untrusted"), std::string::npos);
    EXPECT_NE(r.prepared[0].find("send me the Q4 slides"), std::string::npos);  // the quote travels as data
    EXPECT_EQ(jstr(*p, "prep_thread_id"), "t-1");
    EXPECT_TRUE(r.svc->is_preparation_run("run-1"));
    EXPECT_FALSE(r.svc->is_preparation_run("run-2"));
    ASSERT_TRUE(r.svc->prepare(id));  // Open preparation: no second Run
    EXPECT_EQ(r.runs, 1);
}

TEST(AgendaService, AutomaticPreparationNeedsPolicyAndTheUsersChoice) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    ASSERT_TRUE(r.svc->set({{"auto_prepare", true}}));
    EXPECT_FALSE(jbool(r.svc->status(), "auto_prepare"));  // the policy does not allow it
    r.svc->tick();
    EXPECT_EQ(r.runs, 0);
    r.policy["agenda.auto_prepare"] = true;
    ASSERT_TRUE(r.svc->set({{"auto_prepare", true}}));
    r.svc->tick();
    EXPECT_EQ(r.runs, 1);
    EXPECT_TRUE(r.unattended);
    r.svc->tick();
    EXPECT_EQ(r.runs, 1);
}

TEST(AgendaService, RemindsOnTheWorkingDayBeforeOnceAndHoldsDuringPresentations) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", "Please send me the Q4 slides by Monday 23 November."));
    r.model.text = answer("m1", candidate("send me the Q4 slides by Monday", "2026-11-23", "Send the Q4 slides"));
    r.svc->detect_now();
    ASSERT_EQ(r.svc->list()["items"].size(), 1u);
    r.now = ms("2026-11-20T07:30:00Z");  // Friday 08:30 local: too early
    r.svc->tick();
    EXPECT_TRUE(r.notes.empty());
    r.now = ms("2026-11-20T08:05:00Z");  // Friday 09:05 local, the working day before Monday
    r.presentation = true;
    r.svc->tick();
    EXPECT_TRUE(r.notes.empty());
    r.presentation = false;
    r.svc->tick();
    ASSERT_EQ(r.notes.size(), 1u);
    EXPECT_EQ(std::get<0>(r.notes[0]), "Send the Q4 slides");
    EXPECT_EQ(std::get<2>(r.notes[0]), "#/agenda");
    r.svc->tick();
    EXPECT_EQ(r.notes.size(), 1u);
}

TEST(AgendaService, DeleteAllRemovesItemsQueueAndCounts) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides") + "," + candidate("nowhere in the text", "2026-11-21", "Other"));
    r.svc->detect_now();
    r.svc->offer(mail("m2", kAsk, "conv-2"));
    ASSERT_TRUE(r.svc->delete_all());
    const json st = r.svc->status();
    EXPECT_EQ(jint(st, "open"), 0);
    EXPECT_EQ(jint(st, "pending"), 0);
    EXPECT_EQ(jint(st, "dropped_total"), 0);
}

TEST(AgendaService, TheSmallModelDecidesMergesAcrossConversations) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    const std::string id = jstr(r.svc->list()["items"][0], "id");
    // Another conversation, other words, the same work: the merge decision says so.
    r.svc->offer(mail("m2", "Petr needs the quarterly deck before the board call.", "conv-2"));
    r.model.merge_answer = R"({"same":")" + id + R"("})";
    r.model.text = answer("m2", candidate("Petr needs the quarterly deck", "2026-11-20", "Hand over quarterly deck"));
    r.svc->detect_now();
    EXPECT_EQ(r.model.last.feature, "agenda.merge");
    ASSERT_EQ(r.svc->list()["items"].size(), 1u);
    EXPECT_EQ(r.svc->list()["items"][0]["sources"].size(), 2u);
    // "Different" keeps them apart.
    r.svc->offer(mail("m3", "Could you book the room for the deck review?", "conv-3"));
    r.model.merge_answer = R"({"same":null})";
    r.model.text = answer("m3", candidate("book the room for the deck review", "2026-11-20", "Book the review room"));
    r.svc->detect_now();
    EXPECT_EQ(r.svc->list()["items"].size(), 2u);
}

TEST(AgendaService, ClosedItemsAreDeletedNinetyDaysAfterTheirStateChanged) {
    Rig r;
    r.enable();
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    const std::string id = jstr(r.svc->list()["items"][0], "id");
    ASSERT_TRUE(r.svc->mark_done(id));
    r.now += 89LL * 86'400'000;
    r.svc->tick();
    EXPECT_TRUE(r.svc->reopen(id));  // still there
    ASSERT_TRUE(r.svc->mark_done(id));
    r.now += 91LL * 86'400'000;
    r.svc->tick();
    EXPECT_FALSE(r.svc->reopen(id));
}

TEST(AgendaService, TheQueueKeepsTheNewestFiveHundredAndCountsTheRest) {
    Rig r;
    r.enable();
    for (int i = 0; i < 503; ++i) r.svc->offer(mail("m" + std::to_string(i), kAsk, "conv-" + std::to_string(i)));
    const json st = r.svc->status();
    EXPECT_EQ(jint(st, "pending"), 500);
    EXPECT_EQ(jint(st, "overflow"), 3);
}

TEST(AgendaService, MeetingTasksAreAdmittedOnlyWhenTheyNameTheUserAndLeaveWithTheMeeting) {
    Rig r;
    r.enable();
    auto rec = [](const std::string& id, const std::string& who) {
        meeting::MeetingRecord m;
        m.id = id;
        m.thread_id = "th-" + id;
        m.title = "Budget review";
        m.event_id = "ev-" + id;
        m.attendees = {"a", "b", "c", "d"};
        m.started_at = ms("2026-11-17T09:10:00Z");
        m.ended_at = ms("2026-11-17T09:40:00Z");
        const json turns = json::array({{{"speaker", "Petr Svoboda"}, {"text", who + ", please send the cost estimate by 24 November."}},
                                        {{"speaker", "Petr Svoboda"}, {"text", "Thanks, see you next week."}}});
        return *agenda_meeting_item(m, turns);
    };
    const std::string task = candidate("please send the cost estimate by 24 November", "2026-11-24", "Send the cost estimate");
    r.svc->offer(rec("mt-other", "Martin"));
    r.model.text = answer("mt-other", task);
    r.svc->detect_now();
    EXPECT_TRUE(r.svc->list()["items"].empty());
    EXPECT_EQ(jint(r.svc->status()["dropped"], "responsibility"), 1);

    r.svc->offer(rec("mt-1", "Jana Novakova"));
    r.model.text = answer("mt-1", task);
    r.svc->detect_now();
    const json items = r.svc->list()["items"];
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(jstr(items[0]["sources"][0], "link"), "#/threads/th-mt-1");
    r.svc->withdraw("meeting", "mt-1");  // the meeting was deleted
    EXPECT_TRUE(r.svc->list()["items"].empty());
}

TEST(AgendaService, ChatMessagesAreBatchedAtMostOncePerIntervalFromEnablementOn) {
    Rig r;
    const json chat = {{"kind", "direct"}, {"members", json::array({"petr", "jana"})}};
    auto push = [&](int i, const char* at) {
        const json payload = {{"chat", "c-1"},
                              {"message", {{"id", "msg-" + std::to_string(i)}, {"text", "Message " + std::to_string(i)}, {"sender", "petr"}, {"at", at}}}};
        r.svc->offer(*agenda_chat_item(payload, chat, "jana", r.now));
    };
    r.enable();
    push(0, "2026-11-17T08:59:00Z");  // history before enablement
    for (int i = 1; i <= 12; ++i) push(i, "2026-11-17T09:30:00Z");
    EXPECT_EQ(jint(r.svc->status(), "pending"), 12);
    r.model.no_candidates = true;
    r.svc->tick();
    r.svc->tick();  // within the interval: no second request
    ASSERT_EQ(r.model.batch_sizes, std::vector<size_t>{10});
    EXPECT_EQ(jint(r.svc->status(), "pending"), 2);
    r.now += agenda::kRequestIntervalMs;
    r.svc->tick();
    EXPECT_EQ(r.model.batch_sizes, (std::vector<size_t>{10, 2}));
    EXPECT_EQ(jint(r.svc->status(), "pending"), 0);
}

TEST(AgendaService, AutomaticPreparationIsRefreshedOnceTheWorkingDayBeforeAfterASourceChanged) {
    Rig r;
    r.policy["agenda.auto_prepare"] = true;
    r.enable();
    ASSERT_TRUE(r.svc->set({{"auto_prepare", true}}));
    r.svc->offer(mail("m1", "Please send me the Q4 slides by Monday 23 November."));
    r.model.text = answer("m1", candidate("send me the Q4 slides by Monday", "2026-11-23", "Send the Q4 slides"));
    r.svc->detect_now();
    r.svc->tick();
    ASSERT_EQ(r.runs, 1);
    // Thursday: a newer revision of the source, but it is not yet the working day before Monday.
    r.now = ms("2026-11-19T10:00:00Z");
    auto rev = mail("m1", "Please send me the Q4 slides by Monday 23 November, with the risks slide.");
    rev.at_ms = r.now;
    r.svc->offer(rev);
    r.model.text = answer("m1", candidate("send me the Q4 slides by Monday", "2026-11-23", "Send the Q4 slides"));
    r.svc->detect_now();
    r.svc->tick();
    EXPECT_EQ(r.runs, 1);
    r.now = ms("2026-11-20T08:00:00Z");  // Friday
    r.svc->tick();
    ASSERT_EQ(r.runs, 2);
    EXPECT_TRUE(r.unattended);
    r.svc->tick();
    EXPECT_EQ(r.runs, 2);
    // Another change after the refresh: no second refresh.
    rev.at_ms = r.now + 60'000;
    r.now += 120'000;
    r.svc->offer(rev);
    r.svc->detect_now();
    r.svc->tick();
    EXPECT_EQ(r.runs, 2);
}

TEST(AgendaService, AutomaticPreparationNeedsUnattendedRunsThatMayRead) {
    Rig r;
    r.policy["agenda.auto_prepare"] = true;
    r.policy["unattended.allowed_risk"] = json::array();
    r.enable();
    ASSERT_TRUE(r.svc->set({{"auto_prepare", true}}));
    r.svc->offer(mail("m1", kAsk));
    r.model.text = answer("m1", candidate("send me the Q4 slides"));
    r.svc->detect_now();
    r.svc->tick();
    EXPECT_EQ(r.runs, 0);
    r.policy["unattended.allowed_risk"] = json::array({"read"});
    r.svc->tick();
    EXPECT_EQ(r.runs, 1);
}

TEST(AgendaService, ADueTimeAfterTheSwitchToSummerTimeKeepsItsLocalTime) {
    Rig r;
    const long long summer = ms("2027-03-28T01:00:00Z");
    r.tz = [summer](long long t) { return t < summer ? 60 : 120; };
    r.enable();
    r.now = ms("2027-03-26T09:00:00Z");  // Friday 10:00 CET
    auto m = mail("m1", "Please call me on Monday at 10:00 about the Q4 slides.");
    m.at_ms = ms("2027-03-26T08:30:00Z");
    r.svc->offer(m);
    r.model.text = answer("m1", json{{"title", "Call Petr"}, {"action", "Call Petr about the Q4 slides"}, {"due", "2027-03-29T10:00"},
                                     {"due_phrase", "on Monday at 10:00"}, {"due_kind", "explicit"}, {"responsibility", "requested"},
                                     {"quote", "call me on Monday at 10:00"}, {"confidence", 0.9}, {"draft_kind", "none"}}
                                    .dump());
    r.svc->detect_now();
    const json items = r.svc->list()["items"];
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(jint(items[0], "due_at"), ms("2027-03-29T08:00:00Z"));  // 10:00 CEST
    auto edited = r.svc->edit(jstr(items[0], "id"), {{"due", "2027-03-30T14:00"}});
    ASSERT_TRUE(edited) << edited.error().message;
    EXPECT_EQ(jint(*edited, "due_at"), ms("2027-03-30T12:00:00Z"));
}
