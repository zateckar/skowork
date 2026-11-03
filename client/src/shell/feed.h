// The Thread feed view model (client/work-inbox design D13), ported from the web UI's feed.ts: one fold over Run
// events, used for the stored log that threads.feed returns and for the live run.event messages that follow it.
#pragma once

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
};

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

// Text that is streaming in, per Run, until the stored message replaces it.
struct LiveText {
    std::map<std::string, std::string> text, reasoning;
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

std::vector<FeedItem> build_feed(const FeedSnapshot& snap, const LiveText& live = {});

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
