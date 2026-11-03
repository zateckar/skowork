// Remote tasks on the client (client/scheduler Remote Task Claim, Acceptance and Execution; add-scheduled-and-remote-tasks
// design D6, D7, D9): a remote-task push is claimed first; a won claim becomes a Thread with origin remote, either
// with an acceptance Question or, for a trusted issuer, directly as an Occurrence whose Run gets the task as untrusted
// content; the Occurrence's progress goes back to control-plane/task-dispatch as remote-task-status.
//
// Threading: push and reply handlers run on the control channel's thread, answers on the answering thread, and the
// Occurrence listener under the scheduler's lock. The class never calls the scheduler while holding its own lock.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "runtime/hooks.h"
#include "scheduler/scheduler.h"
#include "storage/database.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::scheduler {

// The answers of the acceptance Question (Remote Task Acceptance).
inline constexpr const char* kAcceptChoice = "Accept";
inline constexpr const char* kAlwaysAcceptChoice = "Always accept from this issuer";
inline constexpr const char* kDeclineChoice = "Decline";

struct RemoteTasksEnv {
    Database* db = nullptr;  // remote_tasks and remote_trusted_issuers (app.db)
    const Clock* clock = nullptr;
    std::function<Scheduler*()> scheduler;  // null while it is not running
    // A client message to control-plane/task-dispatch through the control channel's outbox; `ref` is the remote-task push.
    std::function<Status(const std::string& type, long long ref, const json& payload)> send;
    std::function<std::optional<json>(const std::string& key)> policy;
    // A Thread with this origin (kind "remote", ref {task_id, issuer_id, issuer, issuer_kind, link}).
    std::function<Result<std::string>(const std::string& title, const json& ref)> create_thread;
    // The text of an attachment (document extraction or text decoding), or an error when it cannot be used.
    std::function<Result<std::string>(const std::string& name, const std::string& bytes)> attachment_text;
    // Stores a Thread attachment with untrusted provenance; returns its id.
    std::function<Result<std::string>(const std::string& thread_id, const std::string& name, const std::string& text,
                                      const runtime::Provenance& prov)>
        add_attachment;
    // A client-raised Question in the Thread; returns its Action item id.
    std::function<Result<std::string>(const std::string& thread_id, const std::string& prompt, const std::vector<std::string>& choices)> raise;
    std::function<void(const std::string& action_id, const std::string& reason)> withdraw;
    // The last answer of a Run, for Result Sharing.
    std::function<std::string(const std::string& run_id)> run_result;
    std::function<void()> changed;  // the Remote task history changed (Automations section)
};

class RemoteTasks {
public:
    explicit RemoteTasks(RemoteTasksEnv env);

    // Push messages (client/control-channel; the payload of the envelope).
    void on_remote_task(long long push_id, const json& payload);
    void on_claimed_elsewhere(const json& payload);
    void on_cancelled(const json& payload);
    // The server's answer to a client message: kind "reply" or "error", ref = the remote-task push id.
    void on_reply(const std::string& kind, long long ref, const json& payload);
    // The answer to an acceptance Question.
    void on_answer(const std::string& action_id, const std::string& answer);
    // Hooked to Scheduler::on_occurrence.
    void on_occurrence(const OccurrenceInfo& o);

    struct Row {
        std::string task_id, issuer_id, issuer_name, issuer_kind, state, action_id, thread_id, occurrence_id, note;
        long long push_ref = 0;
        json payload;
    };
    [[nodiscard]] std::optional<Row> task(const std::string& task_id);
    [[nodiscard]] bool trusted(const std::string& issuer_id);

private:
    std::optional<Row> row_where(const std::string& column, const std::string& value);
    std::optional<Row> row_by_ref(long long ref);
    void set(const std::string& task_id, const std::string& state, const std::string& note = {});
    void accepted(const Row& r);
    void ask(const Row& r);
    bool has_issuer(const std::string& policy_key, const std::string& issuer_id);
    void changed();

    RemoteTasksEnv env_;
    std::recursive_mutex mx_;
};

}  // namespace wb::scheduler
