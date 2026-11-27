#include "eval/quality.h"

#include "eval/registry.h"
#include "storage/database.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

namespace wb::eval {
namespace {

constexpr long long kDayMs = 86'400'000LL;

const std::map<std::string, std::set<std::string>, std::less<>>& counters() {
    static const std::map<std::string, std::set<std::string>, std::less<>> all = {
        {"triage", {"suggestion_created", "dismissed", "no_action", "started", "untouched_7d", "batch_failed_unavailable",
                    "batch_failed_cut_off", "batch_failed_mismatch", "batch_failed_other"}},
        {"agenda", {"agenda_admitted", "agenda_done", "agenda_dismissed", "agenda_edited"}},
        {"knowledge", {"facts_stored", "fact_edited", "fact_deleted", "fact_marked"}},
        {"meeting", {"meetings_transcribed", "minutes_transcribed", "speakers_detected", "speakers_unnamed_7d",
                     "speakers_renamed", "terms_added", "transcribed_again", "unclear_permille"}},
    };
    return all;
}

bool known_category(std::string_view c) {
    static const std::set<std::string, std::less<>> all = {"request", "approval", "question", "meeting", "deadline",
                                                           "decision", "update", "none"};
    return all.count(c) > 0;
}

bool known_action(std::string_view a) { return a == "required" || a == "info" || a == "none"; }

bool instruction_id_ok(std::string_view id) {
    if (id == "unknown") return true;
    return id.size() == 12 && id.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

// A decision kind of the decision log as a counter.
std::pair<std::string, std::string> counter_of(const std::string& kind) {
    static const std::map<std::string, std::pair<std::string, std::string>> map = {
        {"dismiss", {"triage", "dismissed"}},
        {"no_action", {"triage", "no_action"}},
        {"start", {"triage", "started"}},
        {"agenda_done", {"agenda", "agenda_done"}},
        {"agenda_dismiss", {"agenda", "agenda_dismissed"}},
        {"agenda_edit", {"agenda", "agenda_edited"}},
        {"fact_edit", {"knowledge", "fact_edited"}},
        {"fact_delete", {"knowledge", "fact_deleted"}},
        {"importance", {"knowledge", "fact_marked"}},
        {"speaker_rename", {"meeting", "speakers_renamed"}},
        {"term_add", {"meeting", "terms_added"}},
        {"transcribe_again", {"meeting", "transcribed_again"}},
    };
    const auto it = map.find(kind);
    return it == map.end() ? std::pair<std::string, std::string>{} : it->second;
}

long long day_start(long long ms) { return ms - (ms % kDayMs + kDayMs) % kDayMs; }

}  // namespace

std::string utc_day(long long ms) {
    using namespace std::chrono;
    const sys_days d{days{day_start(ms) / kDayMs}};
    const year_month_day ymd{d};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()));
    return buf;
}

std::vector<telemetry::Field> quality_fields(const QualityRow& r) {
    return {{telemetry::Attr::quality_step, r.step},
            {telemetry::Attr::quality_counter, r.counter},
            {telemetry::Attr::quality_value, r.value},
            {telemetry::Attr::quality_instruction_id, r.instruction_id},
            {telemetry::Attr::quality_category, r.category},
            {telemetry::Attr::quality_action, r.action},
            {telemetry::Attr::quality_day, r.day}};
}

void QualityCounters::add(const std::string& day, std::string_view step, std::string_view counter, std::string_view instruction_id,
                          long long value, std::string_view category, std::string_view action) {
    const auto it = counters().find(step);
    if (it == counters().end() || !it->second.count(std::string(counter))) return;
    const std::string cat = known_category(category) ? std::string(category) : "none";
    const std::string act = known_action(action) ? std::string(action) : "none";
    const std::string id = instruction_id_ok(instruction_id) ? std::string(instruction_id) : "unknown";
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO quality_counts(day, step, counter, category, action, instruction_id, value) VALUES(?1,?2,?3,?4,?5,?6,?7) "
        "ON CONFLICT(day, step, counter, category, action, instruction_id) DO UPDATE SET value = value + excluded.value");
    (void)st.bind(1, day).bind(2, step).bind(3, counter).bind(4, cat).bind(5, act).bind(6, id).bind(7, value).run();
}

bool QualityCounters::mark(const std::string& kind, const std::string& ref) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("INSERT OR IGNORE INTO quality_marks(kind, ref) VALUES(?1, ?2)");
    if (!st.bind(1, kind).bind(2, ref).run()) return false;
    return db_.changes() > 0;
}

void QualityCounters::collect(long long now_ms) {
    // Marks that can no longer matter go (review minor 12): days older than the collection window, and items or
    // meetings that no longer exist.
    {
        const std::lock_guard lock(db_.mutex());
        auto old = db_.prepare("DELETE FROM quality_marks WHERE kind='collected' AND ref < ?1");
        (void)old.bind(1, utc_day(now_ms - 8 * kDayMs)).run();
        (void)db_.exec("DELETE FROM quality_marks WHERE kind='untouched' AND ref NOT IN (SELECT scope || '|' || item_id FROM triage_items)");
        (void)db_.exec("DELETE FROM quality_marks WHERE kind='unnamed' AND substr(ref, 1, instr(ref, '/') - 1) NOT IN (SELECT id FROM meetings)");
    }
    // Finished days within the last week that were not collected yet.
    const long long today = day_start(now_ms);
    for (long long d = today - 7 * kDayMs; d < today; d += kDayMs) {
        const std::string day = utc_day(d);
        if (mark("collected", day)) collect_day(day, d, d + kDayMs);
    }
    sweep(now_ms);
}

void QualityCounters::collect_day(const std::string& day, long long from_ms, long long to_ms) {
    struct Hit {
        std::string step, counter, id, category = "none", action = "none";
        long long value = 1;
    };
    std::vector<Hit> hits;
    {
        const std::lock_guard lock(db_.mutex());
        // Decisions from the decision log.
        auto q = db_.prepare("SELECT kind, instruction_id, before_json FROM decision_log WHERE at >= ?1 AND at < ?2");
        q.bind(1, from_ms).bind(2, to_ms);
        while (q.step()) {
            const auto [step, counter] = counter_of(q.col_text(0));
            if (step.empty()) continue;
            Hit h{step, counter, q.col_text(1)};
            if (step == "triage")
                if (const auto b = parse_json(q.col_text(2)); b && b->is_object()) h.category = jstr(*b, "category", "none"), h.action = jstr(*b, "action", "none");
            hits.push_back(std::move(h));
        }
        // Triage suggestions made that day.
        auto t = db_.prepare("SELECT category, action, instruction_id FROM triage_items "
                             "WHERE (suggestion IN ('open','started','dismissed','no_action','withdrawn') OR (suggestion='replied' AND thread_id<>'')) AND queued_at >= ?1 AND queued_at < ?2");
        t.bind(1, from_ms).bind(2, to_ms);
        while (t.step()) hits.push_back({"triage", "suggestion_created", t.col_text(2), t.col_text(0), t.col_text(1)});
        // Agenda items admitted that day.
        auto a = db_.prepare("SELECT instruction_id FROM agenda_items WHERE created_at >= ?1 AND created_at < ?2");
        a.bind(1, from_ms).bind(2, to_ms);
        while (a.step()) hits.push_back({"agenda", "agenda_admitted", a.col_text(0)});
        // Meetings transcribed that day.
        auto m = db_.prepare("SELECT started_at, ended_at, speakers, segments FROM meetings WHERE state='done' AND ended_at >= ?1 AND ended_at < ?2");
        m.bind(1, from_ms).bind(2, to_ms);
        const std::string diarize_id = Variant::defaults(Step::diarize).instruction_id();
        const std::string asr_id = Variant::defaults(Step::asr).instruction_id();
        long long meetings = 0, permille_sum = 0;
        while (m.step()) {
            ++meetings;
            const long long minutes = std::max(0LL, (m.col_int(1) - m.col_int(0)) / 60'000);
            hits.push_back({"meeting", "meetings_transcribed", asr_id});
            hits.push_back({"meeting", "minutes_transcribed", asr_id, "none", "none", minutes});
            long long speakers = 0;
            if (const auto sp = parse_json(m.col_text(2)); sp && sp->is_object())
                for (const auto& [k, v] : sp->items()) speakers += k == "me" ? 0 : 1;
            hits.push_back({"meeting", "speakers_detected", diarize_id, "none", "none", speakers});
            long long total = 0, unclear = 0;
            if (const auto seg = parse_json(m.col_text(3)); seg && seg->is_array())
                for (const auto& s : *seg) {
                    ++total;
                    if (s.is_object() && jbool(s, "unclear")) ++unclear;
                }
            permille_sum += total ? unclear * 1000 / total : 0;
        }
        if (meetings) hits.push_back({"meeting", "unclear_permille", asr_id, "none", "none", permille_sum / meetings});
    }
    for (const auto& h : hits) add(day, h.step, h.counter, h.id, h.value, h.category, h.action);
}

void QualityCounters::sweep(long long now_ms) {
    const std::string today = utc_day(now_ms);
    struct Open {
        std::string ref, category, action, id;
    };
    std::vector<Open> open;
    struct Speaker {
        std::string ref;
    };
    std::vector<Speaker> unnamed;
    {
        const std::lock_guard lock(db_.mutex());
        auto t = db_.prepare("SELECT scope, item_id, category, action, instruction_id FROM triage_items WHERE suggestion='open' AND queued_at < ?1");
        t.bind(1, now_ms - kWeekMs);
        while (t.step()) open.push_back({t.col_text(0) + "|" + t.col_text(1), t.col_text(2), t.col_text(3), t.col_text(4)});
        auto m = db_.prepare("SELECT id, speakers, speaker_meta FROM meetings WHERE state='done' AND ended_at > 0 AND ended_at < ?1");
        m.bind(1, now_ms - kWeekMs);
        while (m.step()) {
            const auto sp = parse_json(m.col_text(1));
            const auto meta = parse_json(m.col_text(2));
            if (!sp || !sp->is_object()) continue;
            for (const auto& [k, v] : sp->items()) {
                if (k == "me" || !v.is_string() || v.get<std::string>().rfind("Speaker ", 0) != 0) continue;
                const bool by_user = meta && meta->is_object() && meta->contains(k) && jstr((*meta)[k], "by") == "user";
                if (!by_user) unnamed.push_back({m.col_text(0) + "/" + k});
            }
        }
    }
    for (const auto& o : open)
        if (mark("untouched", o.ref)) add(today, "triage", "untouched_7d", o.id, 1, o.category, o.action);
    const std::string diarize_id = Variant::defaults(Step::diarize).instruction_id();
    for (const auto& s : unnamed)
        if (mark("unnamed", s.ref)) add(today, "meeting", "speakers_unnamed_7d", diarize_id);
}

int QualityCounters::flush(const std::function<void(const std::vector<telemetry::Field>&)>& emit, long long now_ms, bool send) {
    const std::string today = utc_day(now_ms);
    if (!send) {
        // Counts made while telemetry is off are never sent later (review minor 11).
        const std::lock_guard lock(db_.mutex());
        (void)db_.exec("DELETE FROM quality_counts");
        return 0;
    }
    std::vector<QualityRow> due;
    for (auto& r : rows())
        if (r.day < today) due.push_back(std::move(r));
    for (const auto& r : due) emit(quality_fields(r));
    const std::lock_guard lock(db_.mutex());
    auto d = db_.prepare("DELETE FROM quality_counts WHERE day < ?1");
    (void)d.bind(1, today).run();
    return static_cast<int>(due.size());
}

std::vector<QualityRow> QualityCounters::rows() const {
    const std::lock_guard lock(db_.mutex());
    auto q = db_.prepare("SELECT day, step, counter, category, action, instruction_id, value FROM quality_counts ORDER BY day, step, counter");
    std::vector<QualityRow> out;
    while (q.step()) out.push_back({q.col_text(0), q.col_text(1), q.col_text(2), q.col_text(3), q.col_text(4), q.col_text(5), q.col_int(6)});
    return out;
}

}  // namespace wb::eval
