// client/event-triage (improve-proactive-durable-work design D1, D2): opt-in triage of eligible mail and calendar
// changes into reviewable suggestion Threads. The mail sync's page transaction takes changes in (take_in); one
// background worker classifies pending events in bounded batches through the gateway's small role; a relevant item gets
// a system-origin Thread the user can dismiss or start work from. A suggestion never starts a Run by itself.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "mail/mail_store.h"
#include "mail/mail_sync.h"
#include "model/types.h"
#include "runtime/runtime.h"
#include "storage/database.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
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
inline constexpr size_t kMaxResponsibilities = 4096;
inline constexpr size_t kMaxEventText = 8192;

struct TriageEnv {
    Database* db = nullptr;
    const Clock* clock = nullptr;
    mail::MailStore* mail = nullptr;
    model::IChatModel* model = nullptr;
    std::function<bool()> background_paused;  // Pause background work (client/desktop-host)
    // A Thread with origin system and this origin ref, unread.
    std::function<Result<std::string>(const std::string& title, const json& ref)> create_thread;
    std::function<Status(const std::string& thread_id, const std::string& title)> rename_thread;
    std::function<Status(const std::string& thread_id)> mark_unread;
    std::function<Status(const std::string& thread_id)> delete_thread;
    std::function<Status(const std::string& thread_id, const json& ref)> set_origin_ref;
    std::function<Result<std::string>(const std::string& thread_id, const std::string& text, const runtime::StartOptions& options)> start_run;
    // Settings, status or a suggestion changed (the UI reloads).
    std::function<void()> changed;
};

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
    // The user deleted a suggestion Thread: the item counts as dismissed for its current revision and loses the link.
    Status thread_deleted(const std::string& thread_id);

    // One worker step: true when a classification request was made (tests drive it directly).
    bool step();

    // The revision of an item: a hash of what triage classifies (empty when the item is not in the local copy).
    std::string revision(const std::string& scope, const std::string& item_id);

private:
    struct Settings {
        bool enabled = false, calendar = false;
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
    // Withdraws the item's suggestion (or forgets the row when it has none).
    Status withdraw(const std::string& scope, const std::string& item_id);
    Status expire(long long now);
    void notify();
    void run();

    TriageEnv env_;
    std::mutex worker_mx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopping_ = false;
    std::mutex step_mx_;  // one step at a time
};

}  // namespace wb::triage
