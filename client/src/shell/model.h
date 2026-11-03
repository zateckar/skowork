// The window's view of the host's inbox data (client/work-inbox; add-native-ui design D5): the JSON the in-process
// RPC methods return (inbox.list, threads.feed, run.event, inbox.changed), read into plain structs. Portable, so the
// view models are unit-tested on every platform. Unknown or malformed fields read as empty values.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

namespace wb::shell {

// Milliseconds since the Unix epoch of an ISO-8601 UTC timestamp ("2026-10-23T10:00:00.123Z", an offset, or no
// fraction); nullopt when it cannot be read.
std::optional<long long> iso_millis(std::string_view iso);

struct ThreadEntry {
    std::string id, title, preview, last_activity, created_at;
    std::string origin_kind = "user";  // user | trigger | remote | meeting | fork | system
    json origin_ref;
    bool unread = false, archived = false, pinned = false;
    std::string folder_id;  // empty: the Inbox
    bool has_action = false, running = false;
    long long open_actions = 0;
    std::string last_run_state;
    // A Chat's Inbox entry (client/work-inbox Chat Entries): id "chat:<chat id>".
    std::string kind = "thread";  // thread | chat
    long long unread_count = 0;   // unread messages
    std::string chat_kind;        // direct | team
    std::string presence;         // the other user's Presence of a direct Chat
    bool is_chat() const { return kind == "chat"; }
    std::string chat_id() const { return is_chat() && id.size() > 5 ? id.substr(5) : std::string(); }
};
ThreadEntry entry_from(const json& j);
// The string key of a Thread's origin badge: list.origin.<kind>, and list.origin.skill_test for a system Thread that
// runs a Skill test (client/skills Skill Test Run).
inline std::string origin_badge_key(const ThreadEntry& e) {
    if (e.origin_kind == "system" && e.origin_ref.is_object() && e.origin_ref.contains("skill_test")) return "list.origin.skill_test";
    return "list.origin." + e.origin_kind;
}

struct Counts {
    long long unread = 0, action = 0, running = 0;
};
Counts counts_from(const json& j);

struct Folder {
    std::string id, name;
};

struct StoredEvent {
    long long seq = 0;
    std::string at, kind;
    json payload = json::object();
};

// The tokens a Run's model calls used (client/model-access Usage Accounting).
struct RunUsage {
    long long prompt = 0, completion = 0, reasoning = 0, calls = 0;
    bool operator==(const RunUsage&) const = default;
};

struct RunInfo {
    std::string id, parent_run_id, state, reason, role, origin_kind;
    bool unattended = false;
    long long active_ms = 0;
    RunUsage usage;
    std::string created_at, started_at, ended_at;
    std::vector<StoredEvent> events;
};

struct ActionItem {
    std::string id, thread_id, run_id;  // run_id empty: a standalone Action item
    std::string kind;                   // approval | question
    std::string call_id;
    json payload = json::object();
    std::string created_at, resolved_at;  // resolved_at empty while open
    json resolution;
    std::string raised_by;
    bool open() const { return resolved_at.empty(); }
};

struct QueuedMessage {
    long long id = 0;
    std::string text, mode, created_at;
};

struct FeedSnapshot {
    ThreadEntry entry;
    std::vector<RunInfo> runs, subagents;
    std::vector<ActionItem> actions;
    std::vector<QueuedMessage> queued;
};
FeedSnapshot snapshot_from(const json& j);
RunInfo run_from(const json& j);
ActionItem action_from(const json& j);

// A live Run event as the host forwards it (run.event); transient kinds have seq 0.
struct LiveRunEvent {
    std::string kind, run_id, thread_id;
    long long seq = 0;
    std::string at;
    json data = json::object();
};
LiveRunEvent live_event_from(const json& j);

struct SearchHit {
    std::string thread_id, title, run_id;
    long long seq = 0;
    std::string snippet;  // matched terms wrapped in [[ and ]]
    bool archived = false;
};

}  // namespace wb::shell
