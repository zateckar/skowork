// The Thread feed view model (client/work-inbox design D13), ported from the web UI's feed.ts: one fold over Run
// events, used for the stored log that threads.feed returns and for the live run.event messages that follow it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "shell/model.h"

namespace wb::shell {

// A file a tool stored for the user (a generated image), as the tool result lists it.
struct FileRef {
    std::string id, name, mime;
    long long size = 0;
    int width = 0, height = 0;
    bool image() const { return mime.starts_with("image/"); }
};

// A passage knowledge.search returned (client/knowledge-graph Search Result Citation): what the UI opens.
struct Citation {
    long long n = 0;
    std::string title, ref, source, date;
};

struct ToolResult {
    bool ok = false;
    std::string content;
    std::vector<FileRef> files;
    std::vector<Citation> citations;
    // skills.save: the Skill editor route whose Publish dialog shares the saved Skill (add-agent-created-skills 2.8).
    std::string publish_route;
};

// The Publish dialog route of a saved Skill: "#/skills/edit/<name>" for a successful skills.save call, else empty.
std::string publish_route_of(const std::string& tool, const json& args, bool ok);

// The citations of a knowledge.search result's content; empty for anything else.
std::vector<Citation> citations_of(const std::string& tool, const std::string& content);

struct RunError {
    std::string code, message, reason;
    std::optional<long long> retry_after_s;
    std::string reset_at;
    bool operator==(const RunError&) const = default;
};

// details: Show details / Hide details after a finished Run whose work is hidden (Work Details Visibility).
enum class ItemType { user, assistant, reasoning, tool, activity, action, error, stopped, working, details };

struct FeedItem {
    ItemType type = ItemType::activity;
    std::string key, run_id;
    long long seq = 0;
    std::string at;
    std::string text;  // user, assistant, reasoning, activity
    std::optional<RunUsage> usage;  // activity: the Run's token totals, after it ended
    bool streaming = false;
    // assistant: the passages the answer cites with [n], in number order (Cited Sources In The Feed)
    std::vector<Citation> sources;
    // tool
    std::string call_id, tool, risk, decision, reason, rationale;
    json args = json::object();
    std::optional<ToolResult> result;
    bool edited = false;
    bool compact = false;  // tool: only the images and citations it produced (work details hidden)
    // action
    ActionItem action;
    // error
    RunError error;
    // stopped
    bool by_user = false;
};

// Text that is streaming in, per Run, until the stored message replaces it. text_after and reasoning_after hold, per
// Run, the sequence number of the Run's last stored event when its first streamed delta of the current model call
// arrived (Live Feed Updates): a stored answer with a higher number is the answer that text streamed into.
struct LiveText {
    std::map<std::string, std::string> text, reasoning;
    std::map<std::string, long long> text_after, reasoning_after;
    // The highest sequence number of each Run that arrived live. A snapshot can be older than these events, so a delta
    // takes the later of this and the snapshot's last number as its recorded position.
    std::map<std::string, long long> seen;
};

bool is_active_state(const std::string& state);  // queued or running

// The last top-level Run that is queued, running or interrupted / queued or running.
const RunInfo* active_run(const FeedSnapshot& snap);
const RunInfo* running_run(const FeedSnapshot& snap);

RunError error_of(const json& payload);

struct LatestError {
    std::string run_id;
    RunError error;
};
// The error of the last top-level Run when it failed (Error Banners With Remedy).
std::optional<LatestError> latest_error(const FeedSnapshot& snap);

struct FoldedRun;

// The fold of a snapshot with its cache (Live Feed Updates, design D4): each Run's stored events are folded once, and a
// later build folds only the events that were appended since. A streamed delta therefore refolds only the streaming item.
class FeedFold {
public:
    FeedFold();
    ~FeedFold();
    std::vector<FeedItem> build(const FeedSnapshot& snap, const LiveText& live);
    // Stored events folded so far, over all builds (tests check that a stream does not refold them).
    [[nodiscard]] size_t folded_events() const { return folded_; }

private:
    std::map<std::string, FoldedRun> runs_;
    std::string actions_;  // the standalone and Run Action items the cache was folded with
    size_t folded_ = 0;
};

// One fold of the snapshot; the Thread feed keeps a FeedFold instead.
std::vector<FeedItem> build_feed(const FeedSnapshot& snap, const LiveText& live = {});

// Limits repaints of the feed while text streams to one per interval (Live Feed Updates: at most 30 a second).
// request() returns true when a repaint may run now; otherwise the repaint is pending until take_due() reports it due.
class RepaintLimit {
public:
    explicit RepaintLimit(long long interval_ms = 34);
    bool request(long long now_ms);
    bool take_due(long long now_ms);
    [[nodiscard]] bool pending() const { return pending_; }
    [[nodiscard]] long long due_at() const { return due_; }

private:
    long long interval_;
    long long last_ = -1;
    long long due_ = 0;
    bool pending_ = false;
};

// Repaints the feed for a limited stream (Live Feed Updates, D3): a streamed change repaints now when the limit allows, and
// otherwise one window timer waits for the due time. A timer can fire before the due time (the window clock and the
// wall clock differ by a tick), and then the repaint is still pending: the timer is armed again for the rest of the
// interval, so a delta that arrived last is never left without its repaint.
class RepaintTimer {
public:
    using SetTimer = std::function<uintptr_t(unsigned ms, std::function<void()> fn)>;
    using CancelTimer = std::function<void(uintptr_t id)>;
    RepaintTimer(RepaintLimit& limit, SetTimer set, CancelTimer cancel, std::function<long long()> now,
                 std::function<void()> repaint);
    // A streamed change: repaints now, or arms the timer for the pending repaint.
    void streamed_change();
    // A stored change repaints at once and covers the pending repaint, so the timer goes.
    void stored_change();
    [[nodiscard]] bool armed() const { return id_ != 0; }

private:
    void arm();
    void fire();

    RepaintLimit& limit_;
    SetTimer set_;
    CancelTimer cancel_;
    std::function<long long()> now_;
    std::function<void()> repaint_;
    uintptr_t id_ = 0;
};

// Drops the streamed text and reasoning that a stored answer of the snapshot replaces (Live Feed Updates). Called when a
// snapshot is loaded, so the text of a later model call starts from a fresh entry.
void settle_live(const FeedSnapshot& snap, LiveText& live);

// The passages a text cites with [n] in its own text, outside code spans and fenced code, in number order. Numbers
// that are not in `cited` are ignored.
std::vector<Citation> cited_in_text(const std::string& text, const std::map<long long, Citation>& cited);

enum class Apply { ok, refetch };
// Applies one live event to the snapshot and the streaming text; refetch when the snapshot does not hold what the
// event concerns yet (a new Run, a new subagent, a new standalone Action item).
Apply apply_live_event(FeedSnapshot& snap, LiveText& live, const LiveRunEvent& ev);

// ---- Activity Rail ----
struct RailEntry {
    std::string run_id, task, state;
    long long duration_ms = 0;
    std::string skill;  // the Skill the subagent runs, when it is a Skill
};
struct RailGroup {
    std::string key;
    std::vector<RailEntry> entries;
};
// Subagent Runs grouped by the assistant message whose tool calls started them, in start order.
std::vector<RailGroup> build_rail(const FeedSnapshot& snap, long long now_ms);

// The current time as milliseconds since the epoch and as ISO-8601 UTC.
long long now_millis();
std::string iso_from_millis(long long ms);

}  // namespace wb::shell
