// The agent runtime (client/agent-runtime): Threads and Runs, the tool-calling loop, hooks with the mandatory policy
// handler, lazy tool loading, Questions and approvals with idempotent resume, message queue and steering, limits,
// background execution and cancellation by Run id, context compaction, large tool results as Thread attachments,
// subagents, and untrusted-data delimiters.
//
// Threading: every executing Run has its own thread (at most policy.llm.max_concurrent_runs at a time; interrupted
// Runs occupy none). A subagent Run executes on the tool thread of its parent's delegate call. The event listener is called on those threads, and on the caller's thread for calls made through
// the public API; it must not block. All public methods are thread safe.
#pragma once

#include "base/time.h"
#include "model/types.h"
#include "runtime/hooks.h"
#include "runtime/run_store.h"
#include "runtime/tools.h"
#include "runtime/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace wb::runtime {

struct StartOptions {
    Origin origin;           // default: a user message
    bool unattended = false; // Trigger Occurrences and Remote tasks start Unattended runs
    // The message is untrusted content from this source (a Remote task's prompt, client/scheduler Remote Task
    // Execution): the model gets preamble, then the text inside the Run's untrusted-data delimiter, and the content
    // counts as untrusted context for every policy decision of the Run.
    std::optional<Provenance> untrusted;
    std::string preamble;
};

// The four resolutions of an Approval (client/agent-runtime Approval Interrupts).
struct ApprovalResolution {
    enum class Kind { approve, approve_edited, reject, respond_instead };
    Kind kind = Kind::approve;
    json edited_args;  // approve_edited: validated against the tool schema before the call runs
    std::string text;  // respond_instead: returned to the model as the tool result without running the tool

    static ApprovalResolution approve() { return {}; }
    static ApprovalResolution approve_with(json args) { return {Kind::approve_edited, std::move(args), {}}; }
    static ApprovalResolution reject() { return {Kind::reject, nullptr, {}}; }
    static ApprovalResolution respond(std::string text) { return {Kind::respond_instead, nullptr, std::move(text)}; }
};

struct CompactionResult {
    long long id = 0;  // the thread_compactions record
    int messages_compacted = 0;
    long long tokens_before = 0, tokens_after = 0;
    std::string summary;
    model::Usage usage;  // of the summariser call
};

struct SendResult {
    std::string run_id;             // a Run was started for the message (the Thread had no active Run)
    long long queued_message = 0;   // the message was queued behind the active Run (its id)
};

class AgentRuntime {
public:
    // Fails with code "no_policy_handler" when the hook registry has no policy handler (Mandatory Policy Hook).
    static Result<std::unique_ptr<AgentRuntime>> create(RuntimeConfig config, Database& db, model::IChatModel& model,
                                                        const Clock& clock, HookRegistry hooks);
    ~AgentRuntime();
    AgentRuntime(const AgentRuntime&) = delete;
    AgentRuntime& operator=(const AgentRuntime&) = delete;

    // Receives every event (see RunEvent). Set before start().
    void set_listener(EventListener listener);

    // Registers a tool, replacing one with the same id. The ids find_tools, ask_user, read_attachment, delegate and
    // use_skill are reserved. Tools may be added and removed while Runs are active (MCP catalog updates): the change applies from the
    // next model call, and a call already in flight finishes with the definition it started with.
    Status add_tool(ToolDef tool);
    // Withdraws a tool; returns false when there was none.
    bool remove_tool(const std::string& id);
    // The registered tools (runtime tools excluded) with their declared Risk class and whether policy permits them
    // and they are offered now: the input of a Skill's tool scope for the Skills view.
    [[nodiscard]] std::vector<skills::ToolFacts> tool_facts();

    // Recovers from the last shutdown (Runs that were running become failed: "interrupted by shutdown"; queued Runs stay
    // queued in their order; interrupted Runs stay interrupted) and starts queued Runs and Runs whose Action items are
    // resolved. Returns the report.
    Result<RecoveryReport> start();

    // Stops all Run threads without ending their Runs: a Run that was running stays "running" in the database and is
    // recovered as failed by the next start(), exactly as after a crash. Called by the destructor.
    void shutdown();

    // ---- Threads and Runs ----
    Result<std::string> create_thread(const std::string& title = {});

    // Starts a Run in a Thread that has no active Run. Fails with "thread_busy" (use send_message), "blocked" (the start
    // gate: disconnected, minimum version, locked), "revoked" or "not_found". Returns the Run id; the Run is queued until a slot is free.
    Result<std::string> start_run(const std::string& thread_id, const std::string& user_message, const StartOptions& options = {});

    // The user sends a message: starts a Run when the Thread has none, else queues it (queue: delivered when the Run ends or
    // its interrupt resolves; steer_now: injected before the next model call of the Run). While the client is offline
    // (the start gate's code "offline", client/control-channel Offline Operation) the message is queued even in a Thread
    // without a Run, and resume_waiting() starts it once the client is connected.
    Result<SendResult> send_message(const std::string& thread_id, const std::string& text, MessageMode mode = MessageMode::queue);

    // Run Retry: continues the latest top-level Run of the Thread when it failed, without a new user message. Fails
    // with "not_failed" when that Run did not fail, "thread_busy", "blocked"/"offline"/"revoked" (as start_run), or
    // "not_found". Returns the Run id; the Run is queued until a slot is free.
    Result<std::string> retry_run(const std::string& thread_id);
    // Starts a Run with the oldest queued message of every Thread that has queued messages and no active Run, oldest
    // first (called when the client connects again). Returns how many Runs started.
    int resume_waiting();
    Status edit_queued_message(long long id, const std::string& text);
    Status remove_queued_message(long long id);
    [[nodiscard]] std::vector<QueuedMessage> queued_messages(const std::string& thread_id);

    // "Compact Thread" (Manual Compaction): the same procedure as automatic compaction, on the caller's thread (it calls
    // the model). Fails with "thread_busy" while a Run of the Thread is running and "nothing_to_compact" when the
    // Thread is too short.
    Result<CompactionResult> compact_thread(const std::string& thread_id);

    // ---- Action items ----
    Status resolve_approval(const std::string& action_id, const ApprovalResolution& resolution);
    // Answers a Question: one of its choices, or free text when the Question allows it. A standalone Question is resolved
    // and its answer goes to the standalone handler.
    Status answer_question(const std::string& action_id, const std::string& answer);

    // Standalone Action items (add-work-inbox design D9): a Question with fixed choices raised by a client capability
    // without a Run, held by an existing Thread. Emits action.created with an empty run_id. A non-empty `copy` is text the
    // Action card shows with a Copy button (client/meeting-capture participant notice).
    Result<ActionItem> raise_standalone_question(const std::string& thread_id, const std::string& raised_by,
                                                 const std::string& prompt, const std::vector<std::string>& choices,
                                                 const std::string& copy = {});
    // Resolves an open standalone Question as {kind: "withdrawn", reason} without an answer (client/scheduler design D9:
    // a Remote task claimed elsewhere or cancelled by its issuer). The raiser's handler is not called.
    Status withdraw_standalone(const std::string& action_id, const std::string& reason);
    // Called on the answering thread after a standalone Question was resolved.
    void set_standalone_handler(std::function<void(const ActionItem&, const std::string& answer)> handler);

    // The size of the Thread's context as the next request would send it (without tool definitions), and the context
    // window of the model role, for the context-usage indicator.
    struct ContextUsage {
        long long tokens = 0, window = 0;
        long long input_budget = 0, trigger = 0, target = 0, output_reserve = 0;
        long long tool_tokens = 0;
        bool includes_tools = false;
        double compaction_threshold = 0;
    };
    [[nodiscard]] ContextUsage context_usage(const std::string& thread_id);

    // ---- Control ----
    // Cancels the Run with exactly this id. A Run that already ended is not affected (error "not_active"), and no other
    // Run of the Thread is touched.
    Status cancel(const std::string& run_id, const std::string& reason = "cancelled by user");
    // A Skill was revoked or removed (client/skills Skill Revocation): every subagent running it that has not ended is
    // cancelled with `reason`, and its parent gets that as the call's result. Returns how many were stopped.
    int stop_skill(const std::string& name, const std::string& reason);
    // Access was revoked (client/control-channel): queued Runs fail with "access revoked", active and interrupted Runs are
    // cancelled, and no Run starts until restore_access().
    void revoke(const std::string& reason = "access revoked");
    void restore_access();

    // ---- Queries ----
    [[nodiscard]] std::optional<RunRecord> run(const std::string& run_id) { return store_.run(run_id); }
    [[nodiscard]] std::vector<RunRecord> runs(const std::string& thread_id) { return store_.runs_in_thread(thread_id); }
    // The newest top-level Run with this origin (client/scheduler idempotent Run creation).
    [[nodiscard]] std::optional<RunRecord> run_by_origin(const Origin& origin) { return store_.run_by_origin(origin); }
    [[nodiscard]] bool thread_exists(const std::string& id) { return store_.thread_exists(id); }
    [[nodiscard]] std::vector<StoredEvent> events(const std::string& run_id, long long after_seq = 0) { return store_.events(run_id, after_seq); }
    [[nodiscard]] std::vector<ActionItem> pending_actions(const std::string& thread_id = {}) { return store_.open_actions(thread_id); }
    [[nodiscard]] std::vector<model::ChatMessage> conversation(const std::string& thread_id) { return store_.conversation(thread_id); }
    // Number of Runs currently executing (they occupy a slot).
    [[nodiscard]] size_t executing_runs();
    RunStore& store() { return store_; }

    // Child Run waits (client/agent-runtime Child Run Approval Expiry, Child Run Time Limit): expires the Approvals of
    // subagents of attended Runs that were unanswered for 30 minutes, and ends the subagents that waited 60 minutes. The
    // application calls it periodically; returns how many Action items or Runs it changed. Top-level Runs are not touched.
    size_t expire_child_waits();

    // Waits until the Run is in `state` (tests and callers that need to block).
    bool wait_for_state(const std::string& run_id, RunState state, std::chrono::milliseconds timeout);

private:
    class Exec;
    friend class Exec;

    AgentRuntime(RuntimeConfig config, Database& db, model::IChatModel& model, const Clock& clock, HookRegistry hooks);

    struct Active {
        std::shared_ptr<RunControl> control;
        std::thread thread;
        bool unattended = false;
    };

    void emit(const std::string& kind, const std::string& run_id, const std::string& thread_id, json data, long long seq = 0);
    void emit_state(const RunRecord& before, RunState to, const std::string& reason, long long seq);
    void pump_locked();
    void spawn_locked(const std::string& run_id, bool unattended);
    void exec_finished(const std::string& run_id);
    void after_run_ended(const std::string& thread_id);
    // Starts a Run with the Thread's oldest queued message when it has no active Run and the start gate is open.
    bool start_oldest_locked(const std::string& thread_id);
    void maybe_ready(const std::string& run_id);
    void resolve_open_actions(const std::string& run_id, const std::string& why);
    Status end_in_place(const RunRecord& run, RunState to, const std::string& reason);
    // Subagents: the Run and all its descendants, the root of a Run, open Action items anywhere in a subtree.
    std::vector<RunRecord> subtree(const std::string& run_id);
    std::string root_of(const std::string& run_id);
    bool subtree_has_open_actions(const std::string& run_id);
    // Context Compaction (design D4). `run_id` is the compacting Run (empty for a manual compaction); `except_run` keeps
    // its open tool batch unanswered.
    Result<CompactionResult> compact(const std::string& thread_id, const std::string& run_id, const std::string& except_run,
                                     const std::string& kind, const RunInfo& info, const std::shared_ptr<CancelToken>& cancel);
    [[nodiscard]] long long context_window(model::Role role);
    [[nodiscard]] int output_limit(model::Role role);
    [[nodiscard]] const RunLimits& limits_for(bool unattended) const { return unattended ? cfg_.unattended : cfg_.attended; }
    Result<std::string> start_run_locked(const std::string& thread_id, const std::string& text, const StartOptions& options);

    RuntimeConfig cfg_;
    Database& db_;
    model::IChatModel& model_;
    const Clock& clock_;
    HookRegistry hooks_;
    RunStore store_;

    std::recursive_mutex tools_mx_;
    ToolRegistry tools_;

    std::recursive_mutex mx_;
    std::condition_variable_any cv_;
    std::map<std::string, Active> active_;
    std::map<std::string, std::shared_ptr<RunControl>> children_;  // subagent Runs executing inside a parent's batch
    std::deque<std::string> resume_ready_;
    std::vector<std::pair<std::thread::id, std::thread>> finished_;
    std::set<std::string> sessions_;
    std::atomic<bool> stopping_{false};
    bool revoked_ = false;
    bool started_ = false;

    std::recursive_mutex emit_mx_;
    std::mutex context_estimate_mx_;
    std::map<std::string, long long> context_tool_estimates_;
    EventListener listener_;
    std::mutex standalone_mx_;
    std::function<void(const ActionItem&, const std::string&)> standalone_handler_;
};

}  // namespace wb::runtime
