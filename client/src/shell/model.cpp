#include "shell/model.h"

#include <cctype>

namespace wb::shell {

namespace {

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool digits(std::string_view s, size_t pos, size_t n, int& out) {
    if (pos + n > s.size()) return false;
    int v = 0;
    for (size_t i = pos; i < pos + n; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

std::string s(const json& j, const char* k) { return jstr(j, k); }

}  // namespace

std::optional<long long> iso_millis(std::string_view t) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!digits(t, 0, 4, y) || t.size() < 19 || t[4] != '-' || !digits(t, 5, 2, mo) || t[7] != '-' || !digits(t, 8, 2, d) ||
        (t[10] != 'T' && t[10] != ' ') || !digits(t, 11, 2, h) || t[13] != ':' || !digits(t, 14, 2, mi) || t[16] != ':' ||
        !digits(t, 17, 2, se))
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return std::nullopt;
    size_t i = 19;
    long long ms = 0;
    if (i < t.size() && t[i] == '.') {
        ++i;
        long long scale = 100;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
            ms += (t[i] - '0') * scale;
            scale /= 10;
            ++i;
        }
    }
    long long offset_min = 0;
    if (i < t.size()) {
        if (t[i] == 'Z' || t[i] == 'z') {
            ++i;
        } else if (t[i] == '+' || t[i] == '-') {
            int oh = 0, om = 0;
            if (!digits(t, i + 1, 2, oh)) return std::nullopt;
            size_t j = i + 3;
            if (j < t.size() && t[j] == ':') ++j;
            if (!digits(t, j, 2, om)) return std::nullopt;
            offset_min = (oh * 60 + om) * (t[i] == '-' ? -1 : 1);
            i = j + 2;
        }
    }
    if (i != t.size()) return std::nullopt;
    const long long days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ((days * 24 + h) * 60 + mi - offset_min) * 60000LL + se * 1000LL + ms;
}

ThreadEntry entry_from(const json& j) {
    ThreadEntry e;
    e.id = s(j, "id");
    e.title = s(j, "title");
    e.preview = s(j, "preview");
    e.last_activity = s(j, "last_activity");
    e.created_at = s(j, "created_at");
    if (const json* o = jobj(j, "origin")) {
        e.origin_kind = jstr(*o, "kind", "user");
        if (o->is_object() && o->contains("ref")) e.origin_ref = (*o)["ref"];
    }
    e.unread = jbool(j, "unread");
    e.archived = jbool(j, "archived");
    e.pinned = jbool(j, "pinned");
    e.folder_id = s(j, "folder_id");
    e.has_action = jbool(j, "has_action");
    e.running = jbool(j, "running");
    e.open_actions = jint(j, "open_actions");
    e.last_run_state = s(j, "last_run_state");
    e.kind = jstr(j, "kind", "thread");
    e.unread_count = jint(j, "unread_count");
    e.chat_kind = s(j, "chat_kind");
    e.presence = s(j, "presence");
    return e;
}

Counts counts_from(const json& j) { return {jint(j, "unread"), jint(j, "action"), jint(j, "running")}; }

RunInfo run_from(const json& j) {
    RunInfo r;
    r.id = s(j, "id");
    r.parent_run_id = s(j, "parent_run_id");
    r.state = s(j, "state");
    r.reason = s(j, "reason");
    r.role = s(j, "role");
    if (const json* o = jobj(j, "origin")) r.origin_kind = jstr(*o, "kind");
    r.unattended = jbool(j, "unattended");
    r.active_ms = jint(j, "active_ms");
    r.usage.calls = jint(j, "model_calls");
    if (const json* u = jobj(j, "usage"); u && u->is_object())
        r.usage = {jint(*u, "prompt"), jint(*u, "completion"), jint(*u, "reasoning"), r.usage.calls};
    r.created_at = s(j, "created_at");
    r.started_at = s(j, "started_at");
    r.ended_at = s(j, "ended_at");
    if (const json* ev = jobj(j, "events"); ev && ev->is_array())
        for (const json& e : *ev) {
            StoredEvent se;
            se.seq = jint(e, "seq");
            se.at = s(e, "at");
            se.kind = s(e, "kind");
            if (const json* p = jobj(e, "payload"); p && p->is_object()) se.payload = *p;
            r.events.push_back(std::move(se));
        }
    return r;
}

ActionItem action_from(const json& j) {
    ActionItem a;
    a.id = s(j, "id");
    a.thread_id = s(j, "thread_id");
    a.run_id = s(j, "run_id");
    a.kind = jstr(j, "kind") == "approval" ? "approval" : "question";
    a.call_id = s(j, "call_id");
    if (const json* p = jobj(j, "payload"); p && p->is_object()) a.payload = *p;
    a.created_at = s(j, "created_at");
    a.resolved_at = s(j, "resolved_at");
    if (j.is_object() && j.contains("resolution")) a.resolution = j["resolution"];
    a.raised_by = s(j, "raised_by");
    return a;
}

FeedSnapshot snapshot_from(const json& j) {
    FeedSnapshot f;
    if (const json* e = jobj(j, "entry")) f.entry = entry_from(*e);
    if (const json* a = jobj(j, "runs"); a && a->is_array())
        for (const json& r : *a) f.runs.push_back(run_from(r));
    if (const json* a = jobj(j, "subagents"); a && a->is_array())
        for (const json& r : *a) f.subagents.push_back(run_from(r));
    if (const json* a = jobj(j, "actions"); a && a->is_array())
        for (const json& x : *a) f.actions.push_back(action_from(x));
    if (const json* a = jobj(j, "queued"); a && a->is_array())
        for (const json& q : *a) f.queued.push_back({jint(q, "id"), s(q, "text"), s(q, "mode"), s(q, "created_at")});
    return f;
}

LiveRunEvent live_event_from(const json& j) {
    LiveRunEvent e;
    e.kind = s(j, "kind");
    e.run_id = s(j, "run_id");
    e.thread_id = s(j, "thread_id");
    e.seq = jint(j, "seq");
    e.at = s(j, "at");
    if (const json* d = jobj(j, "data"); d && d->is_object()) e.data = *d;
    return e;
}

}  // namespace wb::shell
