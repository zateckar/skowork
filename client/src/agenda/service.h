// client/agenda service (add-agenda): settings, intake, detection, admission and merging, lifecycle, reminders and
// preparation. The app adapts mail, calendar, meeting and Chat events into SourceItems and calls offer(); tick() does
// the bounded background work.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "agenda/core.h"
#include "agenda/store.h"
#include "base/json.h"
#include "base/result.h"
#include "model/types.h"

namespace wb::agenda {

struct Preparation {
    std::string thread_id, run_id;
};

struct Env {
    // A policy value by dotted key ("agenda.mode"); empty when unset (the client defaults apply).
    std::function<std::optional<json>(const std::string&)> policy;
    model::IChatModel* model = nullptr;
    std::function<long long()> now;                  // Unix milliseconds
    std::function<int(long long)> tz_offset_min;     // local offset from UTC at that moment
    std::function<Identity()> identity;
    // Starts a preparation Run (design D5) in `thread_id`, or in a new Thread when it is empty.
    std::function<Result<Preparation>(const Item& item, const std::string& thread_id, const std::string& message,
                                      const std::string& preamble, bool unattended)> start_preparation;
    std::function<void(const std::string& title, const std::string& body, const std::string& route)> notify;
    std::function<bool()> hold_reminders;  // presentation mode or paused background work
    std::function<void()> changed;         // the Agenda changed: the UI reloads
};

constexpr size_t kPendingCap = 500;
constexpr long long kRequestIntervalMs = 60'000;
constexpr long long kMergeWindowMs = 7LL * 86'400'000;
constexpr long long kRetentionMs = 90LL * 86'400'000;
constexpr long long kAutoPrepareWindowMs = 7LL * 86'400'000;

class Service {
public:
    Service(Database& db, Env env);

    // Settings and status (Agenda Is Opt In).
    [[nodiscard]] bool available() const;  // policy.agenda.mode is not off
    [[nodiscard]] bool active();           // available and enabled by the user
    json status();
    Result<json> set(const json& patch);
    Status delete_all();  // Agenda Data Deletion

    // Intake (Agenda Item Sources): queues an item when the Agenda is active, its source kind is selected and the item
    // is not older than the enablement.
    void offer(const SourceItem& item);
    // The source item is gone, excluded or no longer accessible (Agenda Item Withdrawal And Retention).
    void withdraw(const std::string& kind, const std::string& source_id);

    // Bounded background work: one detection batch at most every 60 seconds, reminders, automatic preparation and
    // retention. Safe to call often; returns at once while another tick runs.
    void tick();

    // The open items by due time, and item actions (Agenda Item Lifecycle).
    json list();
    Result<json> mark_done(const std::string& id);
    Result<json> dismiss(const std::string& id);
    Result<json> reopen(const std::string& id);
    Result<json> edit(const std::string& id, const json& patch);
    Result<json> prepare(const std::string& id);  // Agenda Item Preparation

    // True for the Runs preparation started: the app's tool hook denies them everything but read-risk tools
    // (Preparation Has No Effects).
    bool is_preparation_run(const std::string& run_id);

    // Exposed for tests: one detection pass over the due queue regardless of the interval.
    void detect_now();

private:
    struct Settings {
        bool enabled = false;
        long long enabled_at = 0;
        bool mail = true, calendar = true, meetings = true, chats = true;
        bool auto_prepare = false;
        bool remind_day_before = true, remind_hour_before = true;
    };
    Settings settings();
    Status save(const Settings& s);
    json settings_json(const Settings& s);
    std::string policy_string(const std::string& key, const std::string& fallback) const;
    bool policy_bool(const std::string& key, bool fallback) const;
    long long policy_int(const std::string& key, long long fallback, long long lo, long long hi) const;
    double min_confidence() const;

    void detect(const std::vector<Pending>& batch);
    void accept(const Candidate& c, const SourceItem& item, const Due& due);
    std::optional<std::string> model_merge(const Candidate& c, const std::vector<Item>& nearby);
    void reminders(long long now);
    void auto_prepare(long long now);
    Result<json> start_preparation(Item item, bool unattended);
    std::string day_key(long long now);
    void changed();

    Database& db_;
    Store store_;
    Env env_;
    std::mutex mx_;
    std::atomic<bool> ticking_{false};
    long long next_request_at_ = 0;
    long long last_purge_ = 0;
    std::string last_error_;
    std::set<std::string> prep_runs_;
};

}  // namespace wb::agenda
