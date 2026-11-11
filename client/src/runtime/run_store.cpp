#include "runtime/run_store.h"

#include "platform/crypto.h"

#include <map>
#include <mutex>
#include <set>

namespace wb::runtime {

// ---- enums ------------------------------------------------------------------------------------------------------

const char* to_string(RunState s) {
    switch (s) {
    case RunState::queued: return "queued";
    case RunState::running: return "running";
    case RunState::interrupted: return "interrupted";
    case RunState::succeeded: return "succeeded";
    case RunState::failed: return "failed";
    case RunState::cancelled: return "cancelled";
    }
    return "failed";
}

std::optional<RunState> parse_run_state(std::string_view s) {
    for (const RunState st : {RunState::queued, RunState::running, RunState::interrupted, RunState::succeeded, RunState::failed,
                              RunState::cancelled})
        if (s == to_string(st)) return st;
    return std::nullopt;
}

bool is_terminal(RunState s) { return s == RunState::succeeded || s == RunState::failed || s == RunState::cancelled; }

bool transition_allowed(RunState from, RunState to) {
    switch (from) {
    case RunState::queued: return to == RunState::running || to == RunState::failed || to == RunState::cancelled;
    case RunState::running:
        return to == RunState::interrupted || to == RunState::succeeded || to == RunState::failed || to == RunState::cancelled;
    case RunState::interrupted: return to == RunState::running || to == RunState::cancelled;
    case RunState::succeeded:
    case RunState::failed:
    case RunState::cancelled: return false;
    }
    return false;
}

const char* to_string(Risk r) {
    switch (r) {
    case Risk::read: return "read";
    case Risk::write_local: return "write-local";
    case Risk::external: return "external";
    case Risk::destructive: return "destructive";
    }
    return "destructive";
}

std::optional<Risk> parse_risk(std::string_view s) {
    for (const Risk r : {Risk::read, Risk::write_local, Risk::external, Risk::destructive})
        if (s == to_string(r)) return r;
    return std::nullopt;
}

const char* to_string(OriginKind k) {
    switch (k) {
    case OriginKind::user_message: return "user_message";
    case OriginKind::trigger_occurrence: return "trigger_occurrence";
    case OriginKind::remote_task: return "remote_task";
    }
    return "user_message";
}

std::optional<OriginKind> parse_origin(std::string_view s) {
    for (const OriginKind k : {OriginKind::user_message, OriginKind::trigger_occurrence, OriginKind::remote_task})
        if (s == to_string(k)) return k;
    return std::nullopt;
}

namespace {

std::string new_id(const char* prefix) { return std::string(prefix) + random_urlsafe(9); }

constexpr const char* kRunColumns =
    "id,thread_id,parent_run_id,state,reason,origin_kind,origin_ref,unattended,queue_seq,role,active_ms,model_calls,tool_calls,"
    "prompt_tokens,completion_tokens,reasoning_tokens,created_at,started_at,ended_at";

ActionItem read_action(Statement& st) {
    ActionItem a;
    a.id = st.col_text(0);
    a.thread_id = st.col_text(1);
    a.run_id = st.col_text(2);
    a.kind = st.col_text(3);
    a.call_id = st.col_text(4);
    a.payload = json::parse(st.col_text(5), nullptr, false);
    a.created_at = st.col_text(6);
    a.resolved_at = st.col_text(7);
    a.resolution = json::parse(st.col_text(8), nullptr, false);
    a.raised_by = st.col_text(9);
    if (a.payload.is_discarded()) a.payload = nullptr;
    if (a.resolution.is_discarded()) a.resolution = nullptr;
    return a;
}

constexpr const char* kActionColumns =
    "id,thread_id,COALESCE(run_id,''),kind,call_id,payload,created_at,resolved_at,resolution,raised_by";

}  // namespace

// ---- Threads and Runs ----------------------------------------------------------------------------------------------

Result<std::string> RunStore::create_thread(const std::string& title) {
    const std::string id = new_id("thr_");
    auto st = db_.prepare("INSERT INTO threads(id,title,created_at,updated_at) VALUES(?1,?2,?3,?3)");
    st.bind(1, id).bind(2, title).bind(3, now());
    if (auto s = st.run(); !s) return s.error();
    return id;
}

std::vector<ThreadSummary> RunStore::list_threads(int limit) {
    const std::lock_guard lock(db_.mutex());
    std::vector<ThreadSummary> out;
    // The latest Run of the Thread that is not a subagent gives its state.
    auto st = db_.prepare(
        "SELECT t.id, t.title, t.created_at, t.updated_at, "
        "(SELECT r.state FROM runs r WHERE r.thread_id=t.id AND r.parent_run_id IS NULL ORDER BY r.queue_seq DESC LIMIT 1), "
        "(SELECT COUNT(*) FROM action_items a WHERE a.thread_id=t.id AND a.resolved_at='') "
        "FROM threads t ORDER BY t.updated_at DESC, t.id DESC LIMIT ?1");
    st.bind(1, static_cast<long long>(limit));
    while (st.step()) {
        ThreadSummary t;
        t.id = st.col_text(0);
        t.title = st.col_text(1);
        t.created_at = st.col_text(2);
        t.updated_at = st.col_text(3);
        t.last_run_state = st.col_text(4);
        t.open_actions = static_cast<int>(st.col_int(5));
        out.push_back(std::move(t));
    }
    return out;
}

bool RunStore::thread_exists(const std::string& id) {
    auto st = db_.prepare("SELECT 1 FROM threads WHERE id=?1");
    st.bind(1, id);
    return st.step();
}

Status RunStore::set_thread_title(const std::string& id, const std::string& title) {
    auto st = db_.prepare("UPDATE threads SET title=?2, updated_at=?3 WHERE id=?1");
    st.bind(1, id).bind(2, title).bind(3, now());
    return st.run();
}

std::optional<RunRecord> RunStore::read_run(Statement& st) {
    RunRecord r;
    r.id = st.col_text(0);
    r.thread_id = st.col_text(1);
    r.parent_run_id = st.col_text(2);
    r.state = parse_run_state(st.col_text(3)).value_or(RunState::failed);
    r.reason = st.col_text(4);
    r.origin.kind = parse_origin(st.col_text(5)).value_or(OriginKind::user_message);
    r.origin.ref = st.col_text(6);
    r.unattended = st.col_int(7) != 0;
    r.queue_seq = st.col_int(8);
    r.role = st.col_text(9);
    r.active_ms = st.col_int(10);
    r.model_calls = static_cast<int>(st.col_int(11));
    r.tool_calls = static_cast<int>(st.col_int(12));
    r.usage.prompt = st.col_int(13);
    r.usage.completion = st.col_int(14);
    r.usage.reasoning = st.col_int(15);
    r.usage.total = r.usage.prompt + r.usage.completion;
    r.usage.requests = r.model_calls;
    r.created_at = st.col_text(16);
    r.started_at = st.col_text(17);
    r.ended_at = st.col_text(18);
    return r;
}

Result<RunRecord> RunStore::create_run(const std::string& thread_id, const Origin& origin, bool unattended,
                                       const std::string& parent_run_id, const std::string& role) {
    const std::lock_guard lock(db_.mutex());
    if (!thread_exists(thread_id)) return make_error("not_found", "no such Thread");
    const std::string id = new_id("run_");
    const auto status = db_.transaction([&]() -> Status {
        long long seq = 1;
        {
            auto q = db_.prepare("SELECT COALESCE(MAX(queue_seq),0)+1 FROM runs");
            if (q.step()) seq = q.col_int(0);
        }
        auto st = db_.prepare(
            "INSERT INTO runs(id,thread_id,parent_run_id,state,origin_kind,origin_ref,unattended,queue_seq,role,created_at) "
            "VALUES(?1,?2,?3,'queued',?4,?5,?6,?7,?8,?9)");
        st.bind(1, id).bind(2, thread_id);
        if (parent_run_id.empty()) st.bind_null(3);
        else st.bind(3, parent_run_id);
        st.bind(4, to_string(origin.kind)).bind(5, origin.ref).bind(6, unattended ? 1 : 0).bind(7, seq).bind(8, role).bind(9, now());
        if (auto s = st.run(); !s) return s;
        if (auto e = append_event_locked(id, "state", {{"state", "queued"}, {"reason", ""}}); !e) return e.error();
        auto up = db_.prepare("UPDATE threads SET updated_at=?2 WHERE id=?1");
        up.bind(1, thread_id).bind(2, now());
        return up.run();
    });
    if (!status) return status.error();
    return *run(id);
}

std::optional<RunRecord> RunStore::run(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kRunColumns + " FROM runs WHERE id=?1");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return read_run(st);
}

std::vector<RunRecord> RunStore::runs_in_thread(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    std::vector<RunRecord> out;
    auto st = db_.prepare(std::string("SELECT ") + kRunColumns + " FROM runs WHERE thread_id=?1 ORDER BY queue_seq");
    st.bind(1, thread_id);
    while (st.step()) out.push_back(*read_run(st));
    return out;
}

std::vector<RunRecord> RunStore::runs_in_state(RunState state) {
    const std::lock_guard lock(db_.mutex());
    std::vector<RunRecord> out;
    auto st = db_.prepare(std::string("SELECT ") + kRunColumns + " FROM runs WHERE state=?1 ORDER BY queue_seq");
    st.bind(1, to_string(state));
    while (st.step()) out.push_back(*read_run(st));
    return out;
}

std::optional<RunRecord> RunStore::run_by_origin(const Origin& origin) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kRunColumns +
                          " FROM runs WHERE origin_kind=?1 AND origin_ref=?2 AND parent_run_id IS NULL ORDER BY queue_seq DESC LIMIT 1");
    st.bind(1, to_string(origin.kind)).bind(2, origin.ref);
    if (!st.step()) return std::nullopt;
    return read_run(st);
}

std::optional<RunRecord> RunStore::active_run(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kRunColumns +
                          " FROM runs WHERE thread_id=?1 AND parent_run_id IS NULL AND state IN ('queued','running','interrupted') "
                          "ORDER BY queue_seq LIMIT 1");
    st.bind(1, thread_id);
    if (!st.step()) return std::nullopt;
    return read_run(st);
}

Result<long long> RunStore::transition(const std::string& run_id, RunState to, const std::string& reason) {
    const std::lock_guard lock(db_.mutex());
    long long seq = 0;
    const auto status = db_.transaction([&]() -> Status {
        auto cur = run(run_id);
        if (!cur) return make_error("not_found", "no such Run");
        if (!transition_allowed(cur->state, to))
            return make_error("illegal_transition",
                              std::string("a ") + to_string(cur->state) + " Run cannot become " + to_string(to));
        const std::string at = now();
        auto st = db_.prepare(
            "UPDATE runs SET state=?2, reason=?3, "
            "started_at=CASE WHEN ?2='running' AND started_at='' THEN ?4 ELSE started_at END, "
            "ended_at=CASE WHEN ?5 THEN ?4 ELSE ended_at END WHERE id=?1");
        st.bind(1, run_id).bind(2, to_string(to)).bind(3, reason).bind(4, at).bind(5, is_terminal(to) ? 1 : 0);
        if (auto s = st.run(); !s) return s;
        auto e = append_event_locked(run_id, "state", {{"state", to_string(to)}, {"reason", reason}});
        if (!e) return e.error();
        seq = *e;
        auto up = db_.prepare("UPDATE threads SET updated_at=?2 WHERE id=?1");
        up.bind(1, cur->thread_id).bind(2, at);
        return up.run();
    });
    if (!status) return status.error();
    return seq;
}

Result<long long> RunStore::reopen_failed(const std::string& run_id, const std::vector<std::pair<std::string, std::string>>& unfinished) {
    const std::lock_guard lock(db_.mutex());
    long long seq = 0;
    const auto status = db_.transaction([&]() -> Status {
        auto cur = run(run_id);
        if (!cur) return make_error("not_found", "no such Run");
        if (cur->state != RunState::failed) return make_error("not_failed", "only a failed Run can be retried");
        // A call that may have partly run gets a result instead of a second execution (Run Retry).
        for (const auto& [call_id, tool] : unfinished) {
            auto r = append_event_locked(run_id, "tool_result",
                                         {{"call_id", call_id}, {"tool", tool}, {"ok", false},
                                          {"content", "The call did not complete before the Run failed; it was not executed again."},
                                          {"provenance", {{"source_kind", "runtime"}, {"trusted", true}, {"at", now()}}}});
            if (!r) return r.error();
        }
        const std::string at = now();
        // failed -> queued is allowed only here (Run Lifecycle States). The Run keeps its place in the Thread.
        auto st = db_.prepare("UPDATE runs SET state='queued', reason='retry', ended_at='' WHERE id=?1");
        st.bind(1, run_id);
        if (auto s = st.run(); !s) return s;
        auto e = append_event_locked(run_id, "state", {{"state", "queued"}, {"reason", "retry"}});
        if (!e) return e.error();
        seq = *e;
        auto up = db_.prepare("UPDATE threads SET updated_at=?2 WHERE id=?1");
        up.bind(1, cur->thread_id).bind(2, at);
        return up.run();
    });
    if (!status) return status.error();
    return seq;
}

Result<RecoveryReport> RunStore::recover() {
    const std::lock_guard lock(db_.mutex());
    RecoveryReport report;
    for (const auto& r : runs_in_state(RunState::running)) {
        auto t = transition(r.id, RunState::failed, "interrupted by shutdown");
        if (!t) return t.error();
        // Action items of a Run that failed cannot be answered any more.
        for (const auto& a : actions_of_run(r.id))
            if (a.open()) resolve_action(a.id, {{"kind", "cancelled"}, {"reason", "interrupted by shutdown"}});
        report.failed_running.push_back(r.id);
    }
    // A subagent Run is queued only for a moment inside its parent's batch; left queued, its parent was running.
    for (const auto& r : runs_in_state(RunState::queued)) {
        if (r.parent_run_id.empty()) continue;
        if (auto t = transition(r.id, RunState::failed, "interrupted by shutdown"); !t) return t.error();
        report.failed_running.push_back(r.id);
    }
    report.queued = static_cast<int>(runs_in_state(RunState::queued).size());
    report.interrupted = static_cast<int>(runs_in_state(RunState::interrupted).size());
    return report;
}

// ---- Event log ----------------------------------------------------------------------------------------------------

Result<long long> RunStore::append_event_locked(const std::string& run_id, const std::string& kind, const json& payload) {
    long long seq = 1;
    {
        auto q = db_.prepare("SELECT COALESCE(MAX(seq),0)+1 FROM run_events WHERE run_id=?1");
        q.bind(1, run_id);
        if (q.step()) seq = q.col_int(0);
    }
    auto st = db_.prepare("INSERT INTO run_events(run_id,seq,at,kind,payload) VALUES(?1,?2,?3,?4,?5)");
    st.bind(1, run_id).bind(2, seq).bind(3, now()).bind(4, kind).bind(5, payload.dump());
    if (auto s = st.run(); !s) return s.error();
    return seq;
}

Result<long long> RunStore::append_event(const std::string& run_id, const std::string& kind, const json& payload) {
    const std::lock_guard lock(db_.mutex());
    return append_event_locked(run_id, kind, payload);
}

std::vector<StoredEvent> RunStore::events(const std::string& run_id, long long after_seq) {
    const std::lock_guard lock(db_.mutex());
    std::vector<StoredEvent> out;
    auto st = db_.prepare("SELECT seq,at,kind,payload FROM run_events WHERE run_id=?1 AND seq>?2 ORDER BY seq");
    st.bind(1, run_id).bind(2, after_seq);
    while (st.step()) {
        StoredEvent e;
        e.seq = st.col_int(0);
        e.at = st.col_text(1);
        e.kind = st.col_text(2);
        e.payload = json::parse(st.col_text(3), nullptr, false);
        if (e.payload.is_discarded()) e.payload = nullptr;
        out.push_back(std::move(e));
    }
    return out;
}

// ---- Counters -----------------------------------------------------------------------------------------------------

Status RunStore::set_active_ms(const std::string& run_id, long long ms) {
    auto st = db_.prepare("UPDATE runs SET active_ms=?2 WHERE id=?1");
    st.bind(1, run_id).bind(2, ms);
    return st.run();
}

Status RunStore::add_usage(const std::string& run_id, const model::Usage& u) {
    auto st = db_.prepare(
        "UPDATE runs SET prompt_tokens=prompt_tokens+?2, completion_tokens=completion_tokens+?3, "
        "reasoning_tokens=reasoning_tokens+?4, model_calls=model_calls+?5 WHERE id=?1");
    st.bind(1, run_id).bind(2, u.prompt).bind(3, u.completion).bind(4, u.reasoning).bind(5, u.requests);
    return st.run();
}

Status RunStore::set_tool_calls(const std::string& run_id, int tool_calls) {
    auto st = db_.prepare("UPDATE runs SET tool_calls=?2 WHERE id=?1");
    st.bind(1, run_id).bind(2, tool_calls);
    return st.run();
}

// ---- Action items ----------------------------------------------------------------------------------------------------

Result<ActionItem> RunStore::create_action(const std::string& run_id, const std::string& kind, const std::string& call_id,
                                           const json& payload) {
    const std::lock_guard lock(db_.mutex());
    const auto r = run(run_id);
    if (!r) return make_error("not_found", "no such Run");
    const std::string id = new_id("act_");
    auto st = db_.prepare("INSERT INTO action_items(id,thread_id,run_id,kind,call_id,payload,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7)");
    st.bind(1, id).bind(2, r->thread_id).bind(3, run_id).bind(4, kind).bind(5, call_id).bind(6, payload.dump()).bind(7, now());
    if (auto s = st.run(); !s) return s.error();
    return *action(id);
}

Result<ActionItem> RunStore::create_standalone_action(const std::string& thread_id, const std::string& raised_by,
                                                      const json& payload) {
    const std::lock_guard lock(db_.mutex());
    if (!thread_exists(thread_id)) return make_error("not_found", "no such Thread");
    const std::string id = new_id("act_");
    auto st = db_.prepare(
        "INSERT INTO action_items(id,thread_id,run_id,kind,call_id,payload,created_at,raised_by) VALUES(?1,?2,NULL,'question','',?3,?4,?5)");
    st.bind(1, id).bind(2, thread_id).bind(3, payload.dump()).bind(4, now()).bind(5, raised_by);
    if (auto s = st.run(); !s) return s.error();
    return *action(id);
}

std::optional<ActionItem> RunStore::action(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kActionColumns + " FROM action_items WHERE id=?1");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return read_action(st);
}

std::vector<ActionItem> RunStore::actions_of_run(const std::string& run_id) {
    const std::lock_guard lock(db_.mutex());
    std::vector<ActionItem> out;
    auto st = db_.prepare(std::string("SELECT ") + kActionColumns + " FROM action_items WHERE run_id=?1 ORDER BY created_at, id");
    st.bind(1, run_id);
    while (st.step()) out.push_back(read_action(st));
    return out;
}

std::vector<ActionItem> RunStore::open_actions(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    std::vector<ActionItem> out;
    auto st = db_.prepare(std::string("SELECT ") + kActionColumns +
                          " FROM action_items WHERE resolved_at='' AND (?1='' OR thread_id=?1) ORDER BY created_at, id");
    st.bind(1, thread_id);
    while (st.step()) out.push_back(read_action(st));
    return out;
}

Status RunStore::resolve_action(const std::string& id, const json& resolution) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("UPDATE action_items SET resolved_at=?2, resolution=?3 WHERE id=?1 AND resolved_at=''");
    st.bind(1, id).bind(2, now()).bind(3, resolution.dump());
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error(action(id) ? "already_resolved" : "not_found", "the Action item is not open");
    return {};
}

// ---- Queued messages ---------------------------------------------------------------------------------------------------

Result<long long> RunStore::queue_message(const std::string& thread_id, const std::string& text, MessageMode mode) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("INSERT INTO queued_messages(thread_id,text,mode,created_at) VALUES(?1,?2,?3,?4)");
    st.bind(1, thread_id).bind(2, text).bind(3, mode == MessageMode::queue ? "queue" : "steer_now").bind(4, now());
    if (auto s = st.run(); !s) return s.error();
    return db_.last_insert_id();
}

std::vector<QueuedMessage> RunStore::pending_messages(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    std::vector<QueuedMessage> out;
    auto st = db_.prepare("SELECT id,thread_id,text,mode,created_at FROM queued_messages WHERE thread_id=?1 AND delivered_at='' ORDER BY id");
    st.bind(1, thread_id);
    while (st.step()) {
        QueuedMessage m;
        m.id = st.col_int(0);
        m.thread_id = st.col_text(1);
        m.text = st.col_text(2);
        m.mode = st.col_text(3) == "queue" ? MessageMode::queue : MessageMode::steer_now;
        m.created_at = st.col_text(4);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<std::string> RunStore::threads_with_pending_messages() {
    const std::lock_guard lock(db_.mutex());
    std::vector<std::string> out;
    auto st = db_.prepare("SELECT thread_id FROM queued_messages WHERE delivered_at='' GROUP BY thread_id ORDER BY MIN(id)");
    while (st.step()) out.push_back(st.col_text(0));
    return out;
}

std::string RunStore::message_thread(long long id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("SELECT thread_id FROM queued_messages WHERE id=?1");
    st.bind(1, id);
    return st.step() ? st.col_text(0) : std::string();
}

Status RunStore::edit_message(long long id, const std::string& text) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("UPDATE queued_messages SET text=?2 WHERE id=?1 AND delivered_at=''");
    st.bind(1, id).bind(2, text);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "the message was already delivered or removed");
    return {};
}

Status RunStore::remove_message(long long id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM queued_messages WHERE id=?1 AND delivered_at=''");
    st.bind(1, id);
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "the message was already delivered or removed");
    return {};
}

Status RunStore::mark_delivered(long long id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("UPDATE queued_messages SET delivered_at=?2 WHERE id=?1 AND delivered_at=''");
    st.bind(1, id).bind(2, now());
    if (auto s = st.run(); !s) return s;
    if (db_.changes() == 0) return make_error("not_found", "the message was already delivered or removed");
    return {};
}

// ---- Conversation -------------------------------------------------------------------------------------------------------

json message_to_event(const model::ChatMessage& m, const json& extra) {
    json j = {{"role", m.role}, {"content", m.content}};
    if (!m.tool_calls.empty()) {
        json calls = json::array();
        for (const auto& c : m.tool_calls) calls.push_back({{"id", c.id}, {"tool", c.tool}, {"arguments", c.arguments}});
        j["tool_calls"] = std::move(calls);
    }
    if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
    if (extra.is_object())
        for (const auto& [k, v] : extra.items()) j[k] = v;
    return j;
}

model::ChatMessage message_from_event(const json& p) {
    model::ChatMessage m;
    m.role = jstr(p, "role");
    m.content = jstr(p, "content");
    // Run Context: a Run's starting message carries the time it was written, always as the same first line.
    if (const std::string line = jstr(p, "time_line"); !line.empty() && m.role == "user") m.content = line + "\n" + m.content;
    m.tool_call_id = jstr(p, "tool_call_id");
    if (const json* calls = jobj(p, "tool_calls"); calls && calls->is_array())
        for (const auto& c : *calls) m.tool_calls.push_back({jstr(c, "id"), jstr(c, "tool"), jstr(c, "arguments", "{}")});
    return m;
}

std::vector<ContextMessage> RunStore::thread_messages(const std::string& thread_id, const std::string& except_run) {
    std::vector<ContextMessage> out;
    for (const auto& r : runs_in_thread(thread_id)) {
        if (!r.parent_run_id.empty()) continue;  // a subagent's messages belong to the child Run
        std::vector<ContextMessage> msgs;
        for (const auto& e : events(r.id))
            if (e.kind == "message") msgs.push_back({message_from_event(e.payload), r.id, e.seq, false});
        if (r.id != except_run) {
            // Close tool calls without a result.
            std::vector<ContextMessage> fixed;
            for (size_t i = 0; i < msgs.size(); ++i) {
                fixed.push_back(msgs[i]);
                if (msgs[i].msg.role != "assistant" || msgs[i].msg.tool_calls.empty()) continue;
                std::set<std::string> answered;
                size_t j = i + 1;
                for (; j < msgs.size() && msgs[j].msg.role == "tool"; ++j) {
                    answered.insert(msgs[j].msg.tool_call_id);
                    fixed.push_back(msgs[j]);
                }
                for (const auto& c : msgs[i].msg.tool_calls) {
                    if (answered.count(c.id)) continue;
                    ContextMessage m;
                    m.msg.role = "tool";
                    m.msg.tool_call_id = c.id;
                    m.msg.content = "The call was not executed: the Run ended before it finished.";
                    m.run_id = r.id;
                    m.seq = msgs[i].seq;
                    fixed.push_back(std::move(m));
                }
                i = j - 1;
            }
            msgs = std::move(fixed);
        }
        out.insert(out.end(), std::make_move_iterator(msgs.begin()), std::make_move_iterator(msgs.end()));
    }
    return out;
}

std::vector<model::ChatMessage> RunStore::conversation(const std::string& thread_id, const std::string& except_run) {
    const std::lock_guard lock(db_.mutex());
    std::vector<model::ChatMessage> out;
    for (auto& m : thread_messages(thread_id, except_run)) out.push_back(std::move(m.msg));
    return out;
}

ThreadContext RunStore::context(const std::string& thread_id, const std::string& except_run) {
    const std::lock_guard lock(db_.mutex());
    ThreadContext ctx;
    auto all = thread_messages(thread_id, except_run);
    const auto records = compactions(thread_id);
    if (records.empty()) {
        ctx.messages = std::move(all);
        return ctx;
    }
    const CompactionRecord& last = records.back();
    ctx.compaction_revision = last.id;
    // Order of the Runs in the Thread, to compare positions across Runs.
    std::map<std::string, size_t> order;
    for (const auto& r : runs_in_thread(thread_id)) order.emplace(r.id, order.size());
    const auto b = order.find(last.boundary_run);
    if (b == order.end()) {
        ctx.messages = std::move(all);  // a record that points nowhere is ignored
        return ctx;
    }
    ctx.summary = last.summary;
    ctx.summary_nonce = last.nonce;
    if (last.carried.is_array())
        for (const auto& c : last.carried)
            ctx.messages.push_back({message_from_event(c), jstr(c, "run_id"), jint(c, "seq"), true});
    bool kept = false;
    for (auto& m : all) {
        if (!kept) {
            const auto o = order.find(m.run_id);
            const size_t idx = o == order.end() ? 0 : o->second;
            kept = idx > b->second || (idx == b->second && m.seq >= last.boundary_seq);
            // A tool result whose call was compacted cannot stand alone (the boundary never falls there).
            if (!kept || m.msg.role == "tool") {
                kept = false;
                continue;
            }
        }
        ctx.messages.push_back(std::move(m));
    }
    return ctx;
}

// ---- Compaction records ------------------------------------------------------------------------------------------------

Result<long long> RunStore::add_compaction(const CompactionRecord& c, const ThreadContext& expected, const std::string& except_run,
                                           const std::function<bool()>& cancelled) {
    long long id = 0;
    auto result = db_.transaction([&]() -> Status {
        const auto current = context(c.thread_id, except_run);
        bool same = current.compaction_revision == expected.compaction_revision && current.summary == expected.summary &&
                    current.messages.size() == expected.messages.size();
        for (size_t i = 0; same && i < current.messages.size(); ++i) {
            const auto& a = current.messages[i];
            const auto& b = expected.messages[i];
            same = a.run_id == b.run_id && a.seq == b.seq && a.carried == b.carried &&
                   message_to_event(a.msg) == message_to_event(b.msg);
        }
        if (!same) return make_error("context_changed", "the conversation changed while it was being summarised");
        if (cancelled && cancelled()) return make_error("cancelled", "compaction was cancelled");
        auto st = db_.prepare(
            "INSERT INTO thread_compactions(thread_id,at,kind,run_id,summary,boundary_run,boundary_seq,carried,messages_compacted,"
            "tokens_before,tokens_after,nonce) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
        st.bind(1, c.thread_id).bind(2, now()).bind(3, c.kind).bind(4, c.run_id).bind(5, c.summary).bind(6, c.boundary_run);
        st.bind(7, c.boundary_seq).bind(8, c.carried.dump()).bind(9, c.messages_compacted).bind(10, c.tokens_before).bind(11, c.tokens_after).bind(12, c.nonce);
        if (auto s = st.run(); !s) return s;
        // Returning an error rolls the insertion back if cancellation arrived while the record was written.
        if (cancelled && cancelled()) return make_error("cancelled", "compaction was cancelled");
        id = db_.last_insert_id();
        return {};
    });
    if (!result) return result.error();
    return id;
}

std::vector<CompactionRecord> RunStore::compactions(const std::string& thread_id) {
    const std::lock_guard lock(db_.mutex());
    std::vector<CompactionRecord> out;
    auto st = db_.prepare(
        "SELECT id,thread_id,at,kind,run_id,summary,boundary_run,boundary_seq,carried,messages_compacted,tokens_before,tokens_after,nonce "
        "FROM thread_compactions WHERE thread_id=?1 ORDER BY id");
    st.bind(1, thread_id);
    while (st.step()) {
        CompactionRecord c;
        c.id = st.col_int(0);
        c.thread_id = st.col_text(1);
        c.at = st.col_text(2);
        c.kind = st.col_text(3);
        c.run_id = st.col_text(4);
        c.summary = st.col_text(5);
        c.boundary_run = st.col_text(6);
        c.boundary_seq = st.col_int(7);
        c.carried = json::parse(st.col_text(8), nullptr, false);
        if (!c.carried.is_array()) c.carried = json::array();
        c.messages_compacted = static_cast<int>(st.col_int(9));
        c.tokens_before = st.col_int(10);
        c.tokens_after = st.col_int(11);
        c.nonce = st.col_text(12);
        out.push_back(std::move(c));
    }
    return out;
}

std::string RunStore::ensure_compaction_nonce(long long id, const std::string& candidate) {
    const std::lock_guard lock(db_.mutex());
    auto up = db_.prepare("UPDATE thread_compactions SET nonce=?2 WHERE id=?1 AND nonce=''");
    up.bind(1, id).bind(2, candidate);
    (void)up.run();
    auto st = db_.prepare("SELECT nonce FROM thread_compactions WHERE id=?1");
    st.bind(1, id);
    return st.step() ? st.col_text(0) : candidate;
}

// ---- Thread attachments ------------------------------------------------------------------------------------------------

Result<std::string> RunStore::add_attachment(const std::string& thread_id, const std::string& run_id, const std::string& call_id,
                                             const std::string& name, std::string_view content, const json& provenance) {
    const std::lock_guard lock(db_.mutex());
    const std::string id = new_id("att_");
    auto st = db_.prepare(
        "INSERT INTO thread_attachments(id,thread_id,run_id,call_id,name,size,provenance,content,created_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    st.bind(1, id).bind(2, thread_id).bind(3, run_id).bind(4, call_id).bind(5, name).bind(6, static_cast<long long>(content.size()));
    st.bind(7, provenance.dump()).bind_blob(8, std::vector<std::uint8_t>(content.begin(), content.end())).bind(9, now());
    if (auto s = st.run(); !s) return s.error();
    return id;
}

std::optional<AttachmentInfo> RunStore::attachment(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("SELECT id,thread_id,run_id,call_id,name,size,provenance,created_at FROM thread_attachments WHERE id=?1");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    AttachmentInfo a;
    a.id = st.col_text(0);
    a.thread_id = st.col_text(1);
    a.run_id = st.col_text(2);
    a.call_id = st.col_text(3);
    a.name = st.col_text(4);
    a.size = st.col_int(5);
    a.provenance = json::parse(st.col_text(6), nullptr, false);
    if (a.provenance.is_discarded()) a.provenance = nullptr;
    a.created_at = st.col_text(7);
    return a;
}

Result<std::string> RunStore::read_attachment(const std::string& id, long long offset, long long length) {
    const std::lock_guard lock(db_.mutex());
    if (offset < 0 || length < 0) return make_error("invalid_range", "offset and length must not be negative");
    auto st = db_.prepare("SELECT substr(content, ?2, ?3) FROM thread_attachments WHERE id=?1");
    st.bind(1, id).bind(2, offset + 1).bind(3, length);
    if (!st.step()) return make_error("not_found", "no such attachment");
    const auto bytes = st.col_blob(0);
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace wb::runtime
