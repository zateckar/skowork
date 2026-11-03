#include "app/agent_service.h"

#include "base/strings.h"

namespace wb {

using runtime::Risk;

// ---- Interim policy ------------------------------------------------------------------------------------------------

runtime::ToolDecision InterimPolicy::before_tool_call(const runtime::ToolCallInfo& call) {
    if (call.risk == Risk::external || call.risk == Risk::destructive)
        return runtime::ToolDecision::approval(call.risk == Risk::external ? "external action" : "destructive action");
    return runtime::ToolDecision::allow();
}

bool InterimPolicy::tool_permitted(const std::string&, Risk) const { return true; }

bool InterimPolicy::may_require_approval(const std::string&, Risk r) const {
    return r == Risk::external || r == Risk::destructive;
}

// ---- JSON views ------------------------------------------------------------------------------------------------------

json to_json(const runtime::RunRecord& r) {
    return {{"id", r.id},
            {"thread_id", r.thread_id},
            {"parent_run_id", r.parent_run_id},
            {"state", runtime::to_string(r.state)},
            {"reason", r.reason},
            {"origin", {{"kind", runtime::to_string(r.origin.kind)}, {"ref", r.origin.ref}}},
            {"unattended", r.unattended},
            {"role", r.role},
            {"active_ms", r.active_ms},
            {"model_calls", r.model_calls},
            {"tool_calls", r.tool_calls},
            {"usage", {{"prompt", r.usage.prompt}, {"completion", r.usage.completion}, {"reasoning", r.usage.reasoning}}},
            {"created_at", r.created_at},
            {"started_at", r.started_at},
            {"ended_at", r.ended_at}};
}

json to_json(const runtime::ActionItem& a) {
    return {{"id", a.id},           {"thread_id", a.thread_id},   {"run_id", a.run_id},       {"kind", a.kind},
            {"call_id", a.call_id}, {"payload", a.payload},       {"created_at", a.created_at},
            {"resolved_at", a.resolved_at}, {"resolution", a.resolution}};
}

json to_json(const model::ChatMessage& m) {
    json j = {{"role", m.role}, {"content", m.content}};
    if (!m.tool_calls.empty()) {
        json calls = json::array();
        for (const auto& c : m.tool_calls) calls.push_back({{"id", c.id}, {"tool", c.tool}, {"arguments", c.arguments}});
        j["tool_calls"] = std::move(calls);
    }
    if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
    return j;
}

namespace {

json event_json(const runtime::RunEvent& e) {
    return {{"kind", e.kind}, {"run_id", e.run_id}, {"thread_id", e.thread_id}, {"seq", e.seq}, {"at", e.at}, {"data", e.data}};
}

Result<std::string> required(const json& p, const char* key) {
    std::string v = jstr(p, key);
    if (v.empty()) return make_error("invalid_params", std::string(key) + " is required");
    return v;
}

// The first line of the first message, as the Thread title.
std::string title_from(const std::string& text) {
    std::string line = trim(text.substr(0, text.find('\n')));
    if (line.size() > 80) line = utf8_truncate(line, 77) + "...";
    return line;
}

}  // namespace

// ---- Service -----------------------------------------------------------------------------------------------------------

Result<std::unique_ptr<AgentService>> AgentService::create(Database& db, model::IChatModel& model, runtime::RuntimeConfig config,
                                                           std::shared_ptr<runtime::PolicyHook> policy,
                                                           std::vector<std::shared_ptr<runtime::Hook>> hooks) {
    if (!policy) policy = std::make_shared<InterimPolicy>();
    runtime::HookRegistry registry(std::move(policy));
    for (auto& h : hooks) registry.add(std::move(h));
    auto rt = runtime::AgentRuntime::create(std::move(config), db, model, system_clock(), std::move(registry));
    if (!rt) return rt.error();
    std::unique_ptr<AgentService> svc(new AgentService(std::move(*rt)));
    AgentService* self = svc.get();
    svc->rt_->set_listener([self](const runtime::RunEvent& e) {
        std::function<void(const json&)> sink;
        {
            const std::lock_guard lock(self->sink_mx_);
            sink = self->sink_;
        }
        if (sink) sink(event_json(e));
    });
    return svc;
}

void AgentService::set_event_sink(std::function<void(const json&)> sink) {
    const std::lock_guard lock(sink_mx_);
    sink_ = std::move(sink);
}

Status AgentService::start() {
    auto r = rt_->start();
    if (!r) return r.error();
    return {};
}

void AgentService::shutdown() {
    set_event_sink(nullptr);
    rt_->shutdown();
}

void AgentService::register_rpc(RpcRegistry& rpc, bool expose_on_pipe) {
    const RpcExposure ex = expose_on_pipe ? RpcExposure::ui_and_pipe : RpcExposure::ui;
    runtime::AgentRuntime& rt = *rt_;

    rpc.add("threads.create", ex, [&rt](const json& p) -> Result<json> {
        auto id = rt.create_thread(jstr(p, "title"));
        if (!id) return id.error();
        return json{{"thread_id", *id}};
    });
    rpc.add("threads.list", ex, [&rt](const json& p) -> Result<json> {
        const long long limit = std::clamp<long long>(jint(p, "limit", 200), 1, 1000);
        json out = json::array();
        for (const auto& t : rt.store().list_threads(static_cast<int>(limit)))
            out.push_back({{"id", t.id}, {"title", t.title}, {"created_at", t.created_at}, {"updated_at", t.updated_at},
                           {"last_run_state", t.last_run_state}, {"open_actions", t.open_actions}});
        return json{{"threads", out}};
    });
    rpc.add("threads.get", ex, [&rt](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        if (!rt.store().thread_exists(*id)) return make_error("not_found", "no such Thread");
        json messages = json::array(), runs = json::array(), actions = json::array(), queued = json::array();
        for (const auto& m : rt.conversation(*id)) messages.push_back(to_json(m));
        for (const auto& r : rt.runs(*id)) runs.push_back(to_json(r));
        for (const auto& a : rt.pending_actions(*id)) actions.push_back(to_json(a));
        for (const auto& q : rt.queued_messages(*id))
            queued.push_back({{"id", q.id}, {"text", q.text}, {"mode", q.mode == runtime::MessageMode::steer_now ? "steer_now" : "queue"},
                              {"created_at", q.created_at}});
        return json{{"thread_id", *id}, {"messages", messages}, {"runs", runs}, {"actions", actions}, {"queued", queued}};
    });
    rpc.add("threads.send", ex, [&rt](const json& p) -> Result<json> {
        const std::string text = jstr(p, "text");
        if (trim(text).empty()) return make_error("invalid_params", "text is required");
        std::string thread = jstr(p, "thread_id");
        if (thread.empty()) {
            auto created = rt.create_thread(title_from(text));
            if (!created) return created.error();
            thread = *created;
        } else if (!rt.store().thread_exists(thread)) {
            return make_error("not_found", "no such Thread");
        } else if (rt.runs(thread).empty()) {
            (void)rt.store().set_thread_title(thread, title_from(text));
        }
        const auto mode = jstr(p, "mode") == "steer_now" ? runtime::MessageMode::steer_now : runtime::MessageMode::queue;
        auto r = rt.send_message(thread, text, mode);
        if (!r) return r.error();
        json out = {{"thread_id", thread}};
        if (!r->run_id.empty()) out["run_id"] = r->run_id;
        if (r->queued_message) out["queued_message"] = r->queued_message;
        return out;
    });
    rpc.add("threads.compact", ex, [&rt](const json& p) -> Result<json> {
        auto id = required(p, "thread_id");
        if (!id) return id.error();
        auto r = rt.compact_thread(*id);
        if (!r) return r.error();
        return json{{"messages_compacted", r->messages_compacted}, {"tokens_before", r->tokens_before}, {"tokens_after", r->tokens_after}};
    });
    rpc.add("queue.edit", ex, [&rt](const json& p) -> Result<json> {
        if (Status s = rt.edit_queued_message(jint(p, "id"), jstr(p, "text")); !s) return s.error();
        return json{{"ok", true}};
    });
    rpc.add("queue.remove", ex, [&rt](const json& p) -> Result<json> {
        if (Status s = rt.remove_queued_message(jint(p, "id")); !s) return s.error();
        return json{{"ok", true}};
    });
    rpc.add("runs.get", ex, [&rt](const json& p) -> Result<json> {
        auto id = required(p, "run_id");
        if (!id) return id.error();
        const auto r = rt.run(*id);
        if (!r) return make_error("not_found", "no such Run");
        return to_json(*r);
    });
    rpc.add("runs.events", ex, [&rt](const json& p) -> Result<json> {
        auto id = required(p, "run_id");
        if (!id) return id.error();
        json out = json::array();
        for (const auto& e : rt.events(*id, jint(p, "after_seq")))
            out.push_back({{"seq", e.seq}, {"at", e.at}, {"kind", e.kind}, {"payload", e.payload}});
        return json{{"events", out}};
    });
    rpc.add("runs.cancel", ex, [&rt](const json& p) -> Result<json> {
        auto id = required(p, "run_id");
        if (!id) return id.error();
        if (Status s = rt.cancel(*id); !s) return s.error();
        return json{{"ok", true}};
    });
    rpc.add("actions.resolve", ex, [this, &rt](const json& p) -> Result<json> {
        auto id = required(p, "action_id");
        if (!id) return id.error();
        const std::string d = jstr(p, "decision");
        runtime::ApprovalResolution res;
        if (d == "approve") res = runtime::ApprovalResolution::approve();
        else if (d == "approve_edited") res = runtime::ApprovalResolution::approve_with(p.contains("args") ? p["args"] : json::object());
        else if (d == "reject") res = runtime::ApprovalResolution::reject();
        else if (d == "respond") res = runtime::ApprovalResolution::respond(jstr(p, "text"));
        else return make_error("invalid_params", "decision must be approve, approve_edited, reject or respond");
        std::optional<runtime::ActionItem> item;
        for (auto& a : rt.pending_actions())
            if (a.id == *id) item = a;
        if (Status s = rt.resolve_approval(*id, res); !s) return s.error();
        if (item && on_resolution_) on_resolution_(*item, d);
        return json{{"ok", true}};
    });
    rpc.add("actions.answer", ex, [&rt](const json& p) -> Result<json> {
        auto id = required(p, "action_id");
        if (!id) return id.error();
        if (Status s = rt.answer_question(*id, jstr(p, "answer")); !s) return s.error();
        return json{{"ok", true}};
    });
}

}  // namespace wb
