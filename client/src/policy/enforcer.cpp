#include "policy/enforcer.h"

#include "base/strings.h"

namespace wb::policy {

using runtime::Risk;
using runtime::ToolDecision;

const char* risk_key(Risk r) {
    switch (r) {
    case Risk::read: return "read";
    case Risk::write_local: return "write-local";
    case Risk::external: return "external";
    case Risk::destructive: return "destructive";
    }
    return "destructive";
}

bool tool_switched_off(const EffectivePolicy* policy, const std::string& tool_id) {
    const auto mode = [&](const char* key) { return policy ? policy->string(key, "off") : std::string("off"); };
    if (starts_with(tool_id, "shell.")) return mode("tools.shell.mode") == "off";
    if (starts_with(tool_id, "computer.")) return mode("tools.computer.mode") == "off";
    return false;
}

PathRules PolicyEnforcer::path_rules_now() const {
    const auto p = env_.policy ? env_.policy() : nullptr;
    PathRules r = path_rules(p.get(), env_.workspace, env_.data_root, env_.grants ? env_.grants() : std::vector<FolderGrant>{});
    if (env_.app_folders) r.app_folders = final_folders(env_.app_folders({}));
    return r;
}

namespace {
CallFacts facts_of(const runtime::ToolCallInfo& call) {
    CallFacts f;
    f.tool = call.tool;
    f.risk = risk_key(call.risk);
    f.unattended = call.run.unattended;
    f.own_data = call.own_data;
    for (const auto& r : call.resources) {
        Resource x;
        x.kind = r.kind == runtime::ToolResource::Kind::url ? Resource::Kind::url
                 : r.kind == runtime::ToolResource::Kind::write_path ? Resource::Kind::write_path
                                                                     : Resource::Kind::read_path;
        x.value = r.value;
        f.resources.push_back(std::move(x));
    }
    if (call.skill) f.skill = SkillFloor{call.skill->interrupt_on, call.skill->max_risk};
    return f;
}
}  // namespace

Verdict PolicyEnforcer::evaluate(const runtime::ToolCallInfo& call) const {
    const auto policy = env_.policy ? env_.policy() : nullptr;
    const CallFacts f = facts_of(call);
    if (policy && tool_switched_off(policy.get(), call.tool)) {
        const std::string key = starts_with(call.tool, "shell.") ? "tools.shell.mode" : "tools.computer.mode";
        return {Decision::deny, key, policy->source(key), "this tool is switched off by policy", policy->version()};
    }
    if (env_.unavailable) {
        if (const auto why = env_.unavailable(call.tool)) return {Decision::deny, "unavailable", "", *why, policy ? policy->version() : 0};
    }
    PathRules rules;
    bool rules_loaded = false;
    const ResourceCheck check = [&](const Resource& r) -> RuleVerdict {
        if (r.kind == Resource::Kind::url)
            return check_egress_url(r.value, policy->strings("network.egress_allow"), policy->strings("network.egress_deny"));
        if (!rules_loaded) {
            rules = path_rules(policy.get(), env_.workspace, env_.data_root, env_.grants ? env_.grants() : std::vector<FolderGrant>{});
            if (env_.app_folders) rules.app_folders = final_folders(env_.app_folders(call.run.run_id));
            rules_loaded = true;
        }
        auto fp = canonicalize(r.value);
        if (!fp) return {false, "path", fp.error().message};
        return check_path(*fp, r.kind == Resource::Kind::write_path ? Access::write : Access::read, rules);
    };
    return decide(policy.get(), f, env_.floors, check);
}

ToolDecision PolicyEnforcer::before_tool_call(const runtime::ToolCallInfo& call) {
    const Verdict v = evaluate(call);
    if (env_.log && v.decision != Decision::allow) {
        DecisionRecord r;
        r.kind = "decision";
        r.run_id = call.run.run_id;
        r.call_id = call.call_id;
        r.tool = call.tool;
        r.risk = risk_key(call.risk);
        r.outcome = to_string(v.decision);
        r.rule = v.rule;
        r.source = v.source;
        r.policy_version = v.policy_version;
        r.reason = v.reason;
        (void)env_.log->record(r);
    }
    const std::string why = v.reason + " (" + v.rule + (v.source.empty() ? "" : ", from " + v.source) + ")";
    switch (v.decision) {
    case Decision::allow: return ToolDecision::allow();
    case Decision::approve: return ToolDecision::approval(why);
    case Decision::deny: return ToolDecision::deny(why);
    }
    return ToolDecision::deny(why);
}

bool PolicyEnforcer::tool_permitted(const std::string& tool_id, Risk declared_risk) const {
    const auto policy = env_.policy ? env_.policy() : nullptr;
    if (!policy) return declared_risk == Risk::read;  // calls are still decided one by one (own data only)
    if (tool_switched_off(policy.get(), tool_id)) return false;
    if (env_.unavailable && env_.unavailable(tool_id)) return false;
    CallFacts f;
    f.tool = tool_id;
    f.risk = risk_key(declared_risk);
    return decide(policy.get(), f, env_.floors).decision != Decision::deny;
}

std::optional<std::string> PolicyEnforcer::withheld_reason(const std::string& tool_id, Risk declared_risk) const {
    const auto policy = env_.policy ? env_.policy() : nullptr;
    if (!policy) return std::nullopt;  // no policy yet: nothing an administrator has decided
    if (tool_switched_off(policy.get(), tool_id)) {
        const bool shell = starts_with(tool_id, "shell.");
        return std::string(shell ? "Shell commands (shell.*) are switched off by policy.tools.shell.mode."
                                 : "Computer use (computer.*) is switched off by policy.tools.computer.mode.") +
               " An administrator can enable it.";
    }
    CallFacts f;
    f.tool = tool_id;
    f.risk = risk_key(declared_risk);
    const Verdict v = decide(policy.get(), f, env_.floors);
    if (v.decision != Decision::deny) return std::nullopt;
    return tool_id + " is denied by policy." + v.rule + (v.source.empty() ? "" : " (from " + v.source + ")") +
           ". An administrator can change it.";
}

bool PolicyEnforcer::may_require_approval(const std::string& tool_id, Risk declared_risk) const {
    const auto policy = env_.policy ? env_.policy() : nullptr;
    if (!policy) return false;  // without policy nothing reaches approval: own-data reads are allowed, the rest denied
    CallFacts f;
    f.tool = tool_id;
    f.risk = risk_key(declared_risk);
    if (decide(policy.get(), f, env_.floors).decision == Decision::approve) return true;
    f.unattended = true;
    return decide(policy.get(), f, env_.floors).decision == Decision::approve;
}

void PolicyEnforcer::record_resolution(const std::string& run_id, const std::string& call_id, const std::string& tool,
                                       const std::string& resolution) {
    if (!env_.log) return;
    DecisionRecord r;
    r.kind = "resolution";
    r.run_id = run_id;
    r.call_id = call_id;
    r.tool = tool;
    r.outcome = resolution;
    if (const auto d = env_.log->for_call(call_id)) {
        r.risk = d->risk;
        r.rule = d->rule;
        r.source = d->source;
        r.policy_version = d->policy_version;
    }
    r.reason = "resolved by the user";
    (void)env_.log->record(r);
}

}  // namespace wb::policy
