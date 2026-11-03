// Skill authoring (client/skills Skill Authoring, Save Thread As Skill, Skill Publishing; add-skills design D10): the
// editor's two parts of SKILL.md, private Skills on disk with their files, the package zip for publishing, and the draft
// of a Skill from a Thread with personal data turned into parameters. Portable; the host wires the model and the UI.
#pragma once

#include "skills/package.h"
#include "skills/skill.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::skills {

// ---- SKILL.md in two parts ---------------------------------------------------------------------------------------

struct SkillSource {
    std::string frontmatter;  // the YAML between the --- lines, without them
    std::string body;         // the instructions
};

// Splits SKILL.md text; text without a frontmatter block is all body.
SkillSource split_skill_md(std::string_view text);
std::string join_skill_md(const SkillSource& s);

// ---- private Skills on disk --------------------------------------------------------------------------------------

// Saves a private Skill under `private_dir` (<data>/skills/private): validates the text, writes <name>/SKILL.md through
// a temporary file, and renames the folder of `original` when the name changed. Refuses a name another private Skill
// already has. Returns the Skill's name.
Result<std::string> save_private(const std::filesystem::path& private_dir, const std::string& original, const SkillSource& s);
Status delete_private(const std::filesystem::path& private_dir, const std::string& name);

struct SkillFile {
    std::string path;  // relative, forward slashes ("scripts/run.ps1")
    std::uintmax_t size = 0;
};
inline constexpr const char* kSkillFolders[] = {"scripts", "references", "assets"};

// The files under scripts/, references/ and assets/ of a Skill folder.
std::vector<SkillFile> list_files(const std::filesystem::path& folder);
// Copies files into one of the Skill folders; the package limits apply to the whole Skill afterwards.
Status add_files(const std::filesystem::path& folder, std::string_view sub, const std::vector<std::filesystem::path>& files,
                 const PackageLimits& limits = {});
Status remove_file(const std::filesystem::path& folder, std::string_view relative);

// The package of a Skill folder for the registry: SKILL.md at the root, the three folders, README and LICENSE files.
Result<std::string> zip_folder(const std::filesystem::path& folder, const PackageLimits& limits = {});

// ---- Save as skill --------------------------------------------------------------------------------------------------

// Personal data replaced by a parameter.
struct Replacement {
    std::string kind;       // email | path | person
    std::string original;   // the value from the Thread
    std::string parameter;  // email_1
};

struct Parameterised {
    std::string text;
    std::vector<Replacement> replaced;
};

// Replaces email addresses, Windows and UNC paths and the given person names by {{parameter}} placeholders. The same
// value gets the same parameter; `known` carries the parameters already assigned (across several texts).
Parameterised parameterise(std::string_view text, const std::vector<std::string>& names, std::vector<Replacement> known = {});

// The display names of "Name <address>" pairs in a text.
std::vector<std::string> names_in(std::string_view text);

// What a Thread offers to the draft.
struct ThreadDigest {
    std::string title;
    std::vector<std::pair<std::string, std::string>> messages;  // role, content (user and assistant)
    std::vector<std::string> tools;                             // the tools its Runs called
    std::string risk = "read";                                  // the highest declared risk among them
    std::vector<std::string> names;                             // person names known to the client (the user's)
};

struct Draft {
    SkillSource source;
    std::vector<Replacement> replaced;
};

// The model call: system and user text in, the answer's text out.
using DraftModel = std::function<Result<std::string>(const std::string& system, const std::string& user)>;

// Drafts a private Skill from a Thread (design D10): one model call for the name, description and generalised
// instructions, then the deterministic parameterisation of personal data.
Result<Draft> draft_skill(const ThreadDigest& thread, const DraftModel& model);

// Keep: puts the original value back for one replacement and drops its line from the Inputs list.
SkillSource keep_original(const SkillSource& s, const Replacement& r);

// A kebab-case Skill name from free text ("Weekly Report!" -> "weekly-report"); "my-skill" when nothing is left.
std::string kebab_name(std::string_view text);

}  // namespace wb::skills
