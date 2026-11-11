// The agent's save of a private Skill (client/skills Agent Skill Save; add-agent-created-skills design D1, D4 to D9 and
// D12). Portable: the host wires the tool, the provenance record, the catalogue and the user's names. Writes go through
// the Skill editor's save path (save_private) and its validator, so the editor and the agent cannot drift apart.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "skills/authoring.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wb::skills {

// The arguments of `skills.save`. There is no path argument: the client chooses the folder (design D1).
struct AgentSkillArgs {
    std::string name, description, instructions;
    std::string version;                                        // empty: 1.0.0
    std::optional<std::vector<std::string>> tools;              // absent: the default Skill tool set
    std::optional<std::string> risk;                            // absent: read
    std::optional<std::string> network;                         // absent: the Skill default
    std::vector<std::pair<std::string, std::string>> files;     // relative path, text content
    bool update = false;
};

// Reads the tool's JSON arguments. An argument that names a location is refused (design D2).
Result<AgentSkillArgs> parse_agent_args(const json& args);

// What the host provides. Folders: <data>/skills/private and <data>/skills/previous (one kept version per name).
struct AgentSkillEnv {
    std::filesystem::path private_dir;
    std::filesystem::path previous_dir;
    // The tier of a usable Skill of another tier with this name ("org", "group", "shared", "builtin"), when there is one.
    std::function<std::optional<std::string>(const std::string& name)> other_tier;
    // Whether the agent created the private Skill (its provenance record exists; design D6).
    std::function<bool(const std::string& name)> created_by_agent;
    // Writes or updates the provenance record after a successful save.
    std::function<void(const std::string& name, const std::string& thread_id, bool created)> record;
    // The user's own names, for personal data findings (design D8).
    std::function<std::vector<std::string>()> user_names;
    std::function<void()> reload;  // the catalogue reads the disk again
};

// A save that passed every check, ready to show in the approval and to write.
struct PreparedSave {
    AgentSkillArgs args;
    std::string text;         // SKILL.md
    bool exists = false;      // an agent-created private Skill of this name is stored: this is an update
    std::string stored_text;  // the stored SKILL.md when exists
    std::vector<Replacement> findings;  // personal data found (shown, never replaced; design D8)
};

// Checks a save without writing anything: arguments, secret scan, files, SKILL.md validation, name collisions and the
// version rule. The error names the field and never shows a secret value.
Result<PreparedSave> prepare_agent_save(const AgentSkillEnv& env, const AgentSkillArgs& args);

// The part of the approval payload that the save adds to the arguments: the files with sizes, the personal data
// findings and, for an update, the changed lines against the stored text (design D4).
json agent_save_preview(const PreparedSave& p);

// Writes the Skill (create or update). An update keeps the replaced version as the one kept version (design D12).
Result<json> commit_agent_save(const AgentSkillEnv& env, const PreparedSave& p, const std::string& thread_id);

// Swaps the current private Skill with its kept version (design D12, Restore previous version).
Status restore_previous(const AgentSkillEnv& env, const std::string& name);
[[nodiscard]] bool has_previous(const AgentSkillEnv& env, const std::string& name);
// Deletes the kept version of a Skill (on deletion of the Skill in the editor).
Status forget_previous(const AgentSkillEnv& env, const std::string& name);

// The secret rules (design D8): a field that holds a key, token, password or private key. Returns the field name or
// nullopt. Exposed for tests.
std::optional<std::string> secret_field(const AgentSkillArgs& args);
[[nodiscard]] bool has_secret(std::string_view text);

// The changed lines between two texts, "- " for removed and "+ " for added lines, in order (design D4).
std::vector<std::string> changed_lines(std::string_view before, std::string_view after);

}  // namespace wb::skills
