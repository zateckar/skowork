// The decision function of client/policy-enforcement (Tool Call Decision; design D3): one deterministic
// decide(call, policy) -> allow | approve | deny, with the rule that decided it. Portable and unit-testable.
#pragma once

#include "policy/core.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wb::policy {

// A file or URL a call uses; the argument-level checks run on each.
struct Resource {
    enum class Kind { read_path, write_path, url };
    Kind kind = Kind::read_path;
    std::string value;
};

// What a Skill declares for its subagent's calls (client/skills interrupt_on and risk).
struct SkillFloor {
    std::vector<std::string> interrupt_on;  // tool-id patterns that need approval
    std::string max_risk;                   // calls above this Risk class are at least approve
};

// A minimum decision a tool's capability spec declares (Tool-Declared Decision Floor), for example mail.send.
struct ToolFloor {
    Decision min = Decision::allow;
    // The tool spec names policy.unattended.rules as the only way to lower its floor: an explicit allow there lifts the
    // floor in Unattended runs (design D3 step 4).
    bool unattended_rule_lowers = false;
    // The floor applies in Unattended runs only (client/computer-use Unattended Computer Use).
    bool unattended_only = false;
};
using ToolFloors = std::map<std::string, ToolFloor>;  // tool-id pattern -> floor

// The floors declared by the capability specs known to this build.
const ToolFloors& declared_tool_floors();

struct CallFacts {
    std::string tool;
    std::string risk;          // read | write-local | external | destructive
    bool unattended = false;
    bool own_data = false;     // the tool touches only the client's own data (Threads, attachments)
    std::vector<Resource> resources;
    std::optional<SkillFloor> skill;
};

// Checks one resource (filesystem rules for paths, egress rules for URLs). Absent: resources are not checked.
using ResourceCheck = std::function<RuleVerdict(const Resource&)>;

struct Verdict {
    Decision decision = Decision::deny;
    std::string rule;    // the rule that decided, for example tools.rules["browser.*"], tools.risk["external"]
    std::string source;  // the Group, guardrail, "default", "tool", "skill" or "fail-closed"
    std::string reason;  // readable explanation
    long long policy_version = 0;
};

// `policy` null means no valid bundle: only reads of the client's own data are allowed (Cached Policy And Fail-Closed).
Verdict decide(const EffectivePolicy* policy, const CallFacts& call, const ToolFloors& floors = declared_tool_floors(),
               const ResourceCheck& check = {});

// The most specific matching entry of a decision map ({pattern: decision}); ties go to the stricter decision.
struct MapMatch {
    std::string pattern;
    Decision decision = Decision::allow;
};
std::optional<MapMatch> most_specific(const json* map, std::string_view tool);

}  // namespace wb::policy
