#include "scheduler/scheduler.h"

#include "platform/crypto.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace wb::scheduler {

namespace {

constexpr long long kMinute = 60'000, kHour = 3'600'000, kDay = 86'400'000;
constexpr long long kBusyRetryMs = 30'000;  // the Thread was busy or the runtime not started: try again, no attempt counted
constexpr long long kOfflineRetryMs = 60'000;  // the client is offline: try again, no attempt counted
constexpr const char* kTriggerColumns =
    "id, name, kind, conditions, prompt, skill, thread_mode, thread_id, enabled, notify, owner, created_by, "
    "disabled_reason, next_fire_at, created_at, updated_at";

long long backoff_ms(int attempts) { return attempts <= 1 ? kMinute : 5 * kMinute; }

std::string new_id(const char* prefix) { return std::string(prefix) + random_urlsafe(9); }

bool is_kind(const std::string& k) {
    return std::any_of(std::begin(kKinds), std::end(kKinds), [&](const char* x) { return k == x; });
}

}  // namespace

struct Scheduler::TriggerRow {
    std::string id, name, kind, prompt, skill, thread_mode = "new", thread_id, owner, created_by = "user", disabled_reason;
    json conditions = json::object();
    bool enabled = true, notify = true;
    std::optional<long long> next_fire_at;
    long long created_at = 0, updated_at = 0;
};

Scheduler::Scheduler(SchedulerEnv env) : env_(std::move(env)) {}

// ---- helpers ---------------------------------------------------------------------------------------------------

long long Scheduler::policy_int(const std::string& key, long long fallback) const {
    if (!env_.policy) return fallback;
    const auto v = env_.policy(key);
    return v && v->is_number_integer() ? v->get<long long>() : fallback;
}

int Scheduler::slots() const {
    long long n = policy_int("scheduler.max_concurrent_runs", 2);
    if (env_.runtime_slots) n = std::min<long long>(n, env_.runtime_slots());
    return static_cast<int>(std::max<long long>(n, 0));
}

Result<std::shared_ptr<TimeZone>> Scheduler::zone_of(const json& conditions) {
    std::string id = jstr(conditions, "time_zone");
    if (id.empty()) id = env_.local_zone ? env_.local_zone() : "UTC";
    if (const auto it = zones_.find(id); it != zones_.end()) return it->second;
    if (!env_.zone) return std::shared_ptr<TimeZone>(std::make_shared<FixedZone>(0, id));
    auto z = env_.zone(id);
    if (!z) return z.error();
    zones_[id] = *z;
    return *z;
}

std::string Scheduler::local_text(const TriggerRow& t, long long at) {
    auto z = zone_of(t.kind == "schedule" ? t.conditions : json::object());
    const LocalTime l = z ? (*z)->local(at) : FixedZone().local(at);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", l.year, l.month, l.day, l.hour, l.minute);
    return buf;
}

std::optional<long long> Scheduler::next_after(const TriggerRow& t, long long after) {
    if (t.kind != "schedule") return std::nullopt;
    if (t.conditions.contains("interval_minutes")) {
        const long long iv = jint(t.conditions, "interval_minutes") * kMinute;
        if (iv <= 0) return std::nullopt;
        const long long anchor = t.created_at;
        if (after < anchor) return anchor;
        return anchor + ((after - anchor) / iv + 1) * iv;
    }
    const auto c = parse_cron(jstr(t.conditions, "cron"));
    const auto z = zone_of(t.conditions);
    if (!c || !z) return std::nullopt;
    return next_fire(*c, **z, after);
}

std::optional<Scheduler::TriggerRow> Scheduler::trigger(const std::string& id) {
    auto s = env_.db->prepare(std::string("SELECT ") + kTriggerColumns + " FROM triggers WHERE id=?");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    TriggerRow t;
    t.id = s.col_text(0);
    t.name = s.col_text(1);
    t.kind = s.col_text(2);
    t.conditions = parse_json(s.col_text(3)).value_or(json::object());
    t.prompt = s.col_text(4);
    t.skill = s.col_text(5);
    t.thread_mode = s.col_text(6);
    t.thread_id = s.col_text(7);
    t.enabled = s.col_int(8) != 0;
    t.notify = s.col_int(9) != 0;
    t.owner = s.col_text(10);
    t.created_by = s.col_text(11);
    t.disabled_reason = s.col_text(12);
    if (!s.col_null(13)) t.next_fire_at = s.col_int(13);
    t.created_at = s.col_int(14);
    t.updated_at = s.col_int(15);
    return t;
}

std::vector<Scheduler::TriggerRow> Scheduler::triggers() {
    std::vector<std::string> ids;
    auto s = env_.db->prepare("SELECT id FROM triggers ORDER BY created_at, id");
    while (s.step()) ids.push_back(s.col_text(0));
    std::vector<TriggerRow> out;
    for (const auto& id : ids)
        if (auto t = trigger(id)) out.push_back(std::move(*t));
    return out;
}

json Scheduler::to_json(const TriggerRow& t) {
    json j = {{"id", t.id},           {"name", t.name},         {"kind", t.kind},
              {"conditions", t.conditions}, {"prompt", t.prompt}, {"skill", t.skill},
              {"thread_mode", t.thread_mode}, {"thread_id", t.thread_id}, {"enabled", t.enabled},
              {"notify", t.notify},     {"owner", t.owner},       {"created_by", t.created_by},
              {"disabled_reason", t.disabled_reason}};
    if (t.next_fire_at && t.enabled && t.disabled_reason.empty()) {
        j["next_fire_at"] = *t.next_fire_at;
        j["next_fire_text"] = local_text(t, *t.next_fire_at);
    }
    auto s = env_.db->prepare(
        "SELECT fire_at, state, outcome, thread_id, run_id FROM occurrences WHERE trigger_id=? AND state NOT IN ('skipped','pending') "
        "ORDER BY fire_at DESC, created_at DESC LIMIT 1");
    s.bind(1, t.id);
    if (s.step())
        j["last_run"] = {{"fire_at", s.col_int(0)}, {"fire_text", local_text(t, s.col_int(0))}, {"state", s.col_text(1)},
                         {"outcome", s.col_text(2)}, {"thread_id", s.col_text(3)}, {"run_id", s.col_text(4)}};
    return j;
}

std::vector<std::string> Scheduler::available_kinds() {
    const std::lock_guard lock(mx_);
    std::vector<std::string> out = {"schedule", "file", "session"};
    for (const auto& [k, _] : sources_) out.push_back(k);
    return out;
}

// ---- Trigger definition and policy limits --------------------------------------------------------------------

Result<json> Scheduler::validate(const json& def, const std::string& except_id) {
    (void)except_id;
    if (!def.is_object()) return make_error("invalid_trigger", "a Trigger is an object");
    json out = json::object();
    const std::string name = jstr(def, "name");
    if (name.empty() || name.size() > 200) return make_error("invalid_trigger", "a Trigger needs a name of up to 200 characters");
    const std::string kind = jstr(def, "kind");
    if (!is_kind(kind)) return make_error("invalid_trigger", "the kind is one of schedule, file, mail, calendar, meeting, session");
    const auto avail = available_kinds();
    if (std::find(avail.begin(), avail.end(), kind) == avail.end()) return make_error("unavailable", kind + " source unavailable");
    const std::string prompt = jstr(def, "prompt");
    if (prompt.empty()) return make_error("invalid_trigger", "a Trigger needs a prompt");
    const std::string mode = jstr(def, "thread_mode", "new");
    if (mode != "new" && mode != "append") return make_error("invalid_trigger", "the thread mode is new or append");
    json cond = def.contains("conditions") && def["conditions"].is_object() ? def["conditions"] : json::object();

    if (kind == "schedule") {
        const bool has_cron = cond.contains("cron"), has_iv = cond.contains("interval_minutes");
        if (has_cron == has_iv) return make_error("invalid_trigger", "a schedule Trigger has either a cron expression or an interval");
        if (has_cron) {
            if (!cond["cron"].is_string()) return make_error("invalid_cron", "the cron expression is text");
            if (auto c = parse_cron(cond["cron"].get<std::string>()); !c) return c.error();
        } else if (!cond["interval_minutes"].is_number_integer() || cond["interval_minutes"].get<long long>() < 1) {
            return make_error("invalid_trigger", "the interval is a whole number of minutes");
        }
        if (jstr(cond, "time_zone").empty()) cond["time_zone"] = env_.local_zone ? env_.local_zone() : "UTC";
        if (auto z = zone_of(cond); !z) return z.error();
    } else if (kind == "file") {
        const std::string folder = jstr(cond, "folder");
        if (folder.empty()) return make_error("invalid_trigger", "a file Trigger needs a folder");
        if (env_.folder_allowed && !env_.folder_allowed(std::filesystem::path(folder)))
            return make_error("folder_not_allowed", "the folder is not readable under policy.filesystem.read");
        if (jstr(cond, "pattern").empty()) cond["pattern"] = "*";
    } else if (kind == "session") {
        const std::string ev = jstr(cond, "event");
        if (ev != "logon" && ev != "unlock") return make_error("invalid_trigger", "a session Trigger fires at logon or unlock");
    } else {
        const auto it = sources_.find(kind);
        if (it != sources_.end() && it->second.validate)
            if (auto s = it->second.validate(cond); !s) return s.error();
    }
    out["name"] = name;
    out["kind"] = kind;
    out["conditions"] = cond;
    out["prompt"] = prompt;
    out["skill"] = jstr(def, "skill");
    out["thread_mode"] = mode;
    out["enabled"] = !def.contains("enabled") || !def["enabled"].is_boolean() || def["enabled"].get<bool>();
    out["notify"] = !def.contains("notify") || !def["notify"].is_boolean() || def["notify"].get<bool>();
    return out;
}

std::optional<std::string> Scheduler::limit_violation(const TriggerRow& t, int index) {
    if (env_.policy) {
        if (const auto kinds = env_.policy("scheduler.enabled_kinds"); kinds && kinds->is_array()) {
            const bool listed = std::any_of(kinds->begin(), kinds->end(), [&](const json& k) { return k.is_string() && k.get<std::string>() == t.kind; });
            if (!listed) return "policy.scheduler.enabled_kinds does not allow " + t.kind + " Triggers";
        }
    }
    const long long max = policy_int("scheduler.max_triggers", 50);
    if (index >= max) return "more than " + std::to_string(max) + " Triggers (policy.scheduler.max_triggers)";
    if (t.kind == "schedule") {
        const long long min = policy_int("scheduler.min_interval_minutes", 15);
        std::optional<long long> gap;
        if (t.conditions.contains("interval_minutes")) {
            gap = jint(t.conditions, "interval_minutes") * kMinute;
        } else if (const auto c = parse_cron(jstr(t.conditions, "cron")); c) {
            if (auto z = zone_of(t.conditions); z) gap = shortest_gap_ms(*c, **z, now());
        }
        if (gap && *gap < min * kMinute)
            return "fires more often than every " + std::to_string(min) + " minutes (policy.scheduler.min_interval_minutes)";
    }
    return std::nullopt;
}

Result<json> Scheduler::create(const json& def, const std::string& created_by) {
    const std::lock_guard lock(mx_);
    auto v = validate(def, {});
    if (!v) return v.error();
    const long long t_now = now();
    TriggerRow t;
    t.id = new_id("trg_");
    t.name = (*v)["name"].get<std::string>();
    t.kind = (*v)["kind"].get<std::string>();
    t.conditions = (*v)["conditions"];
    t.prompt = (*v)["prompt"].get<std::string>();
    t.skill = (*v)["skill"].get<std::string>();
    t.thread_mode = (*v)["thread_mode"].get<std::string>();
    t.enabled = (*v)["enabled"].get<bool>();
    t.notify = (*v)["notify"].get<bool>();
    t.owner = env_.owner;
    t.created_by = created_by == "agent" ? "agent" : "user";
    t.created_at = t.updated_at = t_now;
    int count = 0;
    {
        auto s = env_.db->prepare("SELECT COUNT(*) FROM triggers");
        if (s.step()) count = static_cast<int>(s.col_int(0));
    }
    if (const auto why = limit_violation(t, count)) return make_error("policy_limit", *why);
    t.next_fire_at = next_after(t, t_now);
    auto s = env_.db->prepare(
        "INSERT INTO triggers(id, name, kind, conditions, prompt, skill, thread_mode, enabled, notify, owner, created_by, "
        "next_fire_at, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, t.id).bind(2, t.name).bind(3, t.kind).bind(4, t.conditions.dump()).bind(5, t.prompt).bind(6, t.skill);
    s.bind(7, t.thread_mode).bind(8, t.enabled ? 1 : 0).bind(9, t.notify ? 1 : 0).bind(10, t.owner).bind(11, t.created_by);
    if (t.next_fire_at) s.bind(12, *t.next_fire_at); else s.bind_null(12);
    s.bind(13, t_now).bind(14, t_now);
    if (auto r = s.run(); !r) return r.error();
    if (env_.changed) env_.changed();
    return to_json(t);
}

Result<json> Scheduler::update(const std::string& id, const json& patch) {
    const std::lock_guard lock(mx_);
    auto t = trigger(id);
    if (!t) return make_error("not_found", "no such Trigger");
    json def = {{"name", t->name}, {"kind", t->kind}, {"conditions", t->conditions}, {"prompt", t->prompt},
                {"skill", t->skill}, {"thread_mode", t->thread_mode}, {"enabled", t->enabled}, {"notify", t->notify}};
    if (patch.is_object())
        for (const char* k : {"name", "conditions", "prompt", "skill", "thread_mode", "enabled", "notify"})
            if (patch.contains(k)) def[k] = patch[k];
    auto v = validate(def, id);
    if (!v) return v.error();
    const bool timing = (*v)["conditions"] != t->conditions || (*v)["enabled"].get<bool>() != t->enabled;
    t->name = (*v)["name"].get<std::string>();
    t->conditions = (*v)["conditions"];
    t->prompt = (*v)["prompt"].get<std::string>();
    t->skill = (*v)["skill"].get<std::string>();
    t->thread_mode = (*v)["thread_mode"].get<std::string>();
    t->enabled = (*v)["enabled"].get<bool>();
    t->notify = (*v)["notify"].get<bool>();
    int index = 0;
    {
        auto s = env_.db->prepare("SELECT COUNT(*) FROM triggers WHERE created_at < ? OR (created_at = ? AND id < ?)");
        s.bind(1, t->created_at).bind(2, t->created_at).bind(3, t->id);
        if (s.step()) index = static_cast<int>(s.col_int(0));
    }
    if (const auto why = limit_violation(*t, index)) return make_error("policy_limit", *why);
    t->disabled_reason.clear();
    const long long t_now = now();
    if (timing || !t->next_fire_at) t->next_fire_at = next_after(*t, t_now);
    auto s = env_.db->prepare(
        "UPDATE triggers SET name=?, conditions=?, prompt=?, skill=?, thread_mode=?, enabled=?, notify=?, disabled_reason='', "
        "next_fire_at=?, updated_at=? WHERE id=?");
    s.bind(1, t->name).bind(2, t->conditions.dump()).bind(3, t->prompt).bind(4, t->skill).bind(5, t->thread_mode);
    s.bind(6, t->enabled ? 1 : 0).bind(7, t->notify ? 1 : 0);
    if (t->next_fire_at) s.bind(8, *t->next_fire_at); else s.bind_null(8);
    s.bind(9, t_now).bind(10, id);
    if (auto r = s.run(); !r) return r.error();
    if (env_.changed) env_.changed();
    return to_json(*t);
}

Status Scheduler::remove(const std::string& id) {
    const std::lock_guard lock(mx_);
    if (!trigger(id)) return make_error("not_found", "no such Trigger");
    auto st = env_.db->transaction([&]() -> Status {
        for (const char* sql : {"DELETE FROM occurrence_attempts WHERE occurrence_id IN "
                                "(SELECT id FROM occurrences WHERE trigger_id=? AND state<>'running')",
                                "DELETE FROM occurrences WHERE trigger_id=? AND state<>'running'",
                                "DELETE FROM trigger_files WHERE trigger_id=?", "DELETE FROM triggers WHERE id=?"}) {
            auto s = env_.db->prepare(sql);
            s.bind(1, id);
            if (auto r = s.run(); !r) return r;
        }
        return {};
    });
    if (st && env_.changed) env_.changed();
    return st;
}

json Scheduler::list() {
    const std::lock_guard lock(mx_);
    json out = json::array();
    for (const auto& t : triggers()) out.push_back(to_json(t));
    return out;
}

json Scheduler::history(const std::string& trigger_id, int limit) {
    const std::lock_guard lock(mx_);
    const auto t = trigger(trigger_id);
    json out = json::array();
    auto s = env_.db->prepare(
        "SELECT id, fire_at, reason, state, attempts, thread_id, run_id, outcome, started_at, ended_at FROM occurrences "
        "WHERE trigger_id=? ORDER BY fire_at DESC, created_at DESC LIMIT ?");
    s.bind(1, trigger_id).bind(2, limit);
    while (s.step()) {
        json o = {{"id", s.col_text(0)},     {"fire_at", s.col_int(1)}, {"reason", s.col_text(2)}, {"state", s.col_text(3)},
                  {"attempt_count", s.col_int(4)}, {"thread_id", s.col_text(5)}, {"run_id", s.col_text(6)},
                  {"outcome", s.col_text(7)}};
        if (t) o["fire_text"] = local_text(*t, s.col_int(1));
        if (s.col_int(8) > 0 && s.col_int(9) > 0) o["duration_ms"] = s.col_int(9) - s.col_int(8);
        json attempts = json::array();
        auto a = env_.db->prepare("SELECT n, run_id, started_at, ended_at, outcome, error FROM occurrence_attempts WHERE occurrence_id=? ORDER BY n");
        a.bind(1, s.col_text(0));
        while (a.step())
            attempts.push_back({{"n", a.col_int(0)}, {"run_id", a.col_text(1)}, {"started_at", a.col_int(2)},
                                {"ended_at", a.col_int(3)}, {"outcome", a.col_text(4)}, {"error", a.col_text(5)}});
        o["attempts"] = std::move(attempts);
        out.push_back(std::move(o));
    }
    return out;
}

Result<json> Scheduler::get(const std::string& id) {
    const std::lock_guard lock(mx_);
    const auto t = trigger(id);
    if (!t) return make_error("not_found", "no such Trigger");
    json j = to_json(*t);
    j["history"] = history(id);
    return j;
}

void Scheduler::policy_changed() {
    const std::lock_guard lock(mx_);
    int index = 0;
    bool any = false;
    for (auto& t : triggers()) {
        const std::string why = limit_violation(t, index++).value_or("");
        if (why == t.disabled_reason) continue;
        std::optional<long long> next = t.next_fire_at;
        // A Trigger enabled again starts from now; its firings while it was disabled are not caught up.
        if (why.empty()) next = next_after(t, now());
        auto s = env_.db->prepare("UPDATE triggers SET disabled_reason=?, next_fire_at=? WHERE id=?");
        s.bind(1, why);
        if (next) s.bind(2, *next); else s.bind_null(2);
        s.bind(3, t.id);
        (void)s.run();
        any = true;
    }
    if (any && env_.changed) env_.changed();
}

// ---- firing -----------------------------------------------------------------------------------------------------

bool Scheduler::add_occurrence(const std::string& trigger_id, const std::string& fire_key, const std::string& reason,
                               long long fire_at, const json& detail, const std::string& state) {
    const long long t_now = now();
    const std::string id = new_id("occ_");
    auto s = env_.db->prepare(
        "INSERT OR IGNORE INTO occurrences(id, trigger_id, fire_key, reason, fire_at, state, detail, outcome, created_at, "
        "next_attempt_at, ended_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, id).bind(2, trigger_id).bind(3, fire_key).bind(4, reason).bind(5, fire_at).bind(6, state).bind(7, detail.dump());
    s.bind(8, state == "skipped" ? "missed" : "").bind(9, t_now).bind(10, t_now).bind(11, state == "skipped" ? t_now : 0);
    if (!s.run() || env_.db->changes() == 0) return false;
    if (state != "skipped") emit(id);
    return true;
}

void Scheduler::fire_schedules(long long t_now) {
    // Firings while background work is paused are not made; on resume they are late and the catch-up rule applies.
    if (env_.paused && env_.paused()) return;
    const long long window = policy_int("scheduler.catch_up_window_hours", 12) * kHour;
    for (const auto& t : triggers()) {
        if (t.kind != "schedule" || !t.enabled || !t.disabled_reason.empty() || !t.next_fire_at || *t.next_fire_at > t_now) continue;
        // The due firings, from at most 30 days before the window (only the latest skipped ones are recorded).
        std::vector<long long> due;
        long long f = *t.next_fire_at;
        if (f < t_now - window - 30 * kDay)
            f = next_after(t, t_now - window - 30 * kDay).value_or(LLONG_MAX);
        while (f <= t_now) {
            due.push_back(f);
            f = next_after(t, f).value_or(LLONG_MAX);
        }
        size_t fired = due.size();  // index of the firing that runs, if any
        // A schedule Trigger keeps at most one pending Occurrence (add-offline-operation D5): while one waits, for
        // example for the connection, the due firings are recorded as skipped.
        bool waiting = false;
        {
            auto p = env_.db->prepare("SELECT 1 FROM occurrences WHERE trigger_id=? AND state='pending' LIMIT 1");
            p.bind(1, t.id);
            waiting = p.step();
        }
        if (!due.empty()) {
            const long long latest = due.back();
            if (waiting) {
                // every due firing is recorded as skipped below
            } else if (t_now - latest <= kLateGraceMs) {
                if (add_occurrence(t.id, std::to_string(latest), "scheduled", latest, json::object())) fired = due.size() - 1;
            } else if (latest >= t_now - window) {
                if (add_occurrence(t.id, std::to_string(latest), "catch-up", latest, json::object())) fired = due.size() - 1;
            }
            const size_t missed_end = fired < due.size() ? fired : due.size();
            const size_t from = missed_end > kMaxSkippedRecorded ? missed_end - kMaxSkippedRecorded : 0;
            for (size_t i = from; i < missed_end; ++i)
                (void)add_occurrence(t.id, std::to_string(due[i]), "scheduled", due[i], json::object(), "skipped");
        }
        auto s = env_.db->prepare("UPDATE triggers SET next_fire_at=? WHERE id=?");
        if (f == LLONG_MAX) s.bind_null(1); else s.bind(1, f);
        s.bind(2, t.id);
        (void)s.run();
        if (env_.changed) env_.changed();
    }
}

Result<json> Scheduler::run_now(const std::string& id) {
    const std::lock_guard lock(mx_);
    const auto t = trigger(id);
    if (!t) return make_error("not_found", "no such Trigger");
    if (!t->disabled_reason.empty()) return make_error("policy_limit", t->disabled_reason);
    const long long t_now = now();
    const std::string key = "manual:" + random_urlsafe(6);
    if (!add_occurrence(id, key, "manual", t_now, json::object())) return make_error("internal", "the Occurrence was not created");
    std::string occ;
    {
        auto s = env_.db->prepare("SELECT id FROM occurrences WHERE trigger_id=? AND fire_key=?");
        s.bind(1, id).bind(2, key);
        if (s.step()) occ = s.col_text(0);
    }
    start_ready(t_now);
    if (env_.changed) env_.changed();
    const auto o = occurrence(occ);
    return json{{"occurrence_id", occ}, {"state", o ? o->state : "pending"}, {"thread_id", o ? o->thread_id : ""},
                {"run_id", o ? o->run_id : ""}};
}

void Scheduler::session_event(const std::string& event, const std::string& key) {
    const std::lock_guard lock(mx_);
    for (const auto& t : triggers()) {
        if (t.kind != "session" || !t.enabled || !t.disabled_reason.empty() || jstr(t.conditions, "event") != event) continue;
        (void)add_occurrence(t.id, event + ":" + key, "event", now(), {{"event", event}});
    }
}

void Scheduler::file_seen(const std::string& trigger_id, const std::string& path, long long size, long long mtime) {
    const std::lock_guard lock(mx_);
    const auto t = trigger(trigger_id);
    if (!t || t->kind != "file") return;
    {
        auto s = env_.db->prepare("SELECT size, mtime FROM trigger_files WHERE trigger_id=? AND path=?");
        s.bind(1, trigger_id).bind(2, path);
        if (s.step() && s.col_int(0) == size && s.col_int(1) == mtime) return;
    }
    auto s = env_.db->prepare("INSERT OR REPLACE INTO trigger_files(trigger_id, path, size, mtime) VALUES(?,?,?,?)");
    s.bind(1, trigger_id).bind(2, path).bind(3, size).bind(4, mtime);
    (void)s.run();
    if (!t->enabled || !t->disabled_reason.empty()) return;
    (void)add_occurrence(trigger_id, path + "|" + std::to_string(size) + "|" + std::to_string(mtime), "event", now(), {{"path", path}});
}

void Scheduler::file_scan(const std::string& trigger_id, const std::vector<FileStat>& files) {
    const std::lock_guard lock(mx_);
    const auto t = trigger(trigger_id);
    if (!t || t->kind != "file") return;
    // The row with an empty path marks that the folder's existing files were recorded.
    bool baselined = false;
    {
        auto s = env_.db->prepare("SELECT 1 FROM trigger_files WHERE trigger_id=? AND path=''");
        s.bind(1, trigger_id);
        baselined = s.step();
    }
    if (!baselined) {
        (void)env_.db->transaction([&]() -> Status {
            auto m = env_.db->prepare("INSERT OR REPLACE INTO trigger_files(trigger_id, path, size, mtime) VALUES(?, '', 0, 0)");
            m.bind(1, trigger_id);
            if (auto r = m.run(); !r) return r;
            for (const auto& f : files) {
                auto s = env_.db->prepare("INSERT OR REPLACE INTO trigger_files(trigger_id, path, size, mtime) VALUES(?,?,?,?)");
                s.bind(1, trigger_id).bind(2, f.path).bind(3, f.size).bind(4, f.mtime);
                if (auto r = s.run(); !r) return r;
            }
            return {};
        });
        return;
    }
    for (const auto& f : files) file_seen(trigger_id, f.path, f.size, f.mtime);
    std::vector<std::string> known;
    {
        auto s = env_.db->prepare("SELECT path FROM trigger_files WHERE trigger_id=? AND path<>''");
        s.bind(1, trigger_id);
        while (s.step()) known.push_back(s.col_text(0));
    }
    for (const auto& p : known) {
        if (std::any_of(files.begin(), files.end(), [&](const FileStat& f) { return f.path == p; })) continue;
        auto s = env_.db->prepare("DELETE FROM trigger_files WHERE trigger_id=? AND path=?");
        s.bind(1, trigger_id).bind(2, p);
        (void)s.run();
    }
}

void Scheduler::register_source(const std::string& kind, Source s) {
    const std::lock_guard lock(mx_);
    sources_[kind] = std::move(s);
}

void Scheduler::unregister_source(const std::string& kind) {
    const std::lock_guard lock(mx_);
    sources_.erase(kind);
}

void Scheduler::source_event(const std::string& kind, const std::string& key, const json& detail,
                             const std::function<bool(const json&)>& matches) {
    const std::lock_guard lock(mx_);
    for (const auto& t : triggers()) {
        if (t.kind != kind || !t.enabled || !t.disabled_reason.empty() || (matches && !matches(t.conditions))) continue;
        (void)add_occurrence(t.id, key, "event", now(), detail);
    }
}

json Scheduler::file_triggers() {
    const std::lock_guard lock(mx_);
    json out = json::array();
    for (const auto& t : triggers())
        if (t.kind == "file" && t.enabled && t.disabled_reason.empty())
            out.push_back({{"id", t.id}, {"folder", jstr(t.conditions, "folder")}, {"pattern", jstr(t.conditions, "pattern", "*")}});
    return out;
}

// ---- Remote tasks ------------------------------------------------------------------------------------------------

Result<std::string> Scheduler::remote_occurrence(const std::string& task_id, const std::string& thread_id, const std::string& text,
                                                 const std::string& issuer_id, const std::string& preamble) {
    const std::lock_guard lock(mx_);
    const long long t_now = now();
    auto s = env_.db->prepare(
        "INSERT OR IGNORE INTO occurrences(id, remote_task_id, fire_key, reason, fire_at, state, detail, thread_id, created_at, "
        "next_attempt_at) VALUES(?,?,'remote','remote',?,'pending',?,?,?,?)");
    s.bind(1, new_id("occ_")).bind(2, task_id).bind(3, t_now);
    s.bind(4, json{{"text", text}, {"issuer_id", issuer_id}, {"preamble", preamble}}.dump()).bind(5, thread_id);
    s.bind(6, t_now).bind(7, t_now);
    if (auto r = s.run(); !r) return r.error();
    auto q = env_.db->prepare("SELECT id FROM occurrences WHERE remote_task_id=? AND trigger_id=''");
    q.bind(1, task_id);
    if (!q.step()) return make_error("internal", "the Occurrence was not created");
    std::string id = q.col_text(0);
    emit(id);
    start_ready(t_now);
    return id;
}

void Scheduler::cancel_remote(const std::string& task_id, const std::string& reason) {
    const std::lock_guard lock(mx_);
    auto q = env_.db->prepare("SELECT id, state, run_id FROM occurrences WHERE remote_task_id=? AND trigger_id=''");
    q.bind(1, task_id);
    if (!q.step()) return;
    const std::string id = q.col_text(0), state = q.col_text(1), run = q.col_text(2);
    if (state != "pending" && state != "running") return;
    finish(id, "cancelled", reason, now());
    if (!run.empty() && env_.cancel_run) (void)env_.cancel_run(run, reason);
}

std::optional<OccurrenceInfo> Scheduler::occurrence(const std::string& id) {
    const std::lock_guard lock(mx_);
    auto s = env_.db->prepare(
        "SELECT id, trigger_id, remote_task_id, state, reason, thread_id, run_id, outcome, attempts FROM occurrences WHERE id=?");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return OccurrenceInfo{s.col_text(0), s.col_text(1), s.col_text(2), s.col_text(3), s.col_text(4),
                          s.col_text(5), s.col_text(6), s.col_text(7), static_cast<int>(s.col_int(8))};
}

void Scheduler::on_occurrence(std::function<void(const OccurrenceInfo&)> listener) {
    const std::lock_guard lock(mx_);
    listeners_.push_back(std::move(listener));
}

void Scheduler::emit(const std::string& occurrence_id) {
    if (listeners_.empty()) return;
    const auto o = occurrence(occurrence_id);
    if (!o) return;
    for (const auto& l : listeners_) l(*o);
}

// ---- execution ---------------------------------------------------------------------------------------------------

std::string Scheduler::run_text(const TriggerRow& t, const json& detail) {
    std::string text = t.skill.empty() ? t.prompt : "/" + t.skill + " " + t.prompt;
    if (const std::string path = jstr(detail, "path"); !path.empty()) {
        text += "\n\nFile: " + path;
    } else if (detail.is_object() && !detail.empty() && !detail.contains("event")) {
        text += "\n\nTrigger event: " + detail.dump();
    }
    return text;
}

int Scheduler::active_runs() {
    int n = 0;
    std::vector<std::string> runs;
    {
        auto s = env_.db->prepare("SELECT run_id FROM occurrences WHERE state='running'");
        while (s.step()) runs.push_back(s.col_text(0));
    }
    for (const auto& r : runs) {
        if (r.empty() || !env_.run_state) { ++n; continue; }
        const auto st = env_.run_state(r);
        // An interrupted Run releases its slot.
        if (st && (st->state == runtime::RunState::queued || st->state == runtime::RunState::running)) ++n;
    }
    return n;
}

void Scheduler::start_ready(long long t_now) {
    if (env_.paused && env_.paused()) return;
    const int limit = slots();
    int active = active_runs();
    std::vector<std::string> ready;
    {
        auto s = env_.db->prepare("SELECT id FROM occurrences WHERE state='pending' AND next_attempt_at<=? ORDER BY fire_at, created_at");
        s.bind(1, t_now);
        while (s.step()) ready.push_back(s.col_text(0));
    }
    for (const auto& id : ready) {
        if (active >= limit) break;
        if (begin_attempt(id, t_now)) ++active;
    }
}

bool Scheduler::begin_attempt(const std::string& occurrence_id, long long t_now) {
    std::string trigger_id, task_id, thread, detail_text;
    long long fire_at = 0;
    int attempts = 0;
    {
        auto s = env_.db->prepare("SELECT trigger_id, remote_task_id, thread_id, detail, fire_at, attempts FROM occurrences WHERE id=? AND state='pending'");
        s.bind(1, occurrence_id);
        if (!s.step()) return false;
        trigger_id = s.col_text(0);
        task_id = s.col_text(1);
        thread = s.col_text(2);
        detail_text = s.col_text(3);
        fire_at = s.col_int(4);
        attempts = static_cast<int>(s.col_int(5));
    }
    const json detail = parse_json(detail_text).value_or(json::object());
    std::optional<TriggerRow> t;
    if (!trigger_id.empty()) {
        t = trigger(trigger_id);
        if (!t) {
            finish(occurrence_id, "cancelled", "the Trigger was deleted", t_now);
            return false;
        }
    }
    const runtime::Origin origin{t ? runtime::OriginKind::trigger_occurrence : runtime::OriginKind::remote_task,
                                 t ? occurrence_id : task_id};

    // Idempotent Run creation: a Run of this origin that has not ended and belongs to no earlier attempt is adopted.
    std::string run_id;
    if (env_.find_run) {
        if (const auto r = env_.find_run(origin)) {
            auto known = env_.db->prepare("SELECT 1 FROM occurrence_attempts WHERE occurrence_id=? AND run_id=?");
            known.bind(1, occurrence_id).bind(2, *r);
            const auto st = env_.run_state ? env_.run_state(*r) : std::nullopt;
            if (!known.step() && st && !runtime::is_terminal(st->state)) run_id = *r;
        }
    }

    if (t) {
        const json ref = {{"trigger_id", t->id}, {"occurrence_id", occurrence_id}, {"name", t->name},
                          {"occurrence_at", local_text(*t, fire_at)}, {"history_url", "#/automations/" + t->id}};
        if (thread.empty() || !env_.thread_exists || !env_.thread_exists(thread)) {
            thread.clear();
            if (t->thread_mode == "append" && !t->thread_id.empty() && env_.thread_exists && env_.thread_exists(t->thread_id))
                thread = t->thread_id;
            if (thread.empty()) {
                auto created = env_.create_thread(t->name, "trigger", ref);
                if (!created) {
                    attempt_failed(occurrence_id, created.error().message, t_now, false);
                    return false;
                }
                thread = *created;
                if (t->thread_mode == "append") {
                    auto s = env_.db->prepare("UPDATE triggers SET thread_id=? WHERE id=?");
                    s.bind(1, thread).bind(2, t->id);
                    (void)s.run();
                }
            }
        }
        if (env_.set_origin) env_.set_origin(thread, "trigger", ref);
    } else if (thread.empty() || (env_.thread_exists && !env_.thread_exists(thread))) {
        finish(occurrence_id, "cancelled", "the Thread was deleted", t_now);
        return false;
    }

    if (run_id.empty()) {
        const std::string text = t ? run_text(*t, detail) : jstr(detail, "text");
        runtime::StartOptions options;
        options.origin = origin;
        options.unattended = true;
        if (!t) {  // a Remote task's request is untrusted content from its issuer
            options.untrusted = runtime::Provenance{"remote_task", jstr(detail, "issuer_id"), "", false};
            options.preamble = jstr(detail, "preamble");
        }
        auto r = env_.start_run(thread, text, options);
        if (!r) {
            const std::string& code = r.error().code;
            // Offline: the Occurrence waits for the connection without an attempt (add-offline-operation D5).
            if (code == "thread_busy" || code == "not_started" || code == "stopping" || code == "offline") {
                auto s = env_.db->prepare("UPDATE occurrences SET thread_id=?, next_attempt_at=? WHERE id=?");
                s.bind(1, thread).bind(2, t_now + (code == "offline" ? kOfflineRetryMs : kBusyRetryMs)).bind(3, occurrence_id);
                (void)s.run();
                return false;
            }
            // Counted as an attempt that failed at once.
            auto s = env_.db->prepare("UPDATE occurrences SET state='running', attempts=attempts+1, thread_id=? WHERE id=?");
            s.bind(1, thread).bind(2, occurrence_id);
            (void)s.run();
            auto a = env_.db->prepare("INSERT OR REPLACE INTO occurrence_attempts(occurrence_id, n, started_at) VALUES(?,?,?)");
            a.bind(1, occurrence_id).bind(2, attempts + 1).bind(3, t_now);
            (void)a.run();
            attempt_failed(occurrence_id, r.error().message.empty() ? code : r.error().message, t_now, false);
            return false;
        }
        run_id = *r;
    }

    (void)env_.db->transaction([&]() -> Status {
        auto s = env_.db->prepare(
            "UPDATE occurrences SET state='running', attempts=attempts+1, thread_id=?, run_id=?, lease_owner=?, lease_until=?, "
            "started_at=CASE WHEN started_at=0 THEN ? ELSE started_at END WHERE id=? AND state='pending'");
        s.bind(1, thread).bind(2, run_id).bind(3, env_.installation_id).bind(4, t_now + kLeaseMs).bind(5, t_now).bind(6, occurrence_id);
        if (auto r = s.run(); !r) return r;
        auto a = env_.db->prepare("INSERT OR REPLACE INTO occurrence_attempts(occurrence_id, n, run_id, started_at) VALUES(?,?,?,?)");
        a.bind(1, occurrence_id).bind(2, attempts + 1).bind(3, run_id).bind(4, t_now);
        return a.run();
    });
    emit(occurrence_id);
    if (env_.changed) env_.changed();
    return true;
}

void Scheduler::attempt_failed(const std::string& occurrence_id, const std::string& error, long long t_now, bool lease_expired) {
    int attempts = 0;
    std::string trigger_id, thread;
    {
        auto s = env_.db->prepare("SELECT attempts, trigger_id, thread_id FROM occurrences WHERE id=?");
        s.bind(1, occurrence_id);
        if (!s.step()) return;
        attempts = static_cast<int>(s.col_int(0));
        trigger_id = s.col_text(1);
        thread = s.col_text(2);
    }
    const std::string why = lease_expired && error.empty() ? "the lease expired" : error;
    {
        auto a = env_.db->prepare("UPDATE occurrence_attempts SET ended_at=?, outcome='failed', error=? WHERE occurrence_id=? AND n=?");
        a.bind(1, t_now).bind(2, why).bind(3, occurrence_id).bind(4, attempts);
        (void)a.run();
    }
    if (attempts >= kMaxAttempts) {
        auto s = env_.db->prepare(
            "UPDATE occurrences SET state='dead', outcome=?, ended_at=?, lease_owner='', lease_until=0 WHERE id=?");
        s.bind(1, why).bind(2, t_now).bind(3, occurrence_id);
        (void)s.run();
        if (const auto t = trigger_id.empty() ? std::nullopt : trigger(trigger_id); t && t->notify && env_.notify)
            env_.notify("failed", why, thread);
    } else {
        auto s = env_.db->prepare(
            "UPDATE occurrences SET state='pending', outcome=?, run_id='', next_attempt_at=?, lease_owner='', lease_until=0 WHERE id=?");
        s.bind(1, why).bind(2, t_now + backoff_ms(attempts)).bind(3, occurrence_id);
        (void)s.run();
    }
    emit(occurrence_id);
    if (env_.changed) env_.changed();
}

void Scheduler::finish(const std::string& occurrence_id, const std::string& state, const std::string& outcome, long long t_now) {
    int attempts = 0;
    std::string trigger_id, thread;
    {
        auto s = env_.db->prepare("SELECT attempts, trigger_id, thread_id FROM occurrences WHERE id=?");
        s.bind(1, occurrence_id);
        if (!s.step()) return;
        attempts = static_cast<int>(s.col_int(0));
        trigger_id = s.col_text(1);
        thread = s.col_text(2);
    }
    auto s = env_.db->prepare("UPDATE occurrences SET state=?, outcome=?, ended_at=?, lease_owner='', lease_until=0 WHERE id=?");
    s.bind(1, state).bind(2, outcome).bind(3, t_now).bind(4, occurrence_id);
    (void)s.run();
    if (attempts > 0) {
        auto a = env_.db->prepare("UPDATE occurrence_attempts SET ended_at=?, outcome=?, error=? WHERE occurrence_id=? AND n=? AND ended_at=0");
        a.bind(1, t_now).bind(2, state).bind(3, state == "succeeded" ? "" : outcome).bind(4, occurrence_id).bind(5, attempts);
        (void)a.run();
    }
    if (state == "succeeded" && !trigger_id.empty() && env_.notify)
        if (const auto t = trigger(trigger_id); t && t->notify) env_.notify("completed", "", thread);
    emit(occurrence_id);
    if (env_.changed) env_.changed();
}

void Scheduler::poll_running(long long t_now, bool at_start) {
    struct Row {
        std::string id, run;
        long long lease_until;
    };
    std::vector<Row> rows;
    {
        auto s = env_.db->prepare("SELECT id, run_id, lease_until FROM occurrences WHERE state='running'");
        while (s.step()) rows.push_back({s.col_text(0), s.col_text(1), s.col_int(2)});
    }
    for (const auto& r : rows) {
        const auto st = r.run.empty() || !env_.run_state ? std::nullopt : env_.run_state(r.run);
        if (!st) {
            attempt_failed(r.id, "the Run is missing", t_now, false);
            continue;
        }
        switch (st->state) {
            case runtime::RunState::running:
                // After a restart, an expired lease on a Run still marked running means its process ended with it.
                if (at_start && r.lease_until < t_now) {
                    attempt_failed(r.id, "interrupted by shutdown", t_now, true);
                    break;
                }
                [[fallthrough]];
            case runtime::RunState::queued:
            case runtime::RunState::interrupted: {
                // Still ours: the lease is renewed (an interrupted Run keeps its Occurrence).
                auto s = env_.db->prepare("UPDATE occurrences SET lease_owner=?, lease_until=? WHERE id=?");
                s.bind(1, env_.installation_id).bind(2, t_now + kLeaseMs).bind(3, r.id);
                (void)s.run();
                break;
            }
            case runtime::RunState::succeeded: finish(r.id, "succeeded", "succeeded", t_now); break;
            case runtime::RunState::cancelled:
                finish(r.id, "cancelled", st->reason.empty() || st->reason == "cancelled by user" ? "stopped by user" : st->reason, t_now);
                break;
            case runtime::RunState::failed: attempt_failed(r.id, st->reason.empty() ? "the Run failed" : st->reason, t_now, false); break;
        }
    }
}

void Scheduler::prune(long long t_now) {
    const long long cutoff = t_now - policy_int("scheduler.history_days", 90) * kDay;
    for (const char* sql : {"DELETE FROM occurrence_attempts WHERE occurrence_id IN (SELECT id FROM occurrences WHERE "
                            "state NOT IN ('pending','running') AND fire_at < ?)",
                            "DELETE FROM occurrences WHERE state NOT IN ('pending','running') AND fire_at < ?"}) {
        auto s = env_.db->prepare(sql);
        s.bind(1, cutoff);
        (void)s.run();
    }
    last_prune_ = t_now;
}

// ---- driving -----------------------------------------------------------------------------------------------------

void Scheduler::start() {
    const std::lock_guard lock(mx_);
    const long long t_now = now();
    poll_running(t_now, true);
    // Schedule Triggers without a next firing (new zone data, a policy change while stopped) get one from now.
    for (const auto& t : triggers()) {
        if (t.kind != "schedule" || t.next_fire_at) continue;
        if (const auto n = next_after(t, t_now)) {
            auto s = env_.db->prepare("UPDATE triggers SET next_fire_at=? WHERE id=?");
            s.bind(1, *n).bind(2, t.id);
            (void)s.run();
        }
    }
    policy_changed();
    prune(t_now);
}

void Scheduler::tick() {
    const std::lock_guard lock(mx_);
    const long long t_now = now();
    fire_schedules(t_now);
    poll_running(t_now, false);
    start_ready(t_now);
    if (t_now - last_prune_ > kHour) prune(t_now);
}

}  // namespace wb::scheduler
