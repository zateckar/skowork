#include "report/daily_report.h"

#include <algorithm>

namespace wb::report {
namespace {

constexpr char kSettings[] = "daily_report.settings";
constexpr char kLastDay[] = "daily_report.last_day";
constexpr char kLastAt[] = "daily_report.last_at";
constexpr char kThreads[] = "daily_report.threads";
constexpr size_t kRememberedThreads = 60;

}  // namespace

bool DailyReport::available() {
    const auto v = env_.policy ? env_.policy("daily_report.mode") : std::nullopt;
    return !(v && v->is_string() && v->get<std::string>() == "off");
}

Settings DailyReport::settings() {
    const auto raw = env_.get ? env_.get(kSettings) : std::nullopt;
    const auto j = raw ? parse_json(*raw) : std::nullopt;
    return parse_settings(j ? *j : json::object());
}

json DailyReport::status() {
    const std::lock_guard lock(mx_);
    json j = settings_json(settings());
    j["available"] = available();
    j["last_day"] = env_.get ? env_.get(kLastDay).value_or("") : "";
    j["last_thread"] = "";
    if (const auto t = env_.get ? env_.get(kThreads) : std::nullopt)
        if (auto list = parse_json(*t); list && list->is_array() && !list->empty()) j["last_thread"] = list->back();
    return j;
}

Status DailyReport::configure(const json& patch) {
    const std::lock_guard lock(mx_);
    Settings s = settings();
    std::string why;
    if (!apply_patch(s, patch, &why)) return make_error("invalid", why);
    if (s.enabled && !available()) return make_error("unavailable", "The organisation has turned off the Daily report (policy.daily_report.mode).");
    if (Status st = env_.set(kSettings, settings_json(s).dump()); !st) return st;
    if (env_.changed) env_.changed();
    return {};
}

bool DailyReport::is_report_thread(const std::string& thread_id) {
    if (thread_id.empty() || !env_.get) return false;
    const auto t = env_.get(kThreads);
    const auto list = t ? parse_json(*t) : std::nullopt;
    if (!list || !list->is_array()) return false;
    return std::any_of(list->begin(), list->end(), [&](const json& x) { return x.is_string() && x.get<std::string>() == thread_id; });
}

void DailyReport::tick() {
    if (!available() || (env_.paused && env_.paused())) return;
    const long long now = env_.now();
    {
        const std::lock_guard lock(mx_);
        const Settings s = settings();
        const std::string last = env_.get(kLastDay).value_or("");
        if (!report_due(s, env_.local(now), last)) return;
        if (env_.sources_ready && !env_.sources_ready()) return;
    }
    (void)write(now);
}

Result<std::string> DailyReport::write_now() {
    if (!available()) return make_error("unavailable", "The organisation has turned off the Daily report.");
    return write(env_.now());
}

Result<std::string> DailyReport::write(long long now) {
    const std::lock_guard lock(mx_);
    const LocalTime lt = env_.local(now);
    const std::string date = day_key(lt);
    long long since = 0;
    if (const auto v = env_.get(kLastAt))
        try {
            since = std::stoll(*v);
        } catch (...) {
            since = 0;
        }
    if (since <= 0 || now - since > 7LL * 86'400'000) since = now - 86'400'000;
    Digest d = env_.collect(since, now);
    d.date = date;
    const std::string digest = render_digest(d);
    // Mark the day first: a failure is not retried in a loop every minute; "Write now" stays available.
    (void)env_.set(kLastDay, date);
    auto thread = env_.start("Daily report – " + date, run_message(date), digest, instructions());
    if (!thread) return thread.error();
    (void)env_.set(kLastAt, std::to_string(now));
    json list = json::array();
    if (const auto t = env_.get(kThreads))
        if (auto j = parse_json(*t); j && j->is_array()) list = *j;
    if (std::find(list.begin(), list.end(), json(*thread)) == list.end()) list.push_back(*thread);  // the host may have added it
    while (list.size() > kRememberedThreads) list.erase(list.begin());
    (void)env_.set(kThreads, list.dump());
    if (env_.notify) env_.notify("Your Daily report is ready", "#/threads/" + *thread);
    if (env_.changed) env_.changed();
    return *thread;
}

Status DailyReport::forget() {
    const std::lock_guard lock(mx_);
    for (const char* k : {kLastDay, kLastAt, kThreads})
        if (Status s = env_.set(k, ""); !s) return s;
    return {};
}

}  // namespace wb::report
