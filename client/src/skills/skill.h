// Skills (client/skills): the SKILL.md package format, the five load tiers with precedence and locking, and the tool
// scope of a Skill's subagent. Portable and unit-testable; loading from disk and the registry live in wb_skills.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wb::skills {

// The resolutions of an Approval (client/agent-runtime Approval Interrupts), as named in interrupt_on.
inline constexpr const char* kResolutions[] = {"approve", "approve_edited", "reject", "respond_instead"};

// One interrupt_on rule: calls of tools matching `tool` need approval; `resolutions` (empty: all) restricts how the
// user may resolve it.
struct InterruptRule {
    std::string tool;
    std::vector<std::string> resolutions;
};

struct Manifest {
    std::string name, description;
    std::string version = "0.0.0";
    bool version_declared = false;
    std::optional<std::vector<std::string>> tools;  // absent: the default Skill tool set
    std::string risk = "read";                      // read | write-local | external | destructive
    std::string network = "proxy";                  // none | proxy
    std::vector<std::string> requires_mcp;
    std::vector<InterruptRule> interrupt_on;
    json triggers = json::array();                  // suggested Trigger definitions (client/scheduler)
    json frontmatter = json::object();              // every field as parsed, unknown ones included
};

// A validation error names the field (Skills view; registry review).
struct ParseError {
    std::string field;
    std::string message;
};

struct Parsed {
    Manifest manifest;
    std::string body;  // the instructions (Markdown)
};

// Parses SKILL.md. `require_version`: the registry's rule (shared, group, org); private and built-in Skills may omit
// it and get 0.0.0 (Agent Skills Compatibility).
std::variant<Parsed, ParseError> parse_skill_md(std::string_view text, bool require_version = false);

[[nodiscard]] bool valid_name(std::string_view name);
[[nodiscard]] bool valid_version(std::string_view version);
// <0, 0, >0 like strcmp, by semantic version precedence; invalid versions compare equal.
int compare_versions(std::string_view a, std::string_view b);
[[nodiscard]] int risk_rank(std::string_view risk);  // read 0 ... destructive 3; unknown -1

// ---- tiers -------------------------------------------------------------------------------------------------

enum class Tier { builtin, org, group, shared, priv };  // lowest to highest precedence

const char* to_string(Tier t);
std::optional<Tier> parse_tier(std::string_view s);

// A Skill as loaded from one tier (a folder with SKILL.md, or one that failed to parse).
struct Loaded {
    Tier tier = Tier::priv;
    std::string name;      // the folder's name when the manifest did not parse
    std::string folder;    // where its files are
    std::optional<Parsed> skill;
    std::optional<ParseError> error;
    std::string source;    // registry provenance: group name for the group tier, author for shared
};

// The resolved view: the Skill in use per name, and what each other copy is.
struct Entry {
    const Loaded* active = nullptr;  // null: no usable copy (all invalid)
    struct Other {
        const Loaded* skill;
        std::string state;  // shadowed | conflicting (shadows a locked org Skill) | invalid
    };
    std::vector<Other> others;
};

// Precedence built-in < org < group < shared < private; org Skills named in policy.skills.locked win over every
// other tier, and the copies that would have shadowed them are conflicting. Invalid copies never win. The entries
// point into `loaded`, which must outlive the result.
std::map<std::string, Entry> resolve_tiers(const std::vector<Loaded>& loaded, const std::vector<std::string>& locked);

// ---- tool scope ----------------------------------------------------------------------------------------------

struct ToolFacts {
    std::string id;
    std::string risk;     // declared risk class
    bool permitted = false;  // the user's policy offers it (PolicyHook::tool_permitted)
};

struct Scope {
    std::vector<std::string> tools;        // the subagent's tool set
    std::vector<std::string> unavailable;  // declared but not permitted (shown in the Skills view)
};

// Skill Tool Scope: declared tools (exact ids or `*` patterns) intersected with the permitted tools; without a tools
// field, the permitted read-class tools.
Scope tool_scope(const Manifest& m, const std::vector<ToolFacts>& tools);

// The resolutions an Approval of `tool` accepts under the Skill's interrupt_on rules (empty: all). Several matching
// rules intersect.
std::vector<std::string> allowed_resolutions(const Manifest& m, std::string_view tool);

// The tool-id glob used by tools, interrupt_on and policy maps (`*` any run of characters).
[[nodiscard]] bool tool_matches(std::string_view pattern, std::string_view tool);

}  // namespace wb::skills
