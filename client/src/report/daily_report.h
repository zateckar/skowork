// client/daily-report in the host (add-local-outlook-and-teams design D7): settings, the schedule with its late-start
// rule, and writing the report: the app collects the digest from the local mail, calendar, Teams and Agenda copies, and
// starts a read-only Unattended run in a new system-origin Thread. The report Threads are remembered so the tool hook
// keeps their Runs read-only (Daily Report Has No Effects).
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "report/report_core.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace wb::report {

struct Env {
    std::function<std::optional<std::string>(const std::string& key)> get;  // durable settings (AppStore)
    std::function<Status(const std::string& key, const std::string& value)> set;
    std::function<std::optional<json>(const std::string& key)> policy;
    std::function<long long()> now;                     // unix ms
    std::function<LocalTime(long long ms)> local;       // the local calendar time of a moment
    std::function<bool()> paused;                       // background deferral (client/desktop-host)
    std::function<bool()> sources_ready;                // mail or Teams can contribute (else the report waits)
    // Fills the digest with what arrived since `since_ms` (0: the last 24 hours) and today's events.
    std::function<Digest(long long since_ms, long long now_ms)> collect;
    // Creates the report Thread (origin system, unread), registers it, and starts the Unattended run; the Thread id.
    std::function<Result<std::string>(const std::string& title, const std::string& message, const std::string& digest,
                                      const std::string& preamble)>
        start;
    std::function<void(const std::string& title, const std::string& route)> notify;
    std::function<void()> changed;
};

class DailyReport {
public:
    explicit DailyReport(Env env) : env_(std::move(env)) {}

    [[nodiscard]] bool available();  // policy.daily_report.mode is on
    json status();
    Status configure(const json& patch);
    // Writes the report when it is due (call it every minute or so).
    void tick();
    // Writes today's report now, whatever the schedule ("Write now" in settings); fails "unavailable" when off.
    Result<std::string> write_now();
    // True for a report Thread: its Runs may use read-risk tools only.
    bool is_report_thread(const std::string& thread_id);
    // Forget: settings stay, the remembered report Threads and the last-report time go.
    Status forget();

private:
    Settings settings();
    Result<std::string> write(long long now);

    Env env_;
    std::mutex mx_;
};

}  // namespace wb::report
