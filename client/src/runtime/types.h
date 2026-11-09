// Types of the agent runtime (client/agent-runtime). Glossary terms are used exactly: Thread, Run, Action item,
// Question, Risk class, Unattended run.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "model/types.h"
#include "skills/skill.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wb::runtime {

// ---- Run states ----------------------------------------------------------------------------------------------

enum class RunState { queued, running, interrupted, succeeded, failed, cancelled };

const char* to_string(RunState s);
std::optional<RunState> parse_run_state(std::string_view s);
[[nodiscard]] bool is_terminal(RunState s);

// The only allowed transitions (client/agent-runtime Run Lifecycle States):
//   queued -> running | failed | cancelled
//   running -> interrupted | succeeded | failed | cancelled
//   interrupted -> running | cancelled
[[nodiscard]] bool transition_allowed(RunState from, RunState to);

// ---- Risk class -----------------------------------------------------------------------------------------------

enum class Risk { read, write_local, external, destructive };

const char* to_string(Risk r);
std::optional<Risk> parse_risk(std::string_view s);

// ---- Run origin -----------------------------------------------------------------------------------------------

enum class OriginKind { user_message, trigger_occurrence, remote_task };

const char* to_string(OriginKind k);
std::optional<OriginKind> parse_origin(std::string_view s);

struct Origin {
    OriginKind kind = OriginKind::user_message;
    std::string ref;  // Trigger and Occurrence identifiers, or the Remote task id
};

// ---- Persisted records ------------------------------------------------------------------------------------------

struct RunRecord {
    std::string id, thread_id, parent_run_id;
    RunState state = RunState::queued;
    std::string reason;  // why the Run is failed or cancelled ("interrupted by shutdown", "tool call limit reached")
    Origin origin;
    bool unattended = false;
    long long queue_seq = 0;
    std::string role = "main";
    long long active_ms = 0;  // time spent running; interrupted time does not count (Run Limits)
    int model_calls = 0;
    int tool_calls = 0;
    model::Usage usage;
    std::string created_at, started_at, ended_at;
};

struct StoredEvent {
    long long seq = 0;
    std::string at;
    std::string kind;
    json payload;
};

struct ActionItem {
    std::string id, thread_id;
    std::string run_id;  // empty for a standalone Action item (add-work-inbox design D9)
    std::string kind;    // approval | question
    std::string call_id;
    json payload;
    std::string created_at, resolved_at;
    json resolution;
    std::string raised_by;  // the client capability that raised a standalone Action item
    [[nodiscard]] bool open() const { return resolved_at.empty(); }
};

enum class MessageMode { queue, steer_now };

struct QueuedMessage {
    long long id = 0;
    std::string thread_id, text;
    MessageMode mode = MessageMode::queue;
    std::string created_at;
};

// ---- Events streamed to the UI ----------------------------------------------------------------------------------

// kind is one of:
//   run.state        {state, reason}
//   message          {role, content, tool_calls?, tool_call_id?}     a message was added to the Thread
//   text.delta       {text}                                          streamed answer text (not persisted)
//   reasoning.delta  {text}                                          streamed reasoning (not persisted, never resent)
//   discard          {}                                              the streamed output so far belongs to a failed attempt
//   tool_call        {call_id, tool, args, risk, rationale}          a tool call was decided and starts
//   tool_result      {call_id, tool, ok, content}
//   action.created   {id, kind, call_id, payload}                    an Action item is waiting for the user (run_id is
//                                                                    empty for a standalone one)
//   action.resolved  {id, resolution}
//   usage            {prompt, completion, reasoning, requests}       totals of the Run so far
//   activity         {text}                                          Activity log entry (retries, edits, compaction)
//   compaction       {id, kind, messages_compacted, tokens_before, tokens_after}   the Thread was compacted
//   subagent         {call_id, child_run_id, task}                   a subagent Run was started for a delegate call
//   queue.changed    {}                                              queued messages of the Thread changed
//   error            {code, message, reason, partial_output, retry_after_s?, reset_at?}   the model request failed
struct RunEvent {
    std::string kind;
    std::string run_id, thread_id;
    json data;
    long long seq = 0;  // sequence number in the Run's event log; 0 for transient events
    std::string at;
};
using EventListener = std::function<void(const RunEvent&)>;

// ---- Configuration ---------------------------------------------------------------------------------------------

struct RunLimits {
    int model_calls = 60;
    int tool_calls = 200;
    int wall_minutes = 60;
};

// A Skill offered to the main agent (client/skills Automatic Skill Selection).
struct SkillInfo {
    std::string name, description;
};

// A Skill resolved for a run (client/skills Skills Run As Subagents): the active copy after tier resolution.
struct SkillRun {
    skills::Manifest manifest;
    std::string instructions;  // the SKILL.md body
    std::string tier;          // builtin | org | group | shared | private
    std::string folder;        // where its scripts/, references/ and assets/ are
};

struct RuntimeConfig {
    // policy.llm.run_limits.attended / .unattended (client/agent-runtime Run Limits)
    RunLimits attended{60, 200, 60};
    RunLimits unattended{30, 80, 30};
    int max_concurrent_runs = 3;  // policy.llm.max_concurrent_runs
    // Unattended runs executing at once (policy.scheduler.max_concurrent_runs, client/scheduler design D9): a resumed or
    // queued Unattended run waits while this many run. Unset: no limit beyond max_concurrent_runs.
    std::function<int()> unattended_slots;
    int max_parallel_tools = 4;   // policy.llm.max_parallel_tools
    int find_tools_max = 5;       // tools one find_tools call may add
    int tool_schema_token_budget = 4000;  // per-turn size of the tool definitions (Lazy Tool Loading)
    model::Role role = model::Role::main;
    std::string system_prompt = "You are SkoWork, an assistant that works for the user on their computer. Use the tools you have; "
                                "call find_tools to discover more. Ask the user with ask_user when you need a decision. "
                                "Delegate independent pieces of work to subagents with delegate.";
    // The system prompt of a subagent; the instructions of the delegate call follow it.
    std::string subagent_prompt = "You are a subagent of SkoWork. Complete the task you are given with the tools you have, then "
                                  "answer with the result only; your answer goes to the agent that delegated the task. Use "
                                  "ask_user only when you cannot continue without the user.";
    // Retained for source compatibility; the working-context budget now determines the trigger.
    double compaction_threshold = 0.85;
    int compaction_keep_messages = 6;          // K: the most recent messages are never compacted
    long long default_context_window = 32768;  // when the gateway model list reports none for the role
    // Large Tool Results: policy.llm.max_tool_result_kb.
    int max_tool_result_kb = 64;
    int tool_result_preview_bytes = 4096;
    // Returns a reason when new Runs may not start (disconnected, minimum version, locked; client/control-channel).
    std::function<std::optional<std::string>()> start_gate;
    // True when the only reason is that the client is offline (disconnected while signed in and registered): starting
    // fails with the code "offline", and the user's messages wait in their Thread (add-offline-operation D1).
    std::function<bool()> offline;
    // Run Context: what the model cannot discover (date, time zone, user, folders). Called when a Run starts or resumes
    // executing; the text is appended to the Run's system prompt and stays fixed for that execution.
    std::function<std::string()> run_context;
    // Skills (client/skills): the usable Skills, listed to the main agent when a Run starts executing, and the lookup
    // of one by name when the agent or the user's /name selects it. Without them no Skill is offered. Both take the
    // Thread: a Skill test Thread resolves its draft (add-skills design D10).
    std::function<std::vector<SkillInfo>(const std::string& thread_id)> skills;
    std::function<Result<SkillRun>(const std::string& thread_id, const std::string& name)> resolve_skill;
    // A Skill's subagent ended (Usage Reporting); `ok`: it succeeded.
    std::function<void(const std::string& thread_id, const std::string& name, bool ok)> skill_finished;
    // How long Run threads get to stop on shutdown.
    std::chrono::milliseconds shutdown_grace{5000};
};

}  // namespace wb::runtime
