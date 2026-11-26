// client/event-triage (improve-proactive-durable-work design D1, D2): opt-in triage of eligible mail and calendar
// changes into results. The mail sync's page transaction takes changes in (take_in); one background worker classifies
// pending events in bounded batches through the gateway's small role. A relevant item that needs action gets a
// system-origin Thread, prepared automatically by a read-only Unattended run (improve-triage-results D2); one for
// information waits for the Daily report (D3).
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "eval/decision_log.h"
#include "mail/mail_store.h"
#include "mail/mail_sync.h"
#include "model/types.h"
#include "runtime/runtime.h"
#include "storage/database.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wb::triage {

inline constexpr int kMaxPending = 1000;
inline constexpr long long kExpiryMs = 30LL * 86'400'000;
inline constexpr size_t kBatch = 20;
inline constexpr long long kGatherMs = 30'000;
inline constexpr long long kIntervalMs = 60'000;
inline constexpr long long kMaxBackoffMs = 3'600'000;
// improve-triage-results D10: the reason stored for an event set aside after an invalid response on its own.
inline constexpr const char* kSetAsideReason = "Triage could not classify this item.";
inline constexpr size_t kMaxResponsibilities = 4096;
inline constexpr size_t kMaxEventText = 8192;
inline constexpr long long kDailyPreparations = 30;
inline constexpr long long kInfoKeepMs = 7LL * 86'400'000;

// A Teams message as triage sees it (add-teams-triage): read live from the local Teams copy (client/teams-chats).
struct TriageChat {
    std::string conversation, sender, text;
    long long at_ms = 0;
    bool addressed = false;  // a one-to-one chat, or a mention of the user
    bool mine = false, system = false;
};

struct TriageEnv {
    Database* db = nullptr;
    const Clock* clock = nullptr;
    mail::MailStore* mail = nullptr;
    // Teams scope item ids are "<conversation id>|<message id>". Empty when Teams chats are off, excluded or gone.
    std::function<std::optional<TriageChat>(const std::string& conversation_id, const std::string& message_id)> teams;
    model::IChatModel* model = nullptr;
    std::function<bool()> background_paused;  // Pause background work (client/desktop-host)
    // A Thread with origin system and this origin ref, unread.
    std::function<Result<std::string>(const std::string& title, const json& ref)> create_thread;
    std::function<Status(const std::string& thread_id, const std::string& title)> rename_thread;
    std::function<Status(const std::string& thread_id)> mark_unread;
    std::function<Status(const std::string& thread_id)> delete_thread;
    std::function<Status(const std::string& thread_id, const json& ref)> set_origin_ref;
    std::function<Result<std::string>(const std::string& thread_id, const std::string& text, const runtime::StartOptions& options)> start_run;
    // policy.unattended.allowed_risk includes read: automatic preparation may run (improve-triage-results D2).
    std::function<bool()> unattended_may_read;
    // The local day of a moment, "YYYY-MM-DD", for the daily preparation cap; UTC when unset.
    std::function<std::string(long long ms)> local_day;
    // No action needed (improve-triage-results D7): cancel the Thread's active Run, if any, and archive the Thread.
    std::function<void(const std::string& thread_id)> cancel_runs;
    std::function<Status(const std::string& thread_id)> archive_thread;
    // Settings, status or a suggestion changed (the UI reloads).
    std::function<void()> changed;
    // The user decision log (client/evaluation); null: decisions are not logged.
    eval::DecisionLog* decisions = nullptr;
    // A quality counter of triage went up by one (client/telemetry Quality Counters); empty: not counted.
    std::function<void(const std::string& counter)> quality;
};

// The text triage classifies for an item (add-evaluation-export design D4: a replay sends the same text), cut to limit.
std::string mail_event_text(const mail::Person& from, const std::string& subject, long long received_at, const std::string& body,
                            size_t limit);
std::string teams_event_text(const std::string& sender, const std::string& conversation, long long at_ms, const std::string& body,
                             size_t limit);
std::string calendar_event_text(const std::string& subject, const mail::Person& organizer, long long start_at, long long end_at,
                                const std::string& location, bool cancelled, const std::string& body, size_t limit);

// The instruction of an automatic preparation Run for an item of this scope (improve-triage-results D2, D6).
std::string prepare_preamble(const std::string& scope, const std::string& item_id);

class TriageService {
public:
    explicit TriageService(TriageEnv env);
    ~TriageService();
    TriageService(const TriageService&) = delete;
    TriageService& operator=(const TriageService&) = delete;

    // The background worker: one request at a time, at most one start per kIntervalMs.
    void start();
    void stop();
    void wake();

    // ---- settings and status (Settings > Triage) ----
    json status();
    // {"enabled", "calendar", "folders": [names], "responsibilities"}; any subset. Enabling or changing the scope starts
    // a baseline; disabling stops new requests and discards in-flight results.
    Status configure(const json& params);
    // The user saw the gap; once capacity is free, a fresh baseline starts. Fails with "full" while 1,000 are pending.
    Status acknowledge_gap();
    // Deletes pending events and suggestions; Threads where work started stay, without their triage link.
    Status delete_data();

    // ---- intake ----
    // The mail sync's page sink: runs inside the page transaction.
    Status take_in(const std::vector<mail::PageChange>& changes);
    // A Teams message event (client/teams-chats Teams Event Feed): kind teams.message | teams.updated | teams.deleted.
    Status take_in_teams(const std::string& kind, const std::string& conversation_id, const std::string& message_id);
    // After a complete sync cycle: the baseline completes, items no longer eligible are withdrawn, old events expire.
    void cycle_completed();
    // Withdraws everything that is no longer eligible (after a settings change, disconnect or exclusion).
    void sweep();

    // ---- suggestions ----
    // The suggestion of a Thread for its view: status, reason and live source details (null when not a suggestion).
    json suggestion(const std::string& thread_id);
    // Claims the open suggestion and starts an ordinary Run with the source as untrusted content. Fails "not_open".
    Result<std::string> start_work(const std::string& thread_id);
    Status dismiss(const std::string& thread_id);
    // No action needed: records the decision on an open or started item, cancels the Thread's Run and archives the
    // Thread. Later revisions keep the decision. Fails "not_open" for any other item.
    Status no_action(const std::string& thread_id);
    // The user deleted a suggestion Thread: the item counts as dismissed for its current revision and loses the link.
    Status thread_deleted(const std::string& thread_id);

    // ---- the Daily report (improve-triage-results D3) ----
    struct DigestItem {
        std::string thread_id, title, from, at, reason, category, scope, item_id;
    };
    struct Digest {
        std::vector<DigestItem> info, action;
    };
    // Unreported information items of the last 7 days and the open action items.
    Digest digest();
    // The report included these information items.
    void reported(const std::vector<DigestItem>& info);

    // One worker step: true when a classification request was made (tests drive it directly).
    bool step();

    // The revision of an item: a hash of what triage classifies (empty when the item is not in the local copy).
    std::string revision(const std::string& scope, const std::string& item_id);

private:
    struct Settings {
        bool enabled = false, calendar = false, teams = false, auto_prepare = true;
        std::vector<std::string> folders;
        std::string responsibilities;
    };
    Settings settings();
    std::string get(const std::string& key, const std::string& fallback = {});
    long long get_int(const std::string& key, long long fallback = 0);
    Status put(const std::string& key, const std::string& value);
    Status put_int(const std::string& key, long long value);
    Status bump(const std::string& key, long long by = 1);
    bool eligible(const Settings& s, const std::string& scope, const std::string& item_id);
    std::string source_text(const std::string& scope, const std::string& item_id);
    std::string source_title(const std::string& scope, const std::string& item_id);
    std::optional<TriageChat> chat(const std::string& item_id);
    bool exists(const std::string& scope, const std::string& item_id);
    // Withdraws the item's suggestion (or forgets the row when it has none).
    Status withdraw(const std::string& scope, const std::string& item_id);
    Status expire(long long now);
    // Starts the read-only preparation of an action item's Thread when allowed (D2); false when it stays a suggestion.
    bool prepare(const std::string& thread_id, const std::string& scope, const std::string& item_id);
    std::string day_of(long long ms);
    void notify();
    void count_failure(const Error& e);  // quality counter batch_failed_<kind>
    // Logs a decision on the item of a Thread (add-evaluation-export design D8); call with the database lock held.
    void log_decision(const std::string& thread_id, const std::string& kind, const std::string& before_suggestion);
    void run();

    TriageEnv env_;
    std::mutex worker_mx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopping_ = false;
    std::mutex step_mx_;  // one step at a time
};

}  // namespace wb::triage
