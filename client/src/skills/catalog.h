// The Skill catalogue (client/skills Skill Load Tiers): the Skills on disk in their five tiers, resolved by precedence
// and policy.skills.locked, with what each copy is (active, shadowed, conflicting, invalid, unavailable) for the Skills
// view, the usable ones for the agent and the composer, and the active copy of one name for a run.
//
// Layout (add-skills design D5):
//   <builtin dir>/<name>/SKILL.md                      shipped with the application
//   <data dir>/private/<name>/SKILL.md                 the user's own
//   <data dir>/{org,group,shared}/<name>/<version>/    installed from the registry; <name>/pinned names the version in
//                                                      use (else the highest), <version>/install.json its provenance
#pragma once

#include "skills/skill.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::skills {

struct CatalogEnv {
    std::filesystem::path builtin_dir;
    std::filesystem::path data_dir;  // <data root>/skills
    std::function<std::vector<std::string>()> locked;  // policy.skills.locked
    // Skill MCP Requirements: why a server cannot be used now, or nullopt.
    std::function<std::optional<std::string>(const std::string& server)> mcp_unavailable;
    // The registered tools with their risk and whether policy permits them (for the tool scope shown in the view).
    std::function<std::vector<ToolFacts>()> tools;
    // Load-time check of a registry copy (signature and integrity, SkillSync::verify_installed); an error makes the
    // copy invalid.
    std::function<std::optional<ParseError>(Tier tier, const std::filesystem::path& folder)> verify;
};

// One usable Skill: the active copy of its name.
struct Usable {
    Parsed skill;
    Tier tier = Tier::priv;
    std::string folder;
};

inline constexpr std::size_t kMaxSkillMdBytes = 256 * 1024;

std::string path_utf8(const std::filesystem::path& p);
std::filesystem::path utf8_path(std::string_view s);

// Reads one Skill folder (SKILL.md at its root). The folder name must be the Skill's name.
Loaded load_folder(const std::filesystem::path& folder, Tier tier, const std::string& folder_name);

class Catalog {
public:
    explicit Catalog(CatalogEnv env);

    // Reads every tier from disk again (after an install, an edit, a removal).
    void reload();

    // The Skills view: one entry per name, [{name, description, version, tier, folder, state, error, unavailable,
    // risk, network, tools, unavailable_tools, requires_mcp, source, others: [{tier, version, state, error, folder}]}].
    [[nodiscard]] json list() const;
    // The usable Skills (active, not unavailable), by name.
    [[nodiscard]] std::vector<Usable> usable() const;
    [[nodiscard]] std::optional<Usable> find(const std::string& name) const;

private:
    // nullopt when usable, else why not ("requires jira-corp").
    [[nodiscard]] std::optional<std::string> unavailable(const Manifest& m) const;
    [[nodiscard]] std::vector<std::string> locked() const;

    CatalogEnv env_;
    mutable std::mutex mx_;
    std::shared_ptr<const std::vector<Loaded>> loaded_;
};

}  // namespace wb::skills
