#include "shell/feed.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

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
            ToolResult r{jbool(p, "ok"), str(p, "content"), {}, {}, {}};
            if (r.ok) r.citations = citations_of(items[t->second].tool, r.content);
            r.publish_route = publish_route_of(items[t->second].tool, items[t->second].args, r.ok);
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

// True when the Run stored an event after `after` that the streamed text (or reasoning) turned into: an assistant
// message for text, a reasoning event for reasoning.
bool stored_after(const RunInfo& run, bool reasoning, long long after) {
    for (const StoredEvent& e : run.events) {
        if (e.seq <= after) continue;
        if (reasoning ? e.kind == "reasoning" : (e.kind == "message" && str(e.payload, "role") == "assistant")) return true;
    }
    return false;
}

bool streamed_into_stored(const RunInfo& run, const std::map<std::string, long long>& after, bool reasoning) {
    const auto it = after.find(run.id);
    return it != after.end() && stored_after(run, reasoning, it->second);
}

// Appends a delta to the streamed text of a Run. The first delta of a model call records the Run's last stored sequence
// number, so the stored answer that follows is recognised as the answer to this text.
void stream_delta(std::map<std::string, std::string>& text, std::map<std::string, long long>& after, const std::string& run_id,
                  long long last_seq, const std::string& delta) {
    std::string& streamed = text[run_id];
    if (streamed.empty()) after[run_id] = last_seq;
    streamed += delta;
}

// Drops the streamed text of a Run once a stored event of that model call exists. An event at or below the recorded
// sequence number belongs to an earlier call and leaves the text alone.
void settle_streamed(LiveText& live, const std::string& run_id, long long seq, bool reasoning) {
    auto& text = reasoning ? live.reasoning : live.text;
    auto& after = reasoning ? live.reasoning_after : live.text_after;
    const auto it = after.find(run_id);
    if (it != after.end() && seq <= it->second) return;
    text.erase(run_id);
    after.erase(run_id);
}

// Child Run Action items (client/work-inbox Child Run Action Items): an open Action item of a subagent is drawn right after
// the tool call that started that subagent (the `subagent` event of its parent names the call), so it sits under the
// subagent entry. The chain of subagents up to the top-level Run is followed; an item whose chain is not in the snapshot
// is returned as unplaced and drawn at the end.
struct ChildCards {
    std::map<std::pair<std::string, std::string>, std::vector<FeedItem>> at_call;  // (top-level Run, call id) -> cards
    std::vector<FeedItem> unplaced;
};

ChildCards child_action_cards(const FeedSnapshot& snap) {
    std::set<std::string> top, children;
    for (const RunInfo& r : snap.runs) top.insert(r.id);
    for (const RunInfo& r : snap.subagents) children.insert(r.id);
    std::map<std::string, std::pair<std::string, std::string>> spawned;  // child Run -> (parent Run, call id)
    const auto scan = [&](const RunInfo& run) {
        for (const StoredEvent& e : run.events)
            if (e.kind == "subagent") spawned[str(e.payload, "child_run_id")] = {run.id, str(e.payload, "call_id")};
    };
    for (const RunInfo& r : snap.runs) scan(r);
    for (const RunInfo& r : snap.subagents) scan(r);
    ChildCards out;
    for (const ActionItem& a : snap.actions) {
        if (a.run_id.empty() || !a.open() || top.contains(a.run_id) || !children.contains(a.run_id)) continue;
        FeedItem it;
        it.type = ItemType::action;
        it.key = "action:" + a.id;
        it.run_id = a.run_id;
        it.action = a;
        bool placed = false;
        std::string cur = a.run_id;
        for (int depth = 0; depth < 32 && !placed; ++depth) {
            const auto sp = spawned.find(cur);
            if (sp == spawned.end()) break;
            if (top.contains(sp->second.first)) {
                out.at_call[{sp->second.first, sp->second.second}].push_back(it);
                placed = true;
            }
            cur = sp->second.first;
        }
        if (!placed) out.unplaced.push_back(std::move(it));
    }
    return out;
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

std::vector<Citation> cited_in_text(const std::string& text, const std::map<long long, Citation>& cited) {
    // Markers of one line outside inline code spans; a backtick run closes at a run of the same length.
    const auto markers_of_line = [](const std::string& line, std::set<long long>& out) {
        for (size_t i = 0; i < line.size();) {
            if (line[i] == '`') {
                size_t run = 0;
                while (i + run < line.size() && line[i + run] == '`') ++run;
                const std::string closing(run, '`');
                const size_t close = line.find(closing, i + run);
                if (close == std::string::npos) {
                    i += run;
                    continue;
                }
                i = close + run;
                continue;
            }
            if (line[i] == '[') {
                size_t j = i + 1;
                long long value = 0;
                while (j < line.size() && line[j] >= '0' && line[j] <= '9' && j - i <= 7) value = value * 10 + (line[j++] - '0');
                if (j > i + 1 && j < line.size() && line[j] == ']') out.insert(value);
                i = j > i + 1 ? j + 1 : i + 1;
                continue;
            }
            ++i;
        }
    };
    std::set<long long> numbers;
    bool fenced = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        const std::string t = trim(line);
        if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) fenced = !fenced;
        else if (!fenced) markers_of_line(line, numbers);
        pos = eol + 1;
    }
    std::vector<Citation> out;
    for (const long long n : numbers)
        if (const auto c = cited.find(n); c != cited.end()) out.push_back(c->second);
    return out;
}

std::string publish_route_of(const std::string& tool, const json& args, bool ok) {
    if (!ok || tool != "skills.save" || !args.is_object() || !args.contains("name") || !args["name"].is_string()) return {};
    const std::string name = args["name"].get<std::string>();
    if (name.empty() || name.size() > 64 || name.front() == '-') return {};
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return {};
    return "#/skills/edit/" + name;
}

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

// The folded stored events of one Run: its items and the index of each tool card among them.
struct FoldedRun {
    size_t count = 0;  // stored events folded
    long long last_seq = 0;
    std::vector<FeedItem> items;
    std::map<std::string, size_t> tools;
};

FeedFold::FeedFold() = default;
FeedFold::~FeedFold() = default;

std::vector<FeedItem> build_feed(const FeedSnapshot& snap, const LiveText& live) {
    FeedFold fold;
    return fold.build(snap, live);
}

std::vector<FeedItem> FeedFold::build(const FeedSnapshot& snap, const LiveText& live) {
    std::map<std::string, const ActionItem*> actions;
    for (const ActionItem& a : snap.actions) actions[a.id] = &a;
    // The fold of an interrupt copies its Action item, so the cache is kept only while the Action items are unchanged.
    std::string action_state;
    for (const ActionItem& a : snap.actions) action_state += a.id + "|" + a.resolved_at + "|" + a.resolution.dump() + "\n";
    if (action_state != actions_) {
        runs_.clear();
        actions_ = std::move(action_state);
    }
    std::set<std::string> seen;
    std::vector<FeedItem> items;
    for (const RunInfo& run : snap.runs) {
        seen.insert(run.id);
        FoldedRun& folded = runs_[run.id];
        const bool appended = folded.count <= run.events.size() && (folded.count == 0 || run.events[folded.count - 1].seq == folded.last_seq);
        if (!appended) folded = FoldedRun{};
        for (size_t i = folded.count; i < run.events.size(); ++i) {
            fold_event(folded.items, folded.tools, actions, run, run.events[i]);
            ++folded_;
        }
        folded.count = run.events.size();
        folded.last_seq = run.events.empty() ? 0 : run.events.back().seq;
        const size_t first = items.size();
        items.insert(items.end(), folded.items.begin(), folded.items.end());
        if (const auto r = live.reasoning.find(run.id);
            r != live.reasoning.end() && !r->second.empty() && !streamed_into_stored(run, live.reasoning_after, true)) {
            FeedItem it;
            it.type = ItemType::reasoning;
            it.key = run.id + ":live-r";
            it.run_id = run.id;
            it.text = r->second;
            it.streaming = true;
            items.push_back(std::move(it));
        }
        // The stored answer replaces the text that streamed into it, also when a snapshot holds both (Live Feed Updates).
        if (const auto t = live.text.find(run.id);
            t != live.text.end() && !t->second.empty() && !streamed_into_stored(run, live.text_after, false)) {
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
        // Cited Sources In The Feed: an answer lists the passages of this Run's searches that its [n] markers name.
        std::map<long long, Citation> cited;
        for (size_t i = first; i < items.size(); ++i)
            if (items[i].type == ItemType::tool && items[i].result)
                for (const Citation& c : items[i].result->citations) cited.emplace(c.n, c);
        if (!cited.empty())
            for (size_t i = first; i < items.size(); ++i)
                if (items[i].type == ItemType::assistant) items[i].sources = cited_in_text(items[i].text, cited);
    }
    // Child Run Action items sit after the call that started their subagent (Child Run Action Items).
    ChildCards cards = child_action_cards(snap);
    if (!cards.at_call.empty()) {
        std::vector<FeedItem> placed;
        placed.reserve(items.size() + cards.at_call.size());
        for (FeedItem& it : items) {
            const bool tool = it.type == ItemType::tool;
            const std::pair<std::string, std::string> call{it.run_id, it.call_id};
            placed.push_back(std::move(it));
            if (!tool) continue;
            const auto c = cards.at_call.find(call);
            if (c == cards.at_call.end()) continue;
            for (FeedItem& card : c->second) placed.push_back(std::move(card));
            cards.at_call.erase(c);
        }
        items = std::move(placed);
        for (auto& [call, list] : cards.at_call)  // the call's item is not in the feed: drawn at the end
            for (FeedItem& card : list) cards.unplaced.push_back(std::move(card));
    }
    for (FeedItem& card : cards.unplaced) items.push_back(std::move(card));
    // Standalone Action items have no Run; they follow the conversation (D9).
    for (const ActionItem& a : snap.actions)
        if (a.run_id.empty()) {
            FeedItem it;
            it.type = ItemType::action;
            it.key = "action:" + a.id;
            it.action = a;
            items.push_back(std::move(it));
        }
    for (auto it = runs_.begin(); it != runs_.end();)  // Runs that left the snapshot
        it = seen.contains(it->first) ? std::next(it) : runs_.erase(it);
    return items;
}

RepaintLimit::RepaintLimit(long long interval_ms) : interval_(interval_ms) {}

bool RepaintLimit::request(long long now_ms) {
    if (last_ < 0 || now_ms - last_ >= interval_) {
        last_ = now_ms;
        pending_ = false;
        return true;
    }
    if (!pending_) {
        pending_ = true;
        due_ = last_ + interval_;
    }
    return false;
}

bool RepaintLimit::take_due(long long now_ms) {
    if (!pending_ || now_ms < due_) return false;
    pending_ = false;
    last_ = now_ms;
    return true;
}

RepaintTimer::RepaintTimer(RepaintLimit& limit, SetTimer set, CancelTimer cancel, std::function<long long()> now,
                           std::function<void()> repaint)
    : limit_(limit), set_(std::move(set)), cancel_(std::move(cancel)), now_(std::move(now)), repaint_(std::move(repaint)) {}

void RepaintTimer::streamed_change() {
    if (limit_.request(now_())) {
        repaint_();
        return;
    }
    arm();
}

void RepaintTimer::stored_change() {
    if (id_) {
        cancel_(id_);
        id_ = 0;
    }
    repaint_();
}

void RepaintTimer::arm() {
    if (id_) return;
    const long long wait = std::max(1LL, limit_.due_at() - now_());
    id_ = set_(static_cast<unsigned>(wait), [this] { fire(); });
}

void RepaintTimer::fire() {
    id_ = 0;
    if (limit_.take_due(now_())) {
        repaint_();
    } else if (limit_.pending()) {
        arm();  // the timer came early: the repaint waits for the rest of its interval
    }
}

Apply apply_live_event(FeedSnapshot& snap, LiveText& live, const LiveRunEvent& ev) {
    if (ev.thread_id != snap.entry.id) return Apply::ok;
    const json& d = ev.data;
    if (ev.seq > 0) {
        long long& seen = live.seen[ev.run_id];
        seen = std::max(seen, ev.seq);
    }
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
                // The card of a subagent's Action item needs the subagent's log, which the snapshot loads (Child Run Action
                // Items): the feed reloads.
                if (ev.kind == "action.created") return Apply::refetch;
                return Apply::ok;
            }
        // A Run the snapshot does not hold yet: its streaming text is kept for when the refetched snapshot has it. Its
        // stored events are not known, so the recorded sequence number is 0.
        if (ev.kind == "text.delta") stream_delta(live.text, live.text_after, ev.run_id, live.seen[ev.run_id], str(d, "text"));
        if (ev.kind == "reasoning.delta") stream_delta(live.reasoning, live.reasoning_after, ev.run_id, live.seen[ev.run_id], str(d, "text"));
        return Apply::refetch;
    }
    const long long last_seq = run->events.empty() ? 0 : run->events.back().seq;  // the snapshot's stored log
    const long long delta_base = std::max(last_seq, live.seen[run->id]);           // the newest event that arrived
    if (ev.kind == "text.delta") {
        stream_delta(live.text, live.text_after, run->id, delta_base, str(d, "text"));
        return Apply::ok;
    }
    if (ev.kind == "reasoning.delta") {
        stream_delta(live.reasoning, live.reasoning_after, run->id, delta_base, str(d, "text"));
        return Apply::ok;
    }
    if (ev.kind == "discard") {
        live.text.erase(run->id);
        live.text_after.erase(run->id);
        live.reasoning.erase(run->id);
        live.reasoning_after.erase(run->id);
        return Apply::ok;
    }
    if (ev.kind == "usage") {  // running totals of the Run
        run->usage = {jint(d, "prompt"), jint(d, "completion"), jint(d, "reasoning"), jint(d, "requests")};
        return Apply::ok;
    }
    // A stored answer or reasoning block of the streamed model call replaces the streamed text, whether it is new or
    // already in the snapshot (Live Feed Updates).
    if (ev.kind == "message" && str(d, "role") == "assistant") settle_streamed(live, run->id, ev.seq, false);
    if (ev.kind == "reasoning") settle_streamed(live, run->id, ev.seq, true);
    if (ev.kind == "run.state") {
        run->state = str(d, "state");
        run->reason = str(d, "reason");
        if (!is_active_state(run->state)) {
            live.text.erase(run->id);
            live.text_after.erase(run->id);
            live.reasoning.erase(run->id);
            live.reasoning_after.erase(run->id);
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
    }
    run->events.push_back({ev.seq, ev.at, kind, payload});
    return ev.kind == "subagent" ? Apply::refetch : Apply::ok;
}

void settle_live(const FeedSnapshot& snap, LiveText& live) {
    for (const RunInfo& run : snap.runs) {
        if (streamed_into_stored(run, live.text_after, false)) {
            live.text.erase(run.id);
            live.text_after.erase(run.id);
        }
        if (streamed_into_stored(run, live.reasoning_after, true)) {
            live.reasoning.erase(run.id);
            live.reasoning_after.erase(run.id);
        }
    }
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
    char buf[160];  // GCC bounds each long long field at 20 bytes; the timestamp itself is 24
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ", y, m, d, rem / 3600000,
                  rem / 60000 % 60, rem / 1000 % 60, rem % 1000);
    return buf;
}

}  // namespace wb::shell
