// The window's view models (add-native-ui group 3), ported with the web UI's tests (feed.test.ts,
// inbox-model.test.ts) so the native screens keep the behaviour client/work-inbox specifies.
#include <gtest/gtest.h>

#include <chrono>

#include "shell/derive.h"
#include "shell/feed.h"
#include "shell/format.h"
#include "shell/inbox_model.h"
#include "shell/routes.h"
#include "shell/shortcuts.h"
#include "ui/bundles.h"

using namespace wb;
using namespace wb::shell;

namespace {

const std::string T = "thr_1";

RunInfo run(const std::string& id, const std::string& state = "queued") {
    RunInfo r;
    r.id = id;
    r.state = state;
    r.role = "main";
    r.created_at = "2026-10-23T10:00:00Z";
    return r;
}

FeedSnapshot snapshot(std::vector<RunInfo> runs) {
    FeedSnapshot s;
    s.entry.id = T;
    s.runs = std::move(runs);
    return s;
}

// A live event as the host forwards it; persisted kinds get the next sequence number of their Run.
struct Recorder {
    std::string run_id;
    long long seq = 0;
    LiveRunEvent operator()(const std::string& kind, json data = json::object(), bool persisted = true) {
        LiveRunEvent e;
        e.kind = kind;
        e.run_id = run_id;
        e.thread_id = T;
        e.seq = persisted ? ++seq : 0;
        char at[32];
        std::snprintf(at, sizeof at, "2026-10-23T10:00:%02lldZ", seq);
        e.at = at;
        e.data = std::move(data);
        return e;
    }
};

std::vector<Apply> replay(FeedSnapshot& snap, LiveText& live, const std::vector<LiveRunEvent>& events) {
    std::vector<Apply> out;
    for (const auto& e : events) out.push_back(apply_live_event(snap, live, e));
    return out;
}

std::vector<std::string> summary(const std::vector<FeedItem>& items) {
    std::vector<std::string> out;
    for (const FeedItem& i : items) switch (i.type) {
            case ItemType::user: out.push_back("user:" + i.text); break;
            case ItemType::assistant: out.push_back(std::string("assistant") + (i.streaming ? "*" : "") + ":" + i.text); break;
            case ItemType::tool:
                out.push_back("tool:" + i.tool + ":" + (i.result ? (i.result->ok ? "ok" : "err") : "pending") +
                              (i.edited ? ":edited" : ""));
                break;
            case ItemType::reasoning: out.push_back(std::string("reasoning") + (i.streaming ? "*" : "") + ":" + i.text); break;
            case ItemType::action: out.push_back("action:" + i.action.kind + ":" + (i.action.open() ? "open" : "resolved")); break;
            case ItemType::error: out.push_back("error:" + i.error.code); break;
            case ItemType::stopped: out.push_back(std::string("stopped:") + (i.by_user ? "true" : "false")); break;
            case ItemType::activity: out.push_back("activity"); break;
            case ItemType::working: out.push_back("working"); break;
        }
    return out;
}

using V = std::vector<std::string>;

std::vector<LiveRunEvent> recorded_stream() {
    // Recorded from the runtime's event order: state, user message, streamed text, tool call and result, answer.
    Recorder ev{"run_1"};
    return {
        ev("run.state", {{"state", "queued"}, {"reason", ""}}),
        ev("message", {{"role", "user"}, {"content", "Read budget.xlsx"}}),
        ev("run.state", {{"state", "running"}, {"reason", ""}}),
        ev("reasoning.delta", {{"text", "Need the "}}, false),
        ev("reasoning.delta", {{"text", "file."}}, false),
        ev("text.delta", {{"text", "Let me "}}, false),
        ev("text.delta", {{"text", "look."}}, false),
        ev("reasoning", {{"text", "Need the file."}}),
        ev("message", {{"role", "assistant"}, {"content", "Let me look."},
                       {"tool_calls", json::array({{{"id", "c1"}, {"tool", "files.read"}, {"arguments", "{}"}}})}}),
        ev("tool_call", {{"call_id", "c1"}, {"tool", "files.read"}, {"args", {{"path", "budget.xlsx"}}}, {"rationale", ""},
                         {"risk", "read"}, {"decision", "allow"}, {"reason", ""}}),
        ev("tool_result", {{"call_id", "c1"}, {"tool", "files.read"}, {"ok", true}, {"content", "Travel: 48 000 CZK"}}),
        ev("message", {{"role", "tool"}, {"content", "Travel: 48 000 CZK"}, {"tool_call_id", "c1"}}),
        ev("text.delta", {{"text", "The travel budget is 48 000 CZK."}}, false),
        ev("message", {{"role", "assistant"}, {"content", "The travel budget is 48 000 CZK."}}),
        ev("usage", {{"prompt", 10}, {"completion", 5}}, false),
        ev("run.state", {{"state", "succeeded"}, {"reason", ""}}),
    };
}

}  // namespace

TEST(ShellFeed, StreamsTextThenTheStoredMessageInItsPlace) {
    const auto stream = recorded_stream();
    FeedSnapshot snap = snapshot({run("run_1")});
    LiveText live;
    for (size_t i = 0; i < 7; ++i) apply_live_event(snap, live, stream[i]);
    EXPECT_EQ(summary(build_feed(snap, live)), (V{"user:Read budget.xlsx", "reasoning*:Need the file.", "assistant*:Let me look."}));
    for (size_t i = 7; i < 9; ++i) apply_live_event(snap, live, stream[i]);
    EXPECT_EQ(summary(build_feed(snap, live)),
              (V{"user:Read budget.xlsx", "reasoning:Need the file.", "assistant:Let me look.", "working"}));
}

TEST(ShellFeed, EndsWithToolCardsAndTheAnswerLikeTheStoredLog) {
    FeedSnapshot snap = snapshot({run("run_1")});
    LiveText live;
    for (Apply a : replay(snap, live, recorded_stream())) EXPECT_EQ(a, Apply::ok);
    const auto items = build_feed(snap, live);
    EXPECT_EQ(summary(items), (V{"user:Read budget.xlsx", "reasoning:Need the file.", "assistant:Let me look.",
                                 "tool:files.read:ok", "assistant:The travel budget is 48 000 CZK."}));
    const FeedItem& tool = items[3];
    EXPECT_EQ(tool.risk, "read");
    EXPECT_EQ(tool.args, json({{"path", "budget.xlsx"}}));
    EXPECT_EQ(tool.result->content, "Travel: 48 000 CZK");
    // Opening the Thread later folds the stored log into the same feed (design D13).
    RunInfo stored_run = run("run_1", "succeeded");
    stored_run.events = snap.runs[0].events;
    EXPECT_EQ(summary(build_feed(snapshot({stored_run}))), summary(items));
}

TEST(ShellFeed, IgnoresEventsAlreadyInTheSnapshotAndOfOtherThreads) {
    const auto stream = recorded_stream();
    FeedSnapshot snap = snapshot({run("run_1")});
    LiveText live;
    replay(snap, live, stream);
    const size_t before = snap.runs[0].events.size();
    LiveText again;
    replay(snap, again, stream);
    EXPECT_EQ(snap.runs[0].events.size(), before);
    LiveRunEvent other = stream[1];
    other.thread_id = "other";
    other.seq = 99;
    EXPECT_EQ(apply_live_event(snap, again, other), Apply::ok);
    EXPECT_EQ(snap.runs[0].events.size(), before);
}

TEST(ShellFeed, MissedEventsAskForANewSnapshot) {
    const auto stream = recorded_stream();
    FeedSnapshot snap = snapshot({run("run_1")});
    LiveText live;
    EXPECT_EQ(apply_live_event(snap, live, stream[0]), Apply::ok);
    // The user message (seq 2) went by while a snapshot was loading: the answer's events have a gap.
    EXPECT_EQ(apply_live_event(snap, live, stream[2]), Apply::refetch);
    // Streaming text of a Run the snapshot does not have yet is kept for it.
    LiveRunEvent other = stream[5];
    other.run_id = "run_new";
    FeedSnapshot without = snapshot({});
    EXPECT_EQ(apply_live_event(without, live, other), Apply::refetch);
    without.runs.push_back(run("run_new", "running"));
    EXPECT_EQ(summary(build_feed(without, live)), (V{"assistant*:Let me "}));
}

TEST(ShellFeed, ApprovalCardAndEditedArguments) {
    Recorder ev{"run_2"};
    FeedSnapshot snap = snapshot({run("run_2", "running")});
    LiveText live;
    const json approval = {{"tool", "mail.send"}, {"args", {{"to", "a@b.cz"}}}, {"risk", "external"}, {"rationale", "asked"}};
    replay(snap, live,
           {ev("message", {{"role", "user"}, {"content", "Send it"}}),
            ev("tool_call", {{"call_id", "m1"}, {"tool", "mail.send"}, {"args", {{"to", "a@b.cz"}}}, {"rationale", "asked"},
                             {"risk", "external"}, {"decision", "approval"}, {"reason", "needs approval"}}),
            ev("action.created", {{"id", "act_1"}, {"kind", "approval"}, {"call_id", "m1"}, {"payload", approval}}),
            ev("run.state", {{"state", "interrupted"}, {"reason", ""}})});
    EXPECT_EQ(summary(build_feed(snap)), (V{"user:Send it", "tool:mail.send:pending", "action:approval:open"}));
    V kinds;
    for (const auto& e : snap.runs[0].events) kinds.push_back(e.kind);
    EXPECT_EQ(kinds, (V{"message", "tool_call", "interrupt", "state"}));  // as the stored log has them

    replay(snap, live,
           {ev("action.resolved", {{"id", "act_1"}, {"resolution", {{"kind", "approve_edited"}, {"args", {{"to", "c@d.cz"}}}}}}),
            ev("activity", {{"text", "The user edited the arguments of mail.send before approving it."},
                            {"before", {{"to", "a@b.cz"}}},
                            {"after", {{"to", "c@d.cz"}}}}),
            ev("run.state", {{"state", "running"}, {"reason", ""}}),
            ev("tool_result", {{"call_id", "m1"}, {"tool", "mail.send"}, {"ok", true}, {"content", "sent"}})});
    const auto items = build_feed(snap);
    EXPECT_EQ(summary(items), (V{"user:Send it", "tool:mail.send:ok:edited", "action:approval:resolved", "activity", "working"}));
    EXPECT_EQ(items[1].args, json({{"to", "c@d.cz"}}));
}

// fix-tool-waits 2.1: the Approval of an interrupted subagent is a card right after the call that started it.
TEST(ShellFeed, ChildRunApprovalIsACardUnderTheCallThatStartedTheSubagent) {
    FeedSnapshot snap = snapshot({run("run_p", "interrupted")});
    snap.runs[0].events = {
        {1, "", "message", {{"role", "user"}, {"content", "Send it"}}},
        {2, "", "tool_call", {{"call_id", "c1"}, {"tool", "delegate"}, {"args", json::object()}, {"decision", "allow"}}},
        {3, "", "subagent", {{"call_id", "c1"}, {"child_run_id", "run_c"}, {"task", "send the note"}}},
        {4, "", "tool_call", {{"call_id", "c2"}, {"tool", "find_tools"}, {"args", json::object()}, {"decision", "allow"}}},
    };
    RunInfo child = run("run_c", "interrupted");
    child.parent_run_id = "run_p";
    snap.subagents = {child};
    ActionItem a;
    a.id = "act_c";
    a.thread_id = T;
    a.run_id = "run_c";
    a.kind = "approval";
    a.call_id = "m1";
    a.payload = {{"tool", "mail.send"}, {"args", {{"to", "a@b.cz"}}}, {"risk", "external"}, {"rationale", "asked"}};
    a.created_at = "2026-10-23T10:00:05Z";
    snap.actions = {a};
    const auto items = build_feed(snap);
    EXPECT_EQ(summary(items), (V{"user:Send it", "tool:delegate:pending", "action:approval:open", "tool:find_tools:pending"}));
    EXPECT_EQ(items[2].run_id, "run_c");
    EXPECT_EQ(items[2].action.id, "act_c");

    // Answered: the card leaves the feed, as the Run resumes.
    snap.actions[0].resolved_at = "2026-10-23T10:01:00Z";
    EXPECT_EQ(summary(build_feed(snap)), (V{"user:Send it", "tool:delegate:pending", "tool:find_tools:pending"}));

    // A subagent whose call is not in the feed: the card is still drawn, at the end.
    snap.actions[0].resolved_at.clear();
    snap.runs[0].events.erase(snap.runs[0].events.begin() + 2);
    EXPECT_EQ(summary(build_feed(snap)), (V{"user:Send it", "tool:delegate:pending", "tool:find_tools:pending", "action:approval:open"}));
}

TEST(ShellFeed, StandaloneActionItemsFollowTheConversation) {
    FeedSnapshot snap = snapshot({});
    ActionItem a;
    a.id = "act_s";
    a.thread_id = T;
    a.kind = "question";
    a.payload = {{"prompt", "Connect your mailbox"}, {"choices", {"Connect", "Not now"}}, {"free_text", false}};
    a.raised_by = "dev.test";
    snap.actions.push_back(a);
    EXPECT_EQ(summary(build_feed(snap)), (V{"action:question:open"}));
    LiveText live;
    LiveRunEvent created{"action.created", "", T, 0, "", {{"id", "act_t"}}};
    EXPECT_EQ(apply_live_event(snap, live, created), Apply::refetch);
    LiveRunEvent resolved{"action.resolved", "", T, 0, "x", {{"id", "act_s"}, {"resolution", {{"kind", "answer"}, {"answer", "Connect"}}}}};
    EXPECT_EQ(apply_live_event(snap, live, resolved), Apply::ok);
    EXPECT_EQ(summary(build_feed(snap)), (V{"action:question:resolved"}));
}

TEST(ShellFeed, ModelAccessErrorIsTheLatestError) {
    Recorder ev{"run_3"};
    FeedSnapshot snap = snapshot({run("run_3", "running")});
    LiveText live;
    replay(snap, live,
           {ev("message", {{"role", "user"}, {"content", "go"}}),
            ev("error", {{"code", "QUOTA_EXCEEDED"}, {"message", "quota"}, {"reason", "quota"}, {"reset_at", "2026-11-01T00:00:00Z"}}),
            ev("run.state", {{"state", "failed"}, {"reason", "quota"}})});
    EXPECT_EQ(summary(build_feed(snap)), (V{"user:go", "error:QUOTA_EXCEEDED"}));
    const auto err = latest_error(snap);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->run_id, "run_3");
    EXPECT_EQ(err->error, (RunError{"QUOTA_EXCEEDED", "quota", "quota", std::nullopt, "2026-11-01T00:00:00Z"}));
}

TEST(ShellFeed, StoppedByTheUserAndUnknownRuns) {
    FeedSnapshot snap = snapshot({run("run_4", "running")});
    LiveText live;
    apply_live_event(snap, live, {"text.delta", "run_4", T, 0, "", {{"text", "half"}}});
    apply_live_event(snap, live, {"run.state", "run_4", T, 7, "", {{"state", "cancelled"}, {"reason", "cancelled by user"}}});
    EXPECT_EQ(summary(build_feed(snap, live)), (V{"stopped:true"}));
    EXPECT_FALSE(latest_error(snap));
    FeedSnapshot empty = snapshot({});
    EXPECT_EQ(apply_live_event(empty, live, {"run.state", "run_new", T, 1, "", {{"state", "queued"}}}), Apply::refetch);
}

TEST(ShellFeed, ActivityRailGroupsParallelSubagents) {
    RunInfo parent = run("run_p", "running");
    auto msg = [](long long seq, std::vector<std::string> ids) {
        json calls = json::array();
        for (auto& id : ids) calls.push_back({{"id", id}});
        return StoredEvent{seq, "", "message", {{"role", "assistant"}, {"content", ""}, {"tool_calls", calls}}};
    };
    auto sub = [](long long seq, const char* call, const char* child, const char* task) {
        return StoredEvent{seq, "", "subagent", {{"call_id", call}, {"child_run_id", child}, {"task", task}}};
    };
    parent.events = {msg(1, {"s1", "s2", "s3"}), sub(2, "s1", "c1", "A"), sub(3, "s2", "c2", "B"), sub(4, "s3", "c3", "C"),
                     msg(5, {"s4"}), sub(6, "s4", "c4", "D")};
    auto child = [](const char* id, const char* state, long long active_ms) {
        RunInfo r = run(id, state);
        r.parent_run_id = "run_p";
        r.active_ms = active_ms;
        r.started_at = "2026-10-23T10:00:00Z";
        return r;
    };
    FeedSnapshot snap = snapshot({parent});
    snap.subagents = {child("c1", "succeeded", 4000), child("c2", "running", 0), child("c3", "failed", 1500),
                      child("c4", "queued", 0)};
    const auto groups = build_rail(snap, *iso_millis("2026-10-23T10:00:10Z"));
    std::vector<V> got;
    for (const auto& g : groups) {
        V v;
        for (const auto& e : g.entries) v.push_back(e.task + ":" + e.state);
        got.push_back(v);
    }
    EXPECT_EQ(got, (std::vector<V>{{"A:succeeded", "B:running", "C:failed"}, {"D:queued"}}));
    EXPECT_EQ(groups[0].entries[0].duration_ms, 4000);
    EXPECT_EQ(groups[0].entries[1].duration_ms, 10000);

    FeedSnapshot s2 = snapshot({run("run_p")});
    RunInfo c1 = run("c1", "running");
    c1.parent_run_id = "run_p";
    s2.subagents = {c1};
    LiveText live;
    EXPECT_EQ(apply_live_event(s2, live, {"run.state", "c1", T, 3, "", {{"state", "succeeded"}, {"reason", ""}}}), Apply::ok);
    EXPECT_EQ(s2.subagents[0].state, "succeeded");
}

TEST(ShellFeed, SnapshotFromJson) {
    const json j = {{"entry", {{"id", T}, {"title", "Budget"}, {"pinned", true}, {"origin", {{"kind", "trigger"}}}}},
                    {"runs", {{{"id", "r1"}, {"state", "running"}, {"events", {{{"seq", 1}, {"kind", "message"},
                                                                                {"payload", {{"role", "user"}, {"content", "hi"}}}}}}}}},
                    {"actions", {{{"id", "a1"}, {"kind", "approval"}, {"run_id", "r1"}}}},
                    {"queued", {{{"id", 3}, {"text", "next"}, {"mode", "queue"}}}}};
    const FeedSnapshot s = snapshot_from(j);
    EXPECT_EQ(s.entry.title, "Budget");
    EXPECT_TRUE(s.entry.pinned);
    EXPECT_EQ(s.entry.origin_kind, "trigger");
    ASSERT_EQ(s.runs.size(), 1u);
    EXPECT_EQ(s.runs[0].events[0].payload["content"], "hi");
    EXPECT_EQ(s.actions[0].kind, "approval");
    EXPECT_EQ(s.queued[0].text, "next");
    EXPECT_TRUE(active_run(s));
    EXPECT_EQ(snapshot_from(json::array()).runs.size(), 0u);  // malformed: empty
}

TEST(ShellModel, IsoTimestamps) {
    EXPECT_EQ(iso_millis("1970-01-01T00:00:00Z"), 0);
    EXPECT_EQ(iso_millis("1970-01-01T00:00:01.5Z"), 1500);
    EXPECT_EQ(iso_millis("2026-10-23T12:00:00+02:00"), iso_millis("2026-10-23T10:00:00Z"));
    EXPECT_EQ(iso_from_millis(*iso_millis("2026-02-28T23:59:59.123Z")), "2026-02-28T23:59:59.123Z");
    EXPECT_EQ(iso_from_millis(*iso_millis("2024-02-29T00:00:00Z")), "2024-02-29T00:00:00.000Z");
    EXPECT_FALSE(iso_millis(""));
    EXPECT_FALSE(iso_millis("2026-13-01T00:00:00Z"));
    EXPECT_FALSE(iso_millis("2026-10-23T10:00:00Zjunk"));
}

// ---- Thread list (inbox-model.test.ts) ----

namespace {

ThreadEntry entry(const std::string& id, const std::string& last_activity = "2026-10-23T10:00:00.000Z") {
    ThreadEntry e;
    e.id = id;
    e.title = id;
    e.last_activity = last_activity;
    e.created_at = "2026-10-01T10:00:00.000Z";
    return e;
}

V ids(const std::vector<ThreadEntry>& list) {
    V v;
    for (const auto& e : list) v.push_back(e.id);
    return v;
}

}  // namespace

TEST(ShellInbox, FiledThreadsStayInSmartViews) {
    ThreadEntry filed = entry("a");
    filed.folder_id = "f1";
    filed.unread = filed.has_action = filed.running = true;
    for (const char* v : {"unread", "action", "running", "all", "folder:f1"}) EXPECT_TRUE(belongs_to(filed, v)) << v;
    EXPECT_FALSE(belongs_to(filed, "inbox"));
    EXPECT_FALSE(belongs_to(filed, "folder:f2"));
}

TEST(ShellInbox, ArchiveLeavesActionView) {
    ThreadEntry a = entry("a");
    a.archived = a.unread = a.has_action = true;
    a.folder_id = "f1";
    EXPECT_TRUE(belongs_to(a, "archive"));
    for (const char* v : {"all", "unread", "inbox", "folder:f1"}) EXPECT_FALSE(belongs_to(a, v)) << v;
    EXPECT_TRUE(belongs_to(a, "action"));
}

// add-chats-in-inbox D2: a Chat entry is in Unread, All, the Inbox, folders and the Archive, never in Action or Running.
TEST(ShellInbox, ChatEntriesInViews) {
    ThreadEntry c = entry("chat:c1");
    c.kind = "chat";
    c.unread = true;
    c.has_action = c.running = true;  // never set by the host for a Chat; still kept out of these views
    EXPECT_EQ(c.chat_id(), "c1");
    for (const char* v : {"unread", "all", "inbox"}) EXPECT_TRUE(belongs_to(c, v)) << v;
    for (const char* v : {"action", "running", "archive"}) EXPECT_FALSE(belongs_to(c, v)) << v;
    c.folder_id = "f1";
    EXPECT_TRUE(belongs_to(c, "folder:f1"));
    EXPECT_FALSE(belongs_to(c, "inbox"));
    c.archived = true;
    EXPECT_TRUE(belongs_to(c, "archive"));
    EXPECT_EQ(entry("t").chat_id(), "");
}

TEST(ShellInbox, OrderingAndLiveUpdates) {
    std::vector<ThreadEntry> list = {entry("old", "2026-10-20T00:00:00Z"), entry("new", "2026-10-23T00:00:00Z"),
                                     entry("pin", "2026-01-01T00:00:00Z")};
    list[2].pinned = true;
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return compare_entries(a, b) < 0; });
    EXPECT_EQ(ids(list), (V{"pin", "new", "old"}));

    std::vector<ThreadEntry> l = {entry("b", "2026-10-22T00:00:00Z"), entry("c", "2026-10-21T00:00:00Z")};
    apply_entry(l, entry("a", "2026-10-23T00:00:00Z"), "all");
    EXPECT_EQ(ids(l), (V{"a", "b", "c"}));
    apply_entry(l, entry("c", "2026-10-24T00:00:00Z"), "all");
    EXPECT_EQ(ids(l), (V{"c", "a", "b"}));
    ThreadEntry archived = entry("a");
    archived.archived = true;
    apply_entry(l, archived, "all");
    EXPECT_EQ(ids(l), (V{"c", "b"}));

    std::vector<ThreadEntry> page = {entry("a", "2026-10-23T00:00:00Z")};
    apply_entry(page, entry("z", "2020-01-01T00:00:00Z"), "all", true);  // older than the loaded page, more to come
    EXPECT_EQ(ids(page), (V{"a"}));
    apply_entry(page, entry("z", "2020-01-01T00:00:00Z"), "all");  // the whole view is loaded
    EXPECT_EQ(ids(page), (V{"a", "z"}));
}

TEST(ShellInbox, DateGroupsInLocalTime) {
    // Thursday 23 October 2026, 15:00 local time.
    const long long now = local_millis(2026, 10, 23, 15);
    auto at = [](int m, int d, int h, int mi) { return iso_from_millis(local_millis(2026, m, d, h, mi)); };
    EXPECT_EQ(date_group(false, at(10, 23, 0, 5), now), Group::today);
    EXPECT_EQ(date_group(false, at(10, 22, 23, 59), now), Group::yesterday);
    EXPECT_EQ(date_group(false, at(10, 20, 9, 0), now), Group::this_week);   // Tuesday
    EXPECT_EQ(date_group(false, at(10, 19, 0, 1), now), Group::this_week);   // Monday
    EXPECT_EQ(date_group(false, at(10, 18, 23, 0), now), Group::last_week);  // Sunday
    EXPECT_EQ(date_group(false, at(10, 12, 8, 0), now), Group::last_week);
    EXPECT_EQ(date_group(false, at(10, 3, 8, 0), now), Group::this_month);
    EXPECT_EQ(date_group(false, at(9, 30, 8, 0), now), Group::older);
    EXPECT_EQ(date_group(true, "2020-01-01T00:00:00Z", now), Group::pinned);
    EXPECT_EQ(date_group(false, "garbage", now), Group::older);
}

TEST(ShellInbox, ListRowsWithPinnedAndCollapsedGroups) {
    const long long now = local_millis(2026, 10, 23, 15);
    std::vector<ThreadEntry> entries = {entry("p", "2020-01-01T00:00:00Z"), entry("t", iso_from_millis(local_millis(2026, 10, 23, 9)))};
    entries[0].pinned = true;
    V got;
    for (const auto& r : list_rows(entries, now, {}))
        got.push_back(r.kind == ListRow::header ? std::string("[") + group_key(r.group) + "]" : r.item->id);
    EXPECT_EQ(got, (V{"[pinned]", "p", "[today]", "t"}));

    EXPECT_EQ(collapsed_by_default(), (std::set<Group>{Group::last_week, Group::this_month, Group::older}));
    const auto rows = list_rows({entry("o", "2020-01-01T00:00:00Z")}, now, collapsed_by_default());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, ListRow::header);
    EXPECT_EQ(rows[0].group, Group::older);
    EXPECT_EQ(rows[0].count, 1u);
    EXPECT_TRUE(rows[0].collapsed);
}

TEST(ShellInbox, TenThousandThreadsGroupQuickly) {
    const long long now = local_millis(2026, 10, 23, 15);
    std::vector<ThreadEntry> entries;
    for (int i = 0; i < 10000; ++i) entries.push_back(entry("t" + std::to_string(i), iso_from_millis(now - i * 1000LL)));
    const auto started = std::chrono::steady_clock::now();
    const auto rows = list_rows(entries, now, {});
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
    EXPECT_EQ(rows.size(), 10001u);  // all today
}

// ---- routes, shortcuts, status, formatting ----

TEST(ShellRoutes, ParsesAndCanonicalises) {
    EXPECT_EQ(parse_route("#/inbox")->view, "inbox");
    EXPECT_EQ(parse_route("inbox/unread/")->hash, "#/inbox/unread");
    EXPECT_EQ(parse_route("/inbox/folder/f_1")->view, "folder:f_1");
    EXPECT_FALSE(parse_route("#/inbox/bogus"));
    EXPECT_FALSE(parse_route("#/inbox/folder/bad id"));
    EXPECT_EQ(parse_route("#/threads/new")->name, RouteName::new_thread);
    const auto t = parse_route("#/threads/thr_9?msg=run_2:14");
    ASSERT_TRUE(t);
    EXPECT_EQ(t->name, RouteName::thread);
    EXPECT_EQ(t->thread_id, "thr_9");
    EXPECT_EQ(t->msg, (MessageRef{"run_2", 14}));
    EXPECT_EQ(t->hash, "#/threads/thr_9?msg=run_2:14");
    EXPECT_FALSE(parse_route("#/threads/thr_9?msg=bad")->msg);
    EXPECT_EQ(parse_route(" about ")->name, RouteName::about);
    EXPECT_EQ(parse_route("#/settings")->name, RouteName::settings);
    EXPECT_EQ(parse_route("#/settings/telemetry")->name, RouteName::telemetry);
    EXPECT_EQ(parse_route("/settings/telemetry/")->hash, "#/settings/telemetry");
    EXPECT_FALSE(parse_route("#/settings/other"));
    EXPECT_EQ(parse_route("#/status")->hash, "#/status");
    EXPECT_FALSE(parse_route(""));
    EXPECT_FALSE(parse_route("#/nowhere"));
    EXPECT_FALSE(parse_route("#/threads/a/b"));
    // No Chats-only view (fold-chats-into-inbox D1): an old #/chats link opens the Inbox; a Chat opens in the reading
    // pane (add-chats-in-inbox D3).
    const auto chats = parse_route("#/chats");
    ASSERT_TRUE(chats);
    EXPECT_EQ(chats->name, RouteName::inbox);
    EXPECT_EQ(chats->view, "inbox");
    EXPECT_EQ(chats->hash, "#/inbox");
    const auto chat = parse_route("/chats/c5471342-f776-4692-b670-3f020d811eb1/");
    ASSERT_TRUE(chat);
    EXPECT_EQ(chat->name, RouteName::chat);
    EXPECT_EQ(chat->thread_id, "c5471342-f776-4692-b670-3f020d811eb1");
    EXPECT_EQ(chat->hash, "#/chats/c5471342-f776-4692-b670-3f020d811eb1");
    EXPECT_FALSE(parse_route("#/chats/bad id"));
    EXPECT_FALSE(parse_route("#/chats/a/b"));
}

TEST(ShellShortcuts, Catalogue) {
    using ui::KeyEvent;
    const KeyContext none, typing{true, false, false}, list{false, true, false}, open{false, false, true};
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyN, false, true, false}, typing), Command::new_thread);
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyComma, false, true, false}, none), Command::settings);
    EXPECT_EQ(command_for(KeyEvent{ui::kKeySlash, false, true, false}, none), Command::help);
    EXPECT_FALSE(command_for(KeyEvent{ui::kKeyN, false, true, true}, none));  // AltGr
    EXPECT_FALSE(command_for(KeyEvent{ui::kKeyE, false, false, false}, typing));
    EXPECT_FALSE(command_for(KeyEvent{ui::kKeyE, false, false, false}, none));
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyE, false, false, false}, list), Command::archive);
    EXPECT_FALSE(command_for(KeyEvent{ui::kKeyE, true, false, false}, list));
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyU, false, false, false}, open), Command::mark_unread);
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyDelete, false, false, false}, open), Command::remove);
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyDown, false, false, false}, list), Command::next);
    EXPECT_FALSE(command_for(KeyEvent{ui::kKeyDown, false, false, false}, open));
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyEnter, false, false, false}, list), Command::open);
    EXPECT_EQ(command_for(KeyEvent{ui::kKeyEscape, false, false, false}, open), Command::back);
    EXPECT_FALSE(command_for(KeyEvent{ui::kKeyEscape, false, false, false}, typing));
    const ui::Strings en = ui::load_bundle("en");
    for (const auto& c : catalogue()) EXPECT_TRUE(en.has(c.label_key)) << c.label_key;
}

TEST(ShellDerive, StatusDerivations) {
    json s = {{"version", json::object()},
              {"auth", {{"state", "signed_in"}}},
              {"control_plane", {{"configured", true}, {"connected", true}}},
              {"background", {{"battery_saver", true}}},
              {"storage", {{"read_only", true}, {"degraded", true}}},
              {"mcp",
               {{{"name", "A"}, {"state", "connected"}, {"usable_tools", 3}},
                {{"name", "B"}, {"state", "failed"}, {"usable_tools", 2}},
                {{"name", "C"}, {"state", "idle"}, {"usable_tools", 1}, {"warning", "no usable tools: x"}}}}};
    EXPECT_TRUE(is_app_status(s));
    EXPECT_EQ(connection_kind(s), "connected");
    EXPECT_EQ(banners(s), (std::vector<BannerSpec>{{"storage_degraded", "warning"}}));
    EXPECT_EQ(deferral_reasons(s), (V{"battery_saver"}));
    const McpHealth h = mcp_health(s);
    EXPECT_EQ(h.total, 3u);
    EXPECT_EQ(h.healthy, 1u);
    EXPECT_EQ(h.usable_tools, 4);
    EXPECT_EQ(h.tone, "bad");
    s["control_plane"]["locked"] = true;
    s["auth"]["state"] = "sign_in_required";
    EXPECT_EQ(connection_kind(s), "locked");
    EXPECT_EQ(banners(s)[0], (BannerSpec{"locked", "error"}));
    EXPECT_EQ(banners(s)[1], (BannerSpec{"sign_in_required", "warning"}));
    s["control_plane"]["configured"] = false;
    EXPECT_EQ(connection_kind(s), "not_configured");
    EXPECT_FALSE(is_app_status(json::object()));
    EXPECT_EQ(mcp_health(json::object()).tone, "");
}

TEST(ShellDerive, PendingRemoteRemovalIsVisibleOnFirstRun) {
    json status = {{"storage", {{"pending_remote_removals", 2}}},
                   {"control_plane", {{"configured", true}}}, {"auth", {{"state", "sign_in_required"}}}};
    const auto notices = banners(status);
    EXPECT_NE(std::find(notices.begin(), notices.end(), BannerSpec{"remote_removal_pending", "warning"}), notices.end());
    status["storage"]["pending_remote_removals"] = 0;
    const auto cleared = banners(status);
    EXPECT_EQ(std::find(cleared.begin(), cleared.end(), BannerSpec{"remote_removal_pending", "warning"}), cleared.end());
}

TEST(ShellFormat, PercentSnippetsAndAboutText) {
    EXPECT_EQ(format_percent(0.42), "42%");
    EXPECT_EQ(format_percent(0.045, ','), "4,5%");
    EXPECT_EQ(format_percent(0), "0%");
    EXPECT_EQ(format_percent(1.5), "150%");
    using P = std::vector<SnippetPart>;
    EXPECT_EQ(snippet_parts("the [[budget]] for [[Q3]]"), (P{{"the ", false}, {"budget", true}, {" for ", false}, {"Q3", true}}));
    EXPECT_EQ(snippet_parts("[[x]]"), (P{{"x", true}}));
    EXPECT_EQ(snippet_parts("no marks [[open"), (P{{"no marks [[open", false}}));
    EXPECT_TRUE(snippet_parts("").empty());
    EXPECT_EQ(about_text({{"version", "1.2.3"}, {"channel", "stable"}, {"commit", "abc"}, {"pid", 42}}),
              "SkoWork 1.2.3\nChannel: stable\nCommit: abc\nProcess ID: 42");
}

TEST(ShellFormat, SizesDurationsAndRelativeTimes) {
    EXPECT_EQ(format_bytes(1536), "1.5 KB");
    EXPECT_EQ(format_bytes(1536, ','), "1,5 KB");
    EXPECT_EQ(format_bytes(512), "512 B");
    EXPECT_EQ(format_bytes(200.0 * 1024 * 1024), "200 MB");
    EXPECT_EQ(format_duration(45), "45 s");
    EXPECT_EQ(format_duration(61), "2 min");
    EXPECT_EQ(format_duration(3900), "1 h 5 min");
    EXPECT_EQ(format_elapsed(185000), "3 min 5 s");
    EXPECT_EQ(format_elapsed(3720000), "1 h 2 min");
    EXPECT_EQ(short_commit("0123456789abcdef"), "01234567");
    EXPECT_EQ(display_user({{"name", "Jana"}, {"email", "jana@x.cz"}}), "Jana (jana@x.cz)");
    EXPECT_EQ(display_user(" bob "), "bob");
    const auto segs = usage_segments({{"a", 10}, {"b", 30}, {"c", 0}}, 100, 6);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].name, "b");
    EXPECT_DOUBLE_EQ(segs[0].fraction, 0.3);

    const ui::Strings en = ui::load_bundle("en"), cs = ui::load_bundle("cs");
    const long long now = *iso_millis("2026-10-23T15:00:00Z");
    auto date = [](long long) { return std::u16string(u"DATE"); };
    EXPECT_EQ(format_relative("2026-10-23T14:59:30Z", now, en, date), u"now");
    EXPECT_EQ(format_relative("2026-10-23T14:55:00Z", now, en, date), u"5 min ago");
    EXPECT_EQ(format_relative("2026-10-22T15:00:00Z", now, en, date), u"yesterday");
    EXPECT_EQ(format_relative("2026-10-20T15:00:00Z", now, cs, date), u"před 3 dny");
    EXPECT_EQ(format_relative("2026-09-01T15:00:00Z", now, en, date), u"DATE");
    EXPECT_EQ(format_relative("bad", now, en, date), u"");
}

TEST(Routes, AutomationPages) {
    const auto page = parse_route("#/automations/trg_A-1");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->name, RouteName::automation);
    EXPECT_EQ(page->trigger, "trg_A-1");
    EXPECT_EQ(page->hash, "#/automations/trg_A-1");
    EXPECT_TRUE(is_page(page->name));
    const auto fresh = parse_route("automations/new");
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->name, RouteName::automation_editor);
    EXPECT_TRUE(fresh->trigger.empty());
    const auto edit = parse_route("#/automations/edit/trg_A-1/");
    ASSERT_TRUE(edit);
    EXPECT_EQ(edit->name, RouteName::automation_editor);
    EXPECT_EQ(edit->hash, "#/automations/edit/trg_A-1");
    EXPECT_FALSE(parse_route("#/automations"));
    EXPECT_FALSE(parse_route("#/automations/bad id"));
}

// ==== fix-feed-live-updates: Live Feed Updates (design D2) and Cited Sources In The Feed (design D7) ==================

namespace {
std::vector<std::string> answers(const std::vector<FeedItem>& items) {
    std::vector<std::string> out;
    for (const FeedItem& i : items)
        if (i.type == ItemType::assistant) out.push_back(i.text);
    return out;
}

// A knowledge.search result body with passages n = 1..count, each from its own file.
std::string search_body(int count) {
    json results = json::array();
    for (int n = 1; n <= count; ++n)
        results.push_back({{"n", n}, {"source", "files"}, {"title", "Doc " + std::to_string(n)}, {"ref", "files://doc" + std::to_string(n)},
                           {"date", "2026-10-01T00:00:00Z"}});
    return json({{"results", results}}).dump();
}
}  // namespace

TEST(ShellFeedLive, StoredAnswerAfterAReasoningGapShowsOnce) {
    Recorder ev{"run_1"};
    FeedSnapshot snap = snapshot({run("run_1", "running")});
    LiveText live;
    EXPECT_EQ(apply_live_event(snap, live, ev("message", {{"role", "user"}, {"content", "hi"}})), Apply::ok);  // seq 1
    EXPECT_EQ(apply_live_event(snap, live, ev("text.delta", {{"text", "Hel"}}, false)), Apply::ok);
    EXPECT_EQ(apply_live_event(snap, live, ev("text.delta", {{"text", "lo"}}, false)), Apply::ok);
    ev("reasoning", {{"text", "x"}});  // stored as seq 2, but this older feed never received it
    const LiveRunEvent answer = ev("message", {{"role", "assistant"}, {"content", "Hello"}});  // seq 3
    EXPECT_EQ(apply_live_event(snap, live, answer), Apply::refetch);
    // The refetched snapshot holds the reasoning and the answer; the streamed copy is gone.
    FeedSnapshot reloaded = snapshot({run("run_1", "running")});
    reloaded.runs[0].events = {{1, "", "message", {{"role", "user"}, {"content", "hi"}}},
                               {2, "", "reasoning", {{"text", "x"}}},
                               {3, "", "message", {{"role", "assistant"}, {"content", "Hello"}}}};
    settle_live(reloaded, live);
    EXPECT_EQ(summary(build_feed(reloaded, live)), (V{"user:hi", "reasoning:x", "assistant:Hello", "working"}));
    EXPECT_EQ(answers(build_feed(reloaded, live)), (std::vector<std::string>{"Hello"}));
}

TEST(ShellFeedLive, SnapshotTakenAfterStreamingShowsTheAnswerOnce) {
    Recorder ev{"run_1"};
    FeedSnapshot snap = snapshot({run("run_1", "running")});
    LiveText live;
    apply_live_event(snap, live, ev("message", {{"role", "user"}, {"content", "hi"}}));  // seq 1
    apply_live_event(snap, live, ev("text.delta", {{"text", "Hello"}}, false));
    // A snapshot loaded while the text streamed already holds the stored answer (seq 2).
    FeedSnapshot loaded = snapshot({run("run_1", "running")});
    loaded.runs[0].events = {{1, "", "message", {{"role", "user"}, {"content", "hi"}}},
                             {2, "", "message", {{"role", "assistant"}, {"content", "Hello"}}}};
    EXPECT_EQ(summary(build_feed(loaded, live)), (V{"user:hi", "assistant:Hello", "working"}));
    settle_live(loaded, live);
    EXPECT_TRUE(live.text.empty());
    EXPECT_EQ(summary(build_feed(loaded, live)), (V{"user:hi", "assistant:Hello", "working"}));
}

TEST(ShellFeedLive, AnEarlierAnswerDoesNotRemoveTextThatStreamsLater) {
    Recorder ev{"run_1"};
    FeedSnapshot snap = snapshot({run("run_1", "running")});
    LiveText live;
    apply_live_event(snap, live, ev("message", {{"role", "user"}, {"content", "hi"}}));  // seq 1
    apply_live_event(snap, live, ev("text.delta", {{"text", "Let me look."}}, false));
    apply_live_event(snap, live, ev("message", {{"role", "assistant"}, {"content", "Let me look."}}));  // seq 2
    apply_live_event(snap, live, ev("text.delta", {{"text", "Done"}}, false));  // the next model call streams
    EXPECT_EQ(summary(build_feed(snap, live)), (V{"user:hi", "assistant:Let me look.", "assistant*:Done"}));
    // The same after a snapshot that holds the earlier answer.
    FeedSnapshot loaded = snapshot({run("run_1", "running")});
    loaded.runs[0].events = {{1, "", "message", {{"role", "user"}, {"content", "hi"}}},
                             {2, "", "message", {{"role", "assistant"}, {"content", "Let me look."}}}};
    settle_live(loaded, live);
    EXPECT_EQ(summary(build_feed(loaded, live)), (V{"user:hi", "assistant:Let me look.", "assistant*:Done"}));
}

TEST(ShellFeedSources, OnlyPassagesTheAnswerCitesAreListedInNumberOrder) {
    Recorder ev{"run_1"};
    FeedSnapshot snap = snapshot({run("run_1", "running")});
    LiveText live;
    replay(snap, live,
           {ev("message", {{"role", "user"}, {"content", "find"}}),
            ev("tool_call", {{"call_id", "c1"}, {"tool", "knowledge.search"}, {"args", {{"query", "doc"}}}, {"rationale", ""},
                             {"risk", "read"}, {"decision", "allow"}, {"reason", ""}}),
            ev("tool_result", {{"call_id", "c1"}, {"tool", "knowledge.search"}, {"ok", true}, {"content", search_body(8)}}),
            ev("message", {{"role", "assistant"}, {"content", "The plan is in Doc 5 [5] and the budget in Doc 2 [2]."}})});
    const auto items = build_feed(snap, live);
    const FeedItem* answer = nullptr;
    for (const FeedItem& i : items)
        if (i.type == ItemType::assistant) answer = &i;
    ASSERT_NE(answer, nullptr);
    ASSERT_EQ(answer->sources.size(), 2u);
    EXPECT_EQ(answer->sources[0].n, 2);
    EXPECT_EQ(answer->sources[0].title, "Doc 2");
    EXPECT_EQ(answer->sources[1].n, 5);
    EXPECT_EQ(answer->sources[1].ref, "files://doc5");
}

TEST(ShellFeedSources, AnswersWithoutUsableMarkersListNothing) {
    std::map<long long, Citation> cited;
    for (long long n = 1; n <= 8; ++n) cited[n] = {n, "Doc", "files://d", "files", ""};
    EXPECT_TRUE(cited_in_text("No citations in this answer.", cited).empty());
    EXPECT_TRUE(cited_in_text("Unknown passage [9] and [0].", cited).size() == 0u);
    EXPECT_TRUE(cited_in_text("A code span `[2]` is not a citation.", cited).empty());
    EXPECT_TRUE(cited_in_text("``[2] inside double ticks``", cited).empty());
    EXPECT_TRUE(cited_in_text("```\n[3]\n```\nAfter the block.", cited).empty());
    const auto marked = cited_in_text("Sentence [2]. Another [2] and [7]. Code `[4]` and [4].", cited);
    ASSERT_EQ(marked.size(), 3u);
    EXPECT_EQ(marked[0].n, 2);
    EXPECT_EQ(marked[1].n, 4);
    EXPECT_EQ(marked[2].n, 7);
}

// Live Feed Updates: a stream of 2,000 deltas in 20 seconds repaints at most 30 times in any second, and the stored events
// are folded once (design D3, D4).
TEST(ShellFeedLive, StreamRepaintsAtMostThirtyTimesAndNeverRefoldsStoredEvents) {
    FeedSnapshot snap = snapshot({run("run_1", "running")});
    for (long long seq = 1; seq <= 50; ++seq)
        snap.runs[0].events.push_back({seq, "", "message", {{"role", "user"}, {"content", "message " + std::to_string(seq)}}});
    Recorder ev{"run_1"};
    LiveText live;
    FeedFold fold;
    fold.build(snap, live);
    EXPECT_EQ(fold.folded_events(), 50u);
    RepaintLimit limit;
    std::vector<long long> repaints;
    long long t = 0;
    for (int i = 0; i < 2000; ++i, t += 10) {
        apply_live_event(snap, live, ev("text.delta", {{"text", "w "}}, false));
        if (limit.request(t) || limit.take_due(t)) {
            repaints.push_back(t);
            const auto items = fold.build(snap, live);
            ASSERT_FALSE(items.empty());
        }
    }
    EXPECT_EQ(fold.folded_events(), 50u);  // the stored events were not folded again
    ASSERT_FALSE(repaints.empty());
    EXPECT_LE(repaints.size(), 600u);
    for (const long long start : repaints) {
        size_t in_second = 0;
        for (const long long other : repaints)
            if (other >= start && other < start + 1000) ++in_second;
        EXPECT_LE(in_second, 30u) << "repaints from " << start << " ms";
    }
    // The text reached the end of the stream: 2,000 words of "w ".
    ASSERT_EQ(live.text["run_1"].size(), 4000u);
}

TEST(ShellFeedLive, RepaintLimitWaitsForTheIntervalAndTakesTheDueRepaint) {
    RepaintLimit limit(34);
    EXPECT_TRUE(limit.request(1000));   // first change: at once
    EXPECT_FALSE(limit.request(1010));  // inside the interval: pending
    EXPECT_TRUE(limit.pending());
    EXPECT_EQ(limit.due_at(), 1034);
    EXPECT_FALSE(limit.take_due(1030));
    EXPECT_TRUE(limit.take_due(1034));
    EXPECT_FALSE(limit.pending());
    EXPECT_FALSE(limit.take_due(1100));  // nothing pending
    EXPECT_TRUE(limit.request(1100));
}

// A repaint waiting for its interval has one timer. When the timer fires before the due time (the window clock and the
// wall clock differ), the repaint stays pending and the timer is armed again; the repaint then runs at its due time.
// Without the re-arm the last delta of a stream was never repainted (ShellFeed.StreamingDeltasKeepEarlierItems).
TEST(ShellFeedLive, AnEarlyRepaintTimerRearmsForThePendingRepaint) {
    long long clock = 0;
    RepaintLimit limit(34);
    std::map<uintptr_t, std::pair<long long, std::function<void()>>> timers;  // id -> (due, closure)
    uintptr_t next = 1;
    int repaints = 0;
    RepaintTimer timer(
        limit,
        [&](unsigned ms, std::function<void()> fn) {
            timers[next] = {clock + ms, std::move(fn)};
            return next++;
        },
        [&](uintptr_t id) { timers.erase(id); }, [&] { return clock; }, [&] { ++repaints; });

    timer.streamed_change();  // the first change repaints at once
    EXPECT_EQ(repaints, 1);
    EXPECT_TRUE(timers.empty());
    clock = 10;
    timer.streamed_change();  // inside the interval: one timer for the due time 34
    EXPECT_EQ(repaints, 1);
    ASSERT_EQ(timers.size(), 1u);
    EXPECT_EQ(timers.begin()->second.first, 34);
    timer.streamed_change();  // a further delta does not add a second timer
    EXPECT_EQ(timers.size(), 1u);

    // The timer fires 1 ms early: nothing is repainted, and the timer is armed for the rest of the interval.
    clock = 33;
    auto early = timers.begin()->second.second;
    timers.clear();
    early();
    EXPECT_EQ(repaints, 1);
    ASSERT_EQ(timers.size(), 1u);
    EXPECT_EQ(timers.begin()->second.first, 34);

    clock = 34;  // the due time: the pending repaint runs
    auto due = timers.begin()->second.second;
    timers.clear();
    due();
    EXPECT_EQ(repaints, 2);
    EXPECT_TRUE(timers.empty());
    EXPECT_FALSE(limit.pending());

    // A stored change repaints at once and takes the waiting timer with it.
    clock = 40;
    timer.streamed_change();
    ASSERT_EQ(timers.size(), 1u);
    timer.stored_change();
    EXPECT_TRUE(timers.empty());
    EXPECT_EQ(repaints, 3);
}

// A stored event of a later model call does not change the fold of the stored events before it (fold cache, design D4).
TEST(ShellFeedLive, FoldCacheFoldsOnlyAppendedEvents) {
    FeedSnapshot snap = snapshot({run("run_1", "running")});
    snap.runs[0].events.push_back({1, "", "message", {{"role", "user"}, {"content", "one"}}});
    FeedFold fold;
    EXPECT_EQ(summary(fold.build(snap, {})), (V{"user:one", "working"}));
    snap.runs[0].events.push_back({2, "", "message", {{"role", "user"}, {"content", "two"}}});
    EXPECT_EQ(summary(fold.build(snap, {})), (V{"user:one", "user:two", "working"}));
    EXPECT_EQ(fold.folded_events(), 2u);
    // A rebuilt log (not a prefix of the cached one) is folded again.
    snap.runs[0].events = {{7, "", "message", {{"role", "user"}, {"content", "other"}}}};
    EXPECT_EQ(summary(fold.build(snap, {})), (V{"user:other", "working"}));
}

// add-agent-created-skills 2.8: the saved Skill's entry carries the link to its Publish dialog; a refused save has none.
TEST(ShellFeed, SavedSkillEntryLinksToItsPublishDialogAndARefusalHasNoLink) {
    Recorder ev{"run_3"};
    FeedSnapshot snap = snapshot({run("run_3", "running")});
    LiveText live;
    replay(snap, live,
           {ev("tool_call", {{"call_id", "s1"}, {"tool", "skills.save"}, {"risk", "write-local"}, {"decision", "allow"},
                             {"args", {{"name", "weekly-report"}, {"description", "d"}, {"instructions", "i"}}}}),
            ev("tool_result", {{"call_id", "s1"}, {"tool", "skills.save"}, {"ok", true}, {"content", "Saved the Skill"}}),
            ev("tool_call", {{"call_id", "s2"}, {"tool", "skills.save"}, {"risk", "write-local"}, {"decision", "allow"},
                             {"args", {{"name", "Bad Name"}, {"description", "d"}, {"instructions", "i"}}}}),
            ev("tool_result", {{"call_id", "s2"}, {"tool", "skills.save"}, {"ok", false}, {"content", "refused"}})});
    const auto items = build_feed(snap, live);
    std::vector<const FeedItem*> tools;
    for (const FeedItem& i : items)
        if (i.type == ItemType::tool) tools.push_back(&i);
    ASSERT_EQ(tools.size(), 2u);
    ASSERT_TRUE(tools[0]->result.has_value());
    EXPECT_EQ(tools[0]->result->publish_route, "#/skills/edit/weekly-report");
    ASSERT_TRUE(tools[1]->result.has_value());
    EXPECT_TRUE(tools[1]->result->publish_route.empty());
}
