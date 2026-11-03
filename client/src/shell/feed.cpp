#include "shell/feed.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace wb::shell {

namespace {

std::string str(const json& j, const char* k) { return jstr(j, k); }

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

void fold_event(std::vector<FeedItem>& items, std::map<std::string, size_t>& tools,
                const std::map<std::string, const ActionItem*>& actions, const RunInfo& run, const StoredEvent& e) {
    const json& p = e.payload;
    const std::string key = run.id + ":" + std::to_string(e.seq);
    FeedItem it;
    it.key = key;
    it.run_id = run.id;
    it.seq = e.seq;
    it.at = e.at;
    if (e.kind == "message") {
        const std::string role = str(p, "role");
        it.text = str(p, "content");
        if (role == "user") {
            it.type = ItemType::user;
            items.push_back(std::move(it));
        } else if (role == "assistant" && !trim(it.text).empty()) {
            it.type = ItemType::assistant;
            items.push_back(std::move(it));
        }
    } else if (e.kind == "reasoning") {
        it.text = str(p, "text");
        if (it.text.empty()) return;
        it.type = ItemType::reasoning;
        items.push_back(std::move(it));
    } else if (e.kind == "tool_call") {
        it.type = ItemType::tool;
        it.call_id = str(p, "call_id");
        it.tool = str(p, "tool");
        if (p.is_object() && p.contains("args") && !p["args"].is_null()) it.args = p["args"];
        it.risk = str(p, "risk");
        it.decision = str(p, "decision");
        it.reason = str(p, "reason");
        it.rationale = str(p, "rationale");
        tools[it.call_id] = items.size();
        items.push_back(std::move(it));
    } else if (e.kind == "tool_result") {
        const auto t = tools.find(str(p, "call_id"));
        if (t != tools.end()) {
            ToolResult r{jbool(p, "ok"), str(p, "content"), {}, {}};
            if (r.ok) r.citations = citations_of(items[t->second].tool, r.content);
            if (const json* files = p.is_object() ? jobj(p, "files") : nullptr; files && files->is_array())
                for (const json& f : *files)
                    r.files.push_back({jstr(f, "id"), jstr(f, "name"), jstr(f, "mime"), jint(f, "size"), static_cast<int>(jint(f, "width")),
                                       static_cast<int>(jint(f, "height"))});
            items[t->second].result = std::move(r);
        }
    } else if (e.kind == "interrupt") {
        const auto a = actions.find(str(p, "action_id"));
        if (a == actions.end()) return;
        it.type = ItemType::action;
        it.action = *a->second;
        items.push_back(std::move(it));
    } else if (e.kind == "activity") {
        if (p.is_object() && p.contains("after")) {
            // The user edited the arguments before approving (Approval Card): the call card shows the edited ones.
            const json& after = p["after"];
            std::vector<size_t> order;
            for (const auto& [id, index] : tools) order.push_back(index);
            std::sort(order.rbegin(), order.rend());
            for (size_t index : order)
                if (items[index].decision == "approval") {
                    items[index].args = after;
                    items[index].edited = true;
                    break;
                }
        }
        it.type = ItemType::activity;
        it.text = str(p, "text");
        items.push_back(std::move(it));
    } else if (e.kind == "error") {
        it.type = ItemType::error;
        it.error = error_of(p);
        items.push_back(std::move(it));
    }
    // state, resolution, subagent, compaction: shown elsewhere
}

long long duration_of(const RunInfo& r, long long now) {
    if (r.active_ms > 0 && !is_active_state(r.state)) return r.active_ms;
    const auto start = iso_millis(r.started_at.empty() ? r.created_at : r.started_at);
    if (!start) return r.active_ms;
    const auto end = r.ended_at.empty() ? std::optional<long long>(now) : iso_millis(r.ended_at);
    return std::max(r.active_ms, end.value_or(now) - *start);
}

}  // namespace

bool is_active_state(const std::string& state) { return state == "queued" || state == "running"; }

std::vector<Citation> citations_of(const std::string& tool, const std::string& content) {
    std::vector<Citation> out;
    if (tool != "knowledge.search") return out;
    const json body = json::parse(content, nullptr, false);
    const json* results = body.is_object() ? jobj(body, "results") : nullptr;
    if (!results || !results->is_array()) return out;
    for (const json& r : *results) {
        if (!r.is_object() || jstr(r, "ref").empty()) continue;
        out.push_back({jint(r, "n"), jstr(r, "title"), jstr(r, "ref"), jstr(r, "source"), jstr(r, "date")});
    }
    return out;
}

const RunInfo* active_run(const FeedSnapshot& snap) {
    for (auto r = snap.runs.rbegin(); r != snap.runs.rend(); ++r)
        if (is_active_state(r->state) || r->state == "interrupted") return &*r;
    return nullptr;
}

const RunInfo* running_run(const FeedSnapshot& snap) {
    for (auto r = snap.runs.rbegin(); r != snap.runs.rend(); ++r)
        if (is_active_state(r->state)) return &*r;
    return nullptr;
}

RunError error_of(const json& p) {
    RunError e;
    e.code = str(p, "code");
    e.message = str(p, "message");
    e.reason = str(p, "reason");
    if (p.is_object() && p.contains("retry_after_s") && p["retry_after_s"].is_number())
        e.retry_after_s = jint(p, "retry_after_s");
    e.reset_at = str(p, "reset_at");
    return e;
}

std::optional<LatestError> latest_error(const FeedSnapshot& snap) {
    if (snap.runs.empty() || snap.runs.back().state != "failed") return std::nullopt;
    const RunInfo& last = snap.runs.back();
    for (auto e = last.events.rbegin(); e != last.events.rend(); ++e)
        if (e->kind == "error") return LatestError{last.id, error_of(e->payload)};
    return std::nullopt;
}

std::vector<FeedItem> build_feed(const FeedSnapshot& snap, const LiveText& live) {
    std::vector<FeedItem> items;
    std::map<std::string, const ActionItem*> actions;
    for (const ActionItem& a : snap.actions) actions[a.id] = &a;
    for (const RunInfo& run : snap.runs) {
        std::map<std::string, size_t> tools;
        for (const StoredEvent& e : run.events) fold_event(items, tools, actions, run, e);
        if (const auto r = live.reasoning.find(run.id); r != live.reasoning.end() && !r->second.empty()) {
            FeedItem it;
            it.type = ItemType::reasoning;
            it.key = run.id + ":live-r";
            it.run_id = run.id;
            it.text = r->second;
            it.streaming = true;
            items.push_back(std::move(it));
        }
        if (const auto t = live.text.find(run.id); t != live.text.end() && !t->second.empty()) {
            FeedItem it;
            it.type = ItemType::assistant;
            it.key = run.id + ":live";
            it.run_id = run.id;
            it.text = t->second;
            it.streaming = true;
            items.push_back(std::move(it));
        } else if (is_active_state(run.state)) {
            FeedItem it;
            it.type = ItemType::working;
            it.key = run.id + ":working";
            it.run_id = run.id;
            items.push_back(std::move(it));
        }
        if (!is_active_state(run.state) && run.state != "interrupted" && run.usage.calls > 0) {  // the Run ended
            FeedItem it;
            it.type = ItemType::activity;
            it.key = run.id + ":usage";
            it.run_id = run.id;
            it.usage = run.usage;
            items.push_back(std::move(it));
        }
        if (run.state == "cancelled") {
            FeedItem it;
            it.type = ItemType::stopped;
            it.key = run.id + ":stopped";
            it.run_id = run.id;
            it.by_user = run.reason.empty() || run.reason == "cancelled by user";
            it.reason = run.reason;
            items.push_back(std::move(it));
        }
    }
    // Standalone Action items have no Run; they follow the conversation (D9).
    for (const ActionItem& a : snap.actions)
        if (a.run_id.empty()) {
            FeedItem it;
            it.type = ItemType::action;
            it.key = "action:" + a.id;
            it.action = a;
            items.push_back(std::move(it));
        }
    return items;
}

Apply apply_live_event(FeedSnapshot& snap, LiveText& live, const LiveRunEvent& ev) {
    if (ev.thread_id != snap.entry.id) return Apply::ok;
    const json& d = ev.data;
    if (ev.kind == "queue.changed") return Apply::refetch;
    if (ev.kind == "action.created" && ev.run_id.empty()) return Apply::refetch;
    if (ev.kind == "action.resolved") {
        ActionItem* found = nullptr;
        for (ActionItem& a : snap.actions)
            if (a.id == str(d, "id")) found = &a;
        if (found) {
            found->resolved_at = ev.at.empty() ? iso_from_millis(now_millis()) : ev.at;
            found->resolution = d.contains("resolution") ? d["resolution"] : json();
        }
        if (ev.run_id.empty()) return found ? Apply::ok : Apply::refetch;
    }
    RunInfo* run = nullptr;
    for (RunInfo& r : snap.runs)
        if (r.id == ev.run_id) run = &r;
    if (!run) {
        for (RunInfo& child : snap.subagents)
            if (child.id == ev.run_id) {
                if (ev.kind == "run.state") {
                    child.state = str(d, "state");
                    child.reason = str(d, "reason");
                }
                return Apply::ok;
            }
        // A Run the snapshot does not hold yet: its streaming text is kept for when the refetched snapshot has it.
        if (ev.kind == "text.delta") live.text[ev.run_id] += str(d, "text");
        if (ev.kind == "reasoning.delta") live.reasoning[ev.run_id] += str(d, "text");
        return Apply::refetch;
    }
    if (ev.kind == "text.delta") {
        live.text[run->id] += str(d, "text");
        return Apply::ok;
    }
    if (ev.kind == "reasoning.delta") {
        live.reasoning[run->id] += str(d, "text");
        return Apply::ok;
    }
    if (ev.kind == "discard") {
        live.text.erase(run->id);
        live.reasoning.erase(run->id);
        return Apply::ok;
    }
    if (ev.kind == "usage") {  // running totals of the Run
        run->usage = {jint(d, "prompt"), jint(d, "completion"), jint(d, "reasoning"), jint(d, "requests")};
        return Apply::ok;
    }
    const long long last_seq = run->events.empty() ? 0 : run->events.back().seq;
    if (ev.kind == "run.state") {
        run->state = str(d, "state");
        run->reason = str(d, "reason");
        if (!is_active_state(run->state)) {
            live.text.erase(run->id);
            live.reasoning.erase(run->id);
        }
        // The stored log holds the transition as a "state" event; keep the sequence without gaps.
        if (ev.seq == last_seq + 1) run->events.push_back({ev.seq, ev.at, "state", d});
        else if (ev.seq > last_seq + 1) return Apply::refetch;
        return Apply::ok;
    }
    if (ev.seq <= 0) return Apply::ok;
    if (ev.seq <= last_seq) return Apply::ok;  // already in the snapshot
    if (ev.seq > last_seq + 1) return Apply::refetch;  // missed events (they arrived while a snapshot was loading)
    std::string kind = ev.kind;
    json payload = d;
    if (ev.kind == "action.created") {
        kind = "interrupt";
        payload = {{"action_id", str(d, "id")}, {"kind", str(d, "kind")}, {"call_id", str(d, "call_id")}};
        ActionItem a;
        a.id = str(d, "id");
        a.thread_id = ev.thread_id;
        a.run_id = ev.run_id;
        a.kind = str(d, "kind") == "approval" ? "approval" : "question";
        a.call_id = str(d, "call_id");
        if (const json* p = jobj(d, "payload"); p && p->is_object()) a.payload = *p;
        a.created_at = ev.at;
        snap.actions.push_back(std::move(a));
    } else if (ev.kind == "action.resolved") {
        kind = "resolution";
        payload = {{"action_id", str(d, "id")}, {"resolution", d.contains("resolution") ? d["resolution"] : json()}};
    } else if (ev.kind == "message" && str(d, "role") == "assistant") {
        live.text.erase(run->id);
    } else if (ev.kind == "reasoning") {
        live.reasoning.erase(run->id);
    }
    run->events.push_back({ev.seq, ev.at, kind, payload});
    return ev.kind == "subagent" ? Apply::refetch : Apply::ok;
}

std::vector<RailGroup> build_rail(const FeedSnapshot& snap, long long now_ms) {
    std::map<std::string, const RunInfo*> children;
    for (const RunInfo& r : snap.subagents) children[r.id] = &r;
    std::vector<RailGroup> groups;
    std::map<std::string, size_t> by_key;
    for (const RunInfo& run : snap.runs) {
        std::map<std::string, long long> call_message;
        for (const StoredEvent& e : run.events) {
            if (e.kind == "message")
                if (const json* calls = jobj(e.payload, "tool_calls"); calls && calls->is_array())
                    for (const json& c : *calls) call_message[str(c, "id")] = e.seq;
            if (e.kind != "subagent") continue;
            const std::string call_id = str(e.payload, "call_id");
            const auto m = call_message.find(call_id);
            const std::string key = run.id + ":" + (m != call_message.end() ? std::to_string(m->second) : call_id);
            auto g = by_key.find(key);
            if (g == by_key.end()) {
                g = by_key.emplace(key, groups.size()).first;
                groups.push_back({key, {}});
            }
            const std::string child_id = str(e.payload, "child_run_id");
            const auto c = children.find(child_id);
            groups[g->second].entries.push_back({child_id, str(e.payload, "task"),
                                                 c != children.end() ? c->second->state : "queued",
                                                 c != children.end() ? duration_of(*c->second, now_ms) : 0, str(e.payload, "skill")});
        }
    }
    return groups;
}

long long now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string iso_from_millis(long long ms) {
    long long days = ms / 86400000;
    long long rem = ms % 86400000;
    if (rem < 0) {
        rem += 86400000;
        --days;
    }
    // civil_from_days (Howard Hinnant)
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long m = mp < 10 ? mp + 3 : mp - 9;
    const long long y = yoe + era * 400 + (m <= 2);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ", y, m, d, rem / 3600000,
                  rem / 60000 % 60, rem / 1000 % 60, rem % 1000);
    return buf;
}

}  // namespace wb::shell
