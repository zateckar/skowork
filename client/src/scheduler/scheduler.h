// client/scheduler (add-scheduled-and-remote-tasks): Triggers, their Occurrences with leases, attempts and retries,
// catch-up of missed firings, Unattended runs within their slots, thread modes, history, and the policy limits on
// Triggers. Accepted Remote tasks run as Occurrences without a Trigger (design D6). The scheduler is driven by tick()
// and reads time only from its clock (design D9).
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "runtime/runtime.h"
#include "runtime/types.h"
#include "scheduler/cron.h"
#include "storage/database.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::scheduler {

inline constexpr const char* kKinds[] = {"schedule", "file", "mail", "calendar", "meeting", "session"};
inline constexpr int kMaxAttempts = 3;
inline constexpr long long kLeaseMs = 120'000;
inline constexpr long long kLateGraceMs = 120'000;  // a firing up to 2 minutes late is still "scheduled"
inline constexpr int kMaxSkippedRecorded = 20;

struct RunView {
    runtime::RunState state = runtime::RunState::queued;
    std::string reason;
};

struct FileStat {
    std::string path;
    long long size = 0, mtime = 0;
};

// An Occurrence as listeners see it (Remote task status reporting, the Automations view).
struct OccurrenceInfo {
    std::string id, trigger_id, remote_task_id, state, reason, thread_id, run_id, outcome;
    int attempts = 0;
};

struct SchedulerEnv {
    Database* db = nullptr;
    const Clock* clock = nullptr;
    std::string owner;            // the signed-in user's name
    std::string installation_id;  // lease owner
    // A policy value by key without "policy." (nullopt: the default applies).
    std::function<std::optional<json>(const std::string& key)> policy;
    std::function<bool()> paused;        // Pause background work (client/desktop-host)
    std::function<int()> runtime_slots;  // the runtime's max_concurrent_runs
    // Threads and Runs.
    std::function<Result<std::string>(const std::string& title, const std::string& origin_kind, const json& origin_ref)> create_thread;
    std::function<bool(const std::string& thread_id)> thread_exists;
    std::function<void(const std::string& thread_id, const std::string& origin_kind, const json& origin_ref)> set_origin;
    // Starts an Unattended run with this origin.
    // Starts the Occurrence's Unattended run; for a Remote task the options carry its untrusted provenance and preamble.
    std::function<Result<std::string>(const std::string& thread_id, const std::string& text, const runtime::StartOptions& options)> start_run;
    std::function<std::optional<std::string>(const runtime::Origin& origin)> find_run;
    std::function<std::optional<RunView>(const std::string& run_id)> run_state;
    std::function<Status(const std::string& run_id, const std::string& reason)> cancel_run;
    // "completed" when a Trigger with notify on completion succeeds; "failed" (with the error) when its Occurrence is dead.
    std::function<void(const std::string& kind, const std::string& reason, const std::string& thread_id)> notify;
    std::function<Result<std::shared_ptr<TimeZone>>(const std::string& iana_id)> zone;
    std::function<std::string()> local_zone;
    // policy.filesystem.read for the folder of a file Trigger.
    std::function<bool(const std::filesystem::path& folder)> folder_allowed;
    // Something changed that the Automations section shows.
    std::function<void()> changed;
};

// A detection source for the mail, calendar and meeting kinds (design D9): it validates conditions; matching events
// come in through Scheduler::source_event.
struct Source {
    std::function<Status(const json& conditions)> validate;
};

class Scheduler {
public:
    explicit Scheduler(SchedulerEnv env);

    // Recovery after a start (D1): expired leases, next firing times, policy limits, retention.
    void start();
    // Fires due Triggers, reads the Runs it started, starts ready Occurrences within the slots.
    void tick();

    // ---- Triggers (Trigger Definition, Policy Limits On Triggers) ----
    // def: {name, kind, conditions, prompt, skill?, thread_mode?, enabled?, notify?}. created_by: user | agent.
    Result<json> create(const json& def, const std::string& created_by = "user");
    Result<json> update(const std::string& id, const json& patch);
    Status remove(const std::string& id);
    [[nodiscard]] json list();
    Result<json> get(const std::string& id);  // with the history
    // Run now: a manual Occurrence, started right away when a slot is free.
    Result<json> run_now(const std::string& id);
    [[nodiscard]] json history(const std::string& trigger_id, int limit = 50);
    // Re-evaluates the policy limits of every Trigger (after a policy change).
    void policy_changed();

    // ---- events of the other Trigger kinds ----
    void session_event(const std::string& event, const std::string& key);  // logon | unlock
    // A stable file seen by the file watcher; fires once per (path, size, mtime) not seen before.
    void file_seen(const std::string& trigger_id, const std::string& path, long long size, long long mtime);
    // A complete, stable listing of a file Trigger's folder (startup reconciliation and every quiet change): the first
    // listing after the Trigger was created only records the files already there; later ones fire for new or changed
    // files, and files that are gone are forgotten.
    void file_scan(const std::string& trigger_id, const std::vector<FileStat>& files);
    void register_source(const std::string& kind, Source s);
    // The source became unavailable (for example the mailbox was disconnected): new Triggers of the kind are refused.
    void unregister_source(const std::string& kind);
    // An event of a detection source; every enabled Trigger of the kind whose conditions `matches` fires once per key.
    void source_event(const std::string& kind, const std::string& key, const json& detail, const std::function<bool(const json& conditions)>& matches);
    [[nodiscard]] std::vector<std::string> available_kinds();
    // The file Triggers' folders and patterns, for the watcher: [{id, folder, pattern}].
    [[nodiscard]] json file_triggers();

    // ---- Remote tasks (design D6) ----
    // An accepted Remote task as an Occurrence in this Thread; returns the Occurrence id. Its Run gets preamble and then
    // `text` as untrusted content from the issuer (client/policy-enforcement Untrusted Content Provenance).
    Result<std::string> remote_occurrence(const std::string& task_id, const std::string& thread_id, const std::string& text,
                                          const std::string& issuer_id = {}, const std::string& preamble = {});
    // The issuer cancelled: the Occurrence is cancelled and its Run stopped.
    void cancel_remote(const std::string& task_id, const std::string& reason);
    [[nodiscard]] std::optional<OccurrenceInfo> occurrence(const std::string& id);

    void on_occurrence(std::function<void(const OccurrenceInfo&)> listener);

    [[nodiscard]] long long now() const { return env_.clock->unix_millis(); }
    // How many Unattended runs may be active: policy.scheduler.max_concurrent_runs within the runtime's limit. Takes no
    // lock (the runtime calls it while holding its own).
    [[nodiscard]] int slots() const;

private:
    struct TriggerRow;
    std::optional<TriggerRow> trigger(const std::string& id);
    std::vector<TriggerRow> triggers();
    json to_json(const TriggerRow& t);
    Result<json> validate(const json& def, const std::string& except_id);
    std::optional<std::string> limit_violation(const TriggerRow& t, int index);
    std::optional<long long> next_after(const TriggerRow& t, long long after);
    Result<std::shared_ptr<TimeZone>> zone_of(const json& conditions);

    void fire_schedules(long long now);
    bool add_occurrence(const std::string& trigger_id, const std::string& fire_key, const std::string& reason, long long fire_at,
                        const json& detail, const std::string& state = "pending");
    void poll_running(long long now, bool at_start);
    int active_runs();
    void start_ready(long long now);
    bool begin_attempt(const std::string& occurrence_id, long long now);
    void attempt_failed(const std::string& occurrence_id, const std::string& error, long long now, bool lease_expired);
    void finish(const std::string& occurrence_id, const std::string& state, const std::string& outcome, long long now);
    void emit(const std::string& occurrence_id);
    std::string run_text(const TriggerRow& t, const json& detail);
    std::string local_text(const TriggerRow& t, long long at);
    long long policy_int(const std::string& key, long long fallback) const;
    void prune(long long now);

    SchedulerEnv env_;
    std::recursive_mutex mx_;
    std::map<std::string, Source> sources_;
    std::vector<std::function<void(const OccurrenceInfo&)>> listeners_;
    std::map<std::string, std::shared_ptr<TimeZone>> zones_;
    long long last_prune_ = 0;
};

}  // namespace wb::scheduler
