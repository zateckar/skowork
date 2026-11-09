#include "policy/decide.h"

#include <algorithm>

namespace wb::policy {

const ToolFloors& declared_tool_floors() {
    // client/mail-calendar: sending mail always needs approval; policy.unattended.rules may allow it for Unattended runs.
    // client/computer-use: in Unattended runs every action other than observe and screenshot needs approval; only an
    // explicit allow in policy.unattended.rules lifts it.
    static const ToolFloors floors = {
        {"mail.send", ToolFloor{Decision::approve, true, false}},
        {"computer.click", ToolFloor{Decision::approve, true, true}},
        {"computer.type", ToolFloor{Decision::approve, true, true}},
        {"computer.keys", ToolFloor{Decision::approve, true, true}},
        {"computer.scroll", ToolFloor{Decision::approve, true, true}},
        {"computer.drag", ToolFloor{Decision::approve, true, true}},
        {"computer.select", ToolFloor{Decision::approve, true, true}},
    };
    return floors;
}

std::optional<MapMatch> most_specific(const json* map, std::string_view tool) {
    if (!map || !map->is_object()) return std::nullopt;
    std::optional<MapMatch> best;
    int best_spec = -1;
    for (const auto& [pattern, v] : map->items()) {
        if (!v.is_string()) continue;
        const auto d = parse_decision(v.get<std::string>());
        if (!d || !glob_match(pattern, tool)) continue;
        const int spec = pattern_specificity(pattern);
        if (spec > best_spec || (spec == best_spec && stricter(*d, best->decision) == *d && *d != best->decision)) {
            best = MapMatch{pattern, *d};
            best_spec = spec;
        }
    }
    return best;
}

namespace {

struct Builder {
    const EffectivePolicy* policy;
    Verdict v;

    // Raises the decision to at least `d`; records the rule when it changed the result.
    void at_least(Decision d, std::string rule, std::string source, std::string reason) {
        if (stricter(v.decision, d) == v.decision) return;
        v.decision = d;
        v.rule = std::move(rule);
        v.source = std::move(source);
        v.reason = std::move(reason);
    }
};

}  // namespace

Verdict decide(const EffectivePolicy* policy, const CallFacts& call, const ToolFloors& floors, const ResourceCheck& check) {
    if (!policy) {
        if (call.own_data && call.risk == "read") return {Decision::allow, "fail-closed", "fail-closed", "no valid policy; reading the client's own data only", 0};
        return {Decision::deny, "fail-closed", "fail-closed",
                "no valid policy is in force; SkoWork must refresh its policy from the control plane before it can do this", 0};
    }
    Builder b{policy, {}};
    b.v.policy_version = policy->version();

    // 1. The most specific tools.rules pattern, then tools.risk for the Risk class, then the Risk Class Defaults.
    const int rank = risk_rank(call.risk);
    const std::string risk = rank < 0 ? "destructive" : call.risk;
    if (const auto m = most_specific(policy->find("tools.rules"), call.tool)) {
        const std::string rule = entry_path("tools.rules", m->pattern);
        b.v = {m->decision, rule, policy->source(rule), "decided by the tool rule " + m->pattern, policy->version()};
    } else {
        const json* risks = policy->is_set("tools.risk") ? policy->find("tools.risk") : nullptr;
        const json* set = risks && risks->is_object() && risks->contains(risk) ? &risks->at(risk) : nullptr;
        const std::string rule = entry_path("tools.risk", risk);
        std::optional<Decision> d = set && set->is_string() ? parse_decision(set->get<std::string>()) : std::nullopt;
        if (d) {
            b.v = {*d, rule, policy->source(rule), "decided by the rule for " + risk + " actions", policy->version()};
        } else {
            const json& defaults = client_policy_defaults().at("tools.risk");
            d = parse_decision(defaults.value(risk, "deny"));
            b.v = {d.value_or(Decision::deny), rule, "default", "decided by the default for " + risk + " actions", policy->version()};
        }
    }
    if (b.v.decision == Decision::deny) return b.v;

    // 2. Argument checks: filesystem rules for paths, egress rules for URLs. A failed check denies.
    if (check) {
        for (const auto& r : call.resources) {
            const RuleVerdict rv = check(r);
            if (!rv.allowed) {
                return {Decision::deny, rv.rule, policy->source(rv.rule), rv.reason.empty() ? "denied by " + rv.rule : rv.reason, policy->version()};
            }
        }
    }

    // 3. Floors declared by the tool's spec and by the active Skill: only ever stricter.
    std::optional<MapMatch> unattended_rule;
    if (call.unattended) unattended_rule = most_specific(policy->find("unattended.rules"), call.tool);
    for (const auto& [pattern, floor] : floors) {
        if (!glob_match(pattern, call.tool) || (floor.unattended_only && !call.unattended)) continue;
        const bool lifted = call.unattended && floor.unattended_rule_lowers && unattended_rule && unattended_rule->decision == Decision::allow;
        if (!lifted) b.at_least(floor.min, "tool:" + pattern, "tool", call.tool + " always needs approval");
    }
    if (call.skill) {
        for (const auto& p : call.skill->interrupt_on)
            if (glob_match(p, call.tool)) b.at_least(Decision::approve, "skill.interrupt_on[" + p + "]", "skill", "the Skill asks for approval of " + call.tool);
        const int max = risk_rank(call.skill->max_risk);
        if (max >= 0 && risk_rank(risk) > max)
            b.at_least(Decision::approve, "skill.risk", "skill", "the call is above the Risk class the Skill declares");
    }

    // 4. Unattended runs: the stricter of the result so far and the unattended rules.
    if (call.unattended) {
        if (unattended_rule) {
            const std::string rule = entry_path("unattended.rules", unattended_rule->pattern);
            b.at_least(unattended_rule->decision, rule, policy->source(rule), "decided by the unattended rule " + unattended_rule->pattern);
        }
        const auto allowed = policy->strings("unattended.allowed_risk");
        const bool in_allowed = std::find(allowed.begin(), allowed.end(), risk) != allowed.end();
        const bool explicitly_allowed = unattended_rule && unattended_rule->decision == Decision::allow;
        if (!in_allowed && !explicitly_allowed) {
            b.at_least(Decision::approve, "unattended.allowed_risk",
                       policy->is_set("unattended.allowed_risk") ? policy->source("unattended.allowed_risk") : "default",
                       risk + " actions need approval in runs that start without you");
        }
    }
    // 5. Final: untrusted content has no path to lower it.
    return b.v;
}

}  // namespace wb::policy
