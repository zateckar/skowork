// Persistence of Threads, Runs and the append-only per-Run event log in app.db (client/agent-runtime Run Lifecycle
// States, Idempotent Resume, Thread Persistence; add-agent-runtime design D1 and D5). Every state change and every
// message is written here before the UI hears about it. All methods are safe to call from several threads.
#pragma once

#include "base/time.h"
#include "runtime/types.h"
#include "storage/database.h"

#include <optional>
#include <string>
#include <vector>

namespace wb::runtime {

// A message of a Thread with its position: the Run and the sequence number of its "message" event.
struct ContextMessage {
    model::ChatMessage msg;
    std::string run_id;
    long long seq = 0;
    bool carried = false;  // kept from before the compaction boundary (the latest user message)
};

// The model's view of a Thread (add-agent-runtime design D4): the latest compaction summary, the carried messages and
// every message from the compaction boundary on.
struct ThreadContext {
    std::string summary;
    std::vector<ContextMessage> messages;
};

struct CompactionRecord {
    long long id = 0;
    std::string thread_id, at;
    std::string kind = "automatic";  // automatic | manual
    std::string run_id;              // the Run that compacted (automatic)
    std::string summary;
    std::string boundary_run;        // the first kept message
    long long boundary_seq = 0;
    json carried = json::array();    // message events, each with run_id and seq
    int messages_compacted = 0;
    long long tokens_before = 0, tokens_after = 0;
};

struct AttachmentInfo {
    std::string id, thread_id, run_id, call_id, name;
    long long size = 0;
    json provenance;
    std::string created_at;
};

struct RecoveryReport {
    std::vector<std::string> failed_running;  // Runs that were running at shutdown, now failed
    int queued = 0;                           // Runs still queued, in their original order
    int interrupted = 0;                      // Runs still waiting for the user
};

struct ThreadSummary {
    std::string id, title, created_at, updated_at;
    std::string last_run_state;  // of the latest Run that is not a subagent; empty when the Thread has none
    int open_actions = 0;
};

class RunStore {
public:
    RunStore(Database& db, const Clock& clock) : db_(db), clock_(clock) {}

    // ---- Threads and Runs ----
    Result<std::string> create_thread(const std::string& title = {});
    // The most recently updated Threads (Local Run API; the inbox views of add-work-inbox replace this list).
    [[nodiscard]] std::vector<ThreadSummary> list_threads(int limit);
    [[nodiscard]] bool thread_exists(const std::string& id);
    Status set_thread_title(const std::string& id, const std::string& title);

    // Creates a Run in state queued (the "state" event is the first entry of its log).
    Result<RunRecord> create_run(const std::string& thread_id, const Origin& origin, bool unattended,
                                 const std::string& parent_run_id = {}, const std::string& role = "main");
    [[nodiscard]] std::optional<RunRecord> run(const std::string& id);
    [[nodiscard]] std::vector<RunRecord> runs_in_thread(const std::string& thread_id);
    [[nodiscard]] std::vector<RunRecord> runs_in_state(RunState state);  // by queue order
    // The newest top-level Run started with this origin (client/scheduler: idempotent Run creation per Occurrence).
    [[nodiscard]] std::optional<RunRecord> run_by_origin(const Origin& origin);
    // The Thread's top-level Run that is queued, running or interrupted, if any.
    [[nodiscard]] std::optional<RunRecord> active_run(const std::string& thread_id);

    // Moves a Run to `to` if the state machine allows it from its current state; otherwise fails with code
    // "illegal_transition" and changes nothing. Appends the "state" event and returns its sequence number.
    Result<long long> transition(const std::string& run_id, RunState to, const std::string& reason = {});

    // Startup recovery: Runs left running become failed ("interrupted by shutdown"), and so do queued subagent Runs (their
    // parent was running); other queued Runs and interrupted Runs stay.
    Result<RecoveryReport> recover();

    // ---- Event log ----
    Result<long long> append_event(const std::string& run_id, const std::string& kind, const json& payload);
    [[nodiscard]] std::vector<StoredEvent> events(const std::string& run_id, long long after_seq = 0);

    // ---- Counters ----
    Status set_active_ms(const std::string& run_id, long long ms);
    Status add_usage(const std::string& run_id, const model::Usage& usage);  // also counts usage.requests as model calls
    Status set_tool_calls(const std::string& run_id, int tool_calls);

    // ---- Action items (Approvals and Questions) ----
    Result<ActionItem> create_action(const std::string& run_id, const std::string& kind, const std::string& call_id,
                                     const json& payload);
    // A Question raised by a client capability without a Run (add-work-inbox Standalone Action Items, design D9).
    Result<ActionItem> create_standalone_action(const std::string& thread_id, const std::string& raised_by, const json& payload);
    [[nodiscard]] std::optional<ActionItem> action(const std::string& id);
    [[nodiscard]] std::vector<ActionItem> actions_of_run(const std::string& run_id);
    [[nodiscard]] std::vector<ActionItem> open_actions(const std::string& thread_id = {});
    // Stores the resolution of an open item; fails with "already_resolved" for a resolved one (the first answer wins).
    Status resolve_action(const std::string& id, const json& resolution);

    // ---- Queued messages ----
    Result<long long> queue_message(const std::string& thread_id, const std::string& text, MessageMode mode);
    [[nodiscard]] std::vector<QueuedMessage> pending_messages(const std::string& thread_id);
    // Threads with undelivered messages, the one with the oldest such message first.
    [[nodiscard]] std::vector<std::string> threads_with_pending_messages();
    Status edit_message(long long id, const std::string& text);    // only while undelivered
    Status remove_message(long long id);                           // only while undelivered
    [[nodiscard]] std::string message_thread(long long id);        // the Thread of a queued message ("" unknown)
    Status mark_delivered(long long id);

    // The Thread's conversation: the "message" events of its Runs in order, as chat messages. Tool calls that never got
    // a result (the Run failed or was cancelled in the middle of a batch) get a synthetic result, so the history stays valid
    // for the model. `except_run` is left as it is (the running Run keeps its own open batch).
    [[nodiscard]] std::vector<model::ChatMessage> conversation(const std::string& thread_id, const std::string& except_run = {});
    // The compacted view of the same conversation (see ThreadContext).
    [[nodiscard]] ThreadContext context(const std::string& thread_id, const std::string& except_run = {});

    // ---- Compaction records ----
    Result<long long> add_compaction(const CompactionRecord& record);
    [[nodiscard]] std::vector<CompactionRecord> compactions(const std::string& thread_id);

    // ---- Thread attachments (Large Tool Results) ----
    Result<std::string> add_attachment(const std::string& thread_id, const std::string& run_id, const std::string& call_id,
                                       const std::string& name, std::string_view content, const json& provenance);
    [[nodiscard]] std::optional<AttachmentInfo> attachment(const std::string& id);
    // Bytes [offset, offset + length) of the attachment.
    Result<std::string> read_attachment(const std::string& id, long long offset, long long length);

    [[nodiscard]] Database& db() { return db_; }

private:
    std::vector<ContextMessage> thread_messages(const std::string& thread_id, const std::string& except_run);
    std::optional<RunRecord> read_run(Statement& st);
    Result<long long> append_event_locked(const std::string& run_id, const std::string& kind, const json& payload);
    std::string now() const { return iso8601_utc(clock_.now()); }

    Database& db_;
    const Clock& clock_;
};

// ChatMessage <-> the payload of a "message" event.
json message_to_event(const model::ChatMessage& m, const json& extra = nullptr);
model::ChatMessage message_from_event(const json& payload);

}  // namespace wb::runtime
