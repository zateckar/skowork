#include "telemetry/collector.h"

#include "model/types.h"

namespace wb::telemetry {

std::string origin_name(const runtime::RunInfo& run) {
    if (!run.parent_run_id.empty()) return "subagent";
    switch (run.origin.kind) {
    case runtime::OriginKind::user_message: return "user";
    case runtime::OriginKind::trigger_occurrence: return "trigger";
    case runtime::OriginKind::remote_task: return "remote";
    }
    return "user";
}

void RunCollector::run_start(const runtime::RunInfo& run) {
    const std::lock_guard lock(mx_);
    runs_[run.run_id] = RunState{};
}

void RunCollector::after_model_call(const runtime::ModelCallInfo& call) {
    TelemetryService* svc = service_();
    if (!svc || !call.response) return;
    const std::string role(model::role_name(call.request ? call.request->role : model::Role::main));
    svc->emit(Event::model_usage, {{Attr::run_id, call.run.run_id}, {Attr::model_role, role}, {Attr::tokens_prompt, call.response->usage.prompt},
                                   {Attr::tokens_completion, call.response->usage.completion}});
    if (!svc->content_active()) return;
    const std::lock_guard lock(mx_);
    auto it = runs_.find(call.run.run_id);
    if (it == runs_.end()) return;
    if (it->second.prompt.empty() && call.request)
        for (const auto& m : call.request->messages)
            if (m.role == "user") {
                it->second.prompt = m.content;
                break;
            }
    if (!call.response->text.empty()) it->second.response = call.response->text;
}

runtime::ToolDecision RunCollector::before_tool_call(const runtime::ToolCallInfo& call) {
    const std::lock_guard lock(mx_);
    calls_[call.call_id] = std::chrono::steady_clock::now();
    return runtime::ToolDecision::allow();
}

void RunCollector::after_tool_call(const runtime::ToolCallInfo& call, const runtime::ToolResultInfo& result) {
    long long ms = 0;
    {
        const std::lock_guard lock(mx_);
        if (auto it = calls_.find(call.call_id); it != calls_.end()) {
            ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - it->second).count();
            calls_.erase(it);
        }
        if (auto r = runs_.find(call.run.run_id); r != runs_.end()) ++r->second.tool_calls;
    }
    TelemetryService* svc = service_();
    if (!svc) return;
    std::string url = jstr(call.args, "url");
    for (const auto& res : call.resources)
        if (url.empty() && res.kind == runtime::ToolResource::Kind::url) url = res.value;
    std::vector<Field> f{{Attr::run_id, call.run.run_id},
                         {Attr::tool_id, call.tool},
                         {Attr::tool_risk, runtime::to_string(call.risk)},
                         {Attr::tool_outcome, result.ok ? "ok" : "error"},
                         {Attr::tool_duration_ms, ms}};
    if (!url.empty()) f.emplace_back(Attr::url_domain, url);  // reduced to the domain by the record builder
    if (svc->content_active()) f.emplace_back(Attr::tool_arguments, call.args.dump());
    svc->emit(Event::tool_call, f);
}

void RunCollector::run_end(const runtime::RunInfo& run, runtime::RunState state, const std::string& reason) {
    RunState rs;
    {
        const std::lock_guard lock(mx_);
        if (auto it = runs_.find(run.run_id); it != runs_.end()) {
            rs = std::move(it->second);
            runs_.erase(it);
        }
    }
    TelemetryService* svc = service_();
    if (!svc) return;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rs.start).count();
    std::vector<Field> f{{Attr::run_id, run.run_id},
                         {Attr::run_outcome, runtime::to_string(state)},
                         {Attr::run_origin, origin_name(run)},
                         {Attr::run_duration_ms, ms},
                         {Attr::run_tool_calls, rs.tool_calls}};
    // The reason of a failed Run starts with its typed code ("timeout: ...", "QUOTA_EXCEEDED ..."): only the code goes.
    if (state == runtime::RunState::failed && !reason.empty()) f.emplace_back(Attr::error_code, reason.substr(0, reason.find_first_of(": ")));
    if (svc->content_active()) {
        if (!rs.prompt.empty()) f.emplace_back(Attr::content_prompt, rs.prompt);
        if (!rs.response.empty()) f.emplace_back(Attr::content_response, rs.response);
    }
    svc->emit(Event::run_finished, f);
}

std::string rule_id(const std::string& rule) {
    // Filesystem rules and folder grants carry expanded paths (the user's profile folder, a granted folder): only the
    // rule key goes. Tool and network rules name tool-id and host patterns from the policy and stay whole.
    if (rule.starts_with("grant:")) return "grant";
    if (rule.starts_with("filesystem.")) return rule.substr(0, rule.find('['));
    return rule;
}

void record_decision(TelemetryService& svc, const policy::DecisionRecord& r) {
    if (r.kind == "resolution") {
        svc.emit(Event::approval_resolved, {{Attr::run_id, r.run_id}, {Attr::tool_id, r.tool}, {Attr::tool_risk, r.risk},
                                            {Attr::approval_decision, r.outcome}});
        return;
    }
    // Decisions and egress refusals: the rule id and version, never the path, URL or arguments.
    svc.emit(Event::policy_decision, {{Attr::run_id, r.run_id}, {Attr::tool_id, r.tool}, {Attr::tool_risk, r.risk},
                                      {Attr::policy_rule, rule_id(r.rule)}, {Attr::policy_version, r.policy_version}, {Attr::policy_outcome, r.outcome}});
}

}  // namespace wb::telemetry
