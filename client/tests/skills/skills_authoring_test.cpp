// Skill authoring (client/skills Skill Authoring, Save Thread As Skill, Skill Publishing; add-skills tasks 4.1, 4.2,
// 4.4, design D10): SKILL.md in two parts, private Skills and their files on disk, the package zip, and drafts from
// sample Threads whose names, email addresses and paths become parameters.
#include "skills/agent_save.h"
#include "skills/authoring.h"
#include "skills/catalog.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <random>
#include <set>

using namespace wb;
using namespace wb::skills;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("skowork-authoring-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

SkillSource source(const std::string& name, const std::string& description = "Writes the weekly report") {
    return {"name: " + name + "\ndescription: " + description + "\n", "Collect the notes and summarise them.\n"};
}

}  // namespace

TEST(SkillAuthoring, SplitAndJoinKeepTheTwoParts) {
    const auto s = split_skill_md("---\r\nname: a\r\ndescription: b\r\n---\r\n\r\nBody line\n");
    EXPECT_EQ(s.frontmatter, "name: a\r\ndescription: b\r\n");
    EXPECT_EQ(s.body, "Body line\n");  // the blank line after the block belongs to the separator
    EXPECT_EQ(split_skill_md("No frontmatter").body, "No frontmatter");
    EXPECT_TRUE(split_skill_md("No frontmatter").frontmatter.empty());
    EXPECT_EQ(split_skill_md("---\nname: a\nno end").body, "---\nname: a\nno end");
    const SkillSource round{"name: a\ndescription: b", "Do it.\n"};
    EXPECT_EQ(join_skill_md(round), "---\nname: a\ndescription: b\n---\n\nDo it.\n");
    const auto back = split_skill_md(join_skill_md(round));
    EXPECT_EQ(back.frontmatter, "name: a\ndescription: b\n");
    EXPECT_EQ(back.body, "Do it.\n");
}

TEST(SkillAuthoring, SavingCreatesRenamesAndRefusesTakenNames) {
    TempDir tmp;
    const fs::path priv = tmp.path / "private";
    auto saved = save_private(priv, "", source("weekly-report"));
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_EQ(*saved, "weekly-report");
    EXPECT_NE(read(priv / "weekly-report" / "SKILL.md").find("name: weekly-report"), std::string::npos);
    // The catalogue reads it as a valid private Skill.
    const auto loaded = load_folder(priv / "weekly-report", Tier::priv, "weekly-report");
    ASSERT_TRUE(loaded.skill.has_value());

    // Invalid frontmatter is refused with its field, and nothing is written.
    auto bad = save_private(priv, "", {"name: Not Valid\ndescription: x\n", "x"});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, "invalid");
    EXPECT_NE(bad.error().message.find("name"), std::string::npos);

    // A new Skill cannot take an existing name; renaming moves the folder with its files.
    write(priv / "weekly-report" / "scripts" / "collect.ps1", "Get-Date");
    EXPECT_EQ(save_private(priv, "", source("weekly-report")).error().code, "exists");
    ASSERT_TRUE(save_private(priv, "weekly-report", source("status-report")));
    EXPECT_FALSE(fs::exists(priv / "weekly-report"));
    EXPECT_TRUE(fs::exists(priv / "status-report" / "scripts" / "collect.ps1"));
    ASSERT_TRUE(save_private(priv, "", source("other")));
    EXPECT_EQ(save_private(priv, "status-report", source("other")).error().code, "exists");
    // Editing in place.
    ASSERT_TRUE(save_private(priv, "status-report", source("status-report", "Writes the status report")));
    EXPECT_NE(read(priv / "status-report" / "SKILL.md").find("Writes the status report"), std::string::npos);

    ASSERT_TRUE(delete_private(priv, "status-report"));
    EXPECT_FALSE(fs::exists(priv / "status-report"));
    EXPECT_EQ(delete_private(priv, "status-report").error().code, "not_found");
    EXPECT_EQ(delete_private(priv, "../x").error().code, "invalid_params");
}

TEST(SkillAuthoring, FilesAreListedAddedAndRemovedWithinTheSkillFolders) {
    TempDir tmp;
    const fs::path priv = tmp.path / "private";
    ASSERT_TRUE(save_private(priv, "", source("weekly-report")));
    const fs::path folder = priv / "weekly-report";
    write(tmp.path / "in" / "collect.ps1", "Get-Date");
    write(tmp.path / "in" / "template.md", "# Week");
    ASSERT_TRUE(add_files(folder, "scripts", {tmp.path / "in" / "collect.ps1"}));
    ASSERT_TRUE(add_files(folder, "references", {tmp.path / "in" / "template.md"}));
    EXPECT_EQ(add_files(folder, "other", {tmp.path / "in" / "template.md"}).error().code, "invalid_params");
    EXPECT_EQ(add_files(priv / "missing", "scripts", {tmp.path / "in" / "collect.ps1"}).error().code, "not_found");
    PackageLimits small;
    small.max_unpacked = 10;
    EXPECT_EQ(add_files(folder, "assets", {tmp.path / "in" / "template.md"}, small).error().code, "too_large");
    const auto files = list_files(folder);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].path, "references/template.md");
    EXPECT_EQ(files[1].path, "scripts/collect.ps1");
    EXPECT_EQ(files[1].size, 8u);

    // The package has SKILL.md at the root and reads back with the registry's rules.
    write(folder / "README.md", "Read me");
    write(folder / "notes.txt", "not packaged");
    auto zip = zip_folder(folder);
    ASSERT_TRUE(zip) << zip.error().message;
    auto pkg = read_package(*zip);
    ASSERT_TRUE(pkg) << pkg.error().message;
    EXPECT_EQ(pkg->size(), 4u);
    EXPECT_TRUE(pkg->count("SKILL.md"));
    EXPECT_TRUE(pkg->count("README.md"));
    EXPECT_EQ(pkg->at("scripts/collect.ps1"), "Get-Date");
    EXPECT_FALSE(pkg->count("notes.txt"));

    EXPECT_EQ(remove_file(folder, "../SKILL.md").error().code, "invalid_params");
    EXPECT_EQ(remove_file(folder, "SKILL.md").error().code, "invalid_params");
    ASSERT_TRUE(remove_file(folder, "scripts/collect.ps1"));
    EXPECT_EQ(list_files(folder).size(), 1u);
}

TEST(SkillAuthoring, PersonalDataBecomesParameters) {
    const auto p = parameterise(
        "Send the report to jan.novak@example.com and petra@example.com, cc jan.novak@example.com. Save it to "
        "C:\\Users\\jnovak\\Documents\\Weekly\\report.docx. Then copy \\\\fs01\\team\\reports\\w43. Ask Petra Svobodová.",
        {"Petra Svobodová", "Jan Novák"});
    EXPECT_EQ(p.text,
              "Send the report to {{email_1}} and {{email_2}}, cc {{email_1}}. Save it to {{path_1}}. Then copy {{path_2}}. "
              "Ask {{person_1}}.");
    ASSERT_EQ(p.replaced.size(), 5u);
    EXPECT_EQ(p.replaced[2].original, "C:\\Users\\jnovak\\Documents\\Weekly\\report.docx");
    EXPECT_EQ(p.replaced[3].original, "\\\\fs01\\team\\reports\\w43");
    EXPECT_EQ(p.replaced[4].kind, "person");
    // Whole words only, and the same value keeps its parameter across texts.
    const auto q = parameterise("Jan Nováková met Petra Svobodová", {"Petra Svobodová", "Jan Novák"}, p.replaced);
    EXPECT_EQ(q.text, "Jan Nováková met {{person_1}}");
    EXPECT_EQ(names_in("From: Jan Novák <jan.novak@example.com>\nTo: \"Svobodová, Petra\" <petra@x.cz>, bob@x.cz"),
              (std::vector<std::string>{"Jan Novák", "Svobodová, Petra"}));
    EXPECT_EQ(names_in("mail the summary to Petr Dvořák <p@x.cz> and to the team <t@x.cz>"), std::vector<std::string>{"Petr Dvořák"});
    EXPECT_EQ(kebab_name("Weekly Report!"), "weekly-report");
    EXPECT_EQ(kebab_name("Žádost"), "dost");
    EXPECT_EQ(kebab_name("!!"), "my-skill");
}

// Sample Threads: the draft generalises them, and what the model left in is parameterised.
TEST(SkillAuthoring, DraftFromSampleThreadsParameterisesNamesEmailsAndPaths) {
    ThreadDigest t;
    t.title = "Weekly report for Petr";
    t.messages = {{"user", "Collect the notes in C:\\Users\\jnovak\\Notes and mail the summary to Petr Dvořák <petr.dvorak@example.com>"},
                  {"assistant", "Done: the summary went to petr.dvorak@example.com."}};
    t.tools = {"files.read", "mail.send"};
    t.risk = "external";
    t.names = {"Jan Novák"};
    std::string asked_system, asked_user;
    auto d = draft_skill(t, [&](const std::string& system, const std::string& user) -> Result<std::string> {
        asked_system = system;
        asked_user = user;
        // A model that left personal data in.
        return std::string(R"(Here it is: {"name": "Weekly Report", "description": "Mails Petr Dvořák the weekly summary",
            "instructions": "1. Read the notes in C:\\Users\\jnovak\\Notes.\n2. Mail the summary to petr.dvorak@example.com, signed Jan Novák.\n3. Use {{week}}.",
            "inputs": [{"name": "{{week}}", "description": "the week number"}]})");
    });
    ASSERT_TRUE(d) << d.error().message;
    EXPECT_NE(asked_system.find("{{parameter}}"), std::string::npos);
    EXPECT_NE(asked_user.find("Tools used: files.read mail.send"), std::string::npos);
    EXPECT_NE(asked_user.find("[assistant] Done"), std::string::npos);
    const auto& fm = d->source.frontmatter;
    EXPECT_NE(fm.find("name: weekly-report\n"), std::string::npos) << fm;
    EXPECT_NE(fm.find("description: \"Mails {{person_1}} the weekly summary\"\n"), std::string::npos) << fm;
    EXPECT_NE(fm.find("risk: external\n"), std::string::npos);
    EXPECT_NE(fm.find("tools:\n  - files.read\n  - mail.send\n"), std::string::npos);
    const auto& body = d->source.body;
    EXPECT_EQ(body.find("Dvořák"), std::string::npos) << body;
    EXPECT_EQ(body.find("@example.com"), std::string::npos) << body;
    EXPECT_EQ(body.find("jnovak"), std::string::npos) << body;
    EXPECT_EQ(body.find("Jan Novák"), std::string::npos) << body;
    EXPECT_NE(body.find("1. Read the notes in {{path_1}}."), std::string::npos) << body;
    EXPECT_NE(body.find("## Inputs\n\n- `{{week}}`: the week number\n"), std::string::npos) << body;
    EXPECT_NE(body.find("- `{{email_1}}`: an email address\n"), std::string::npos) << body;
    ASSERT_EQ(d->replaced.size(), 4u);
    // The draft is a valid Skill.
    ASSERT_TRUE(std::holds_alternative<Parsed>(parse_skill_md(join_skill_md(d->source))));

    // Keep puts a value back and drops its input line.
    const Replacement person = d->replaced[0];
    EXPECT_EQ(person.original, "Petr Dvořák");
    const auto kept = keep_original(d->source, person);
    EXPECT_NE(kept.frontmatter.find("description: \"Mails Petr Dvořák the weekly summary\""), std::string::npos) << kept.frontmatter;
    const auto path = *std::find_if(d->replaced.begin(), d->replaced.end(), [](const Replacement& r) { return r.kind == "path"; });
    const auto kept_path = keep_original(kept, path);
    EXPECT_NE(kept_path.body.find("1. Read the notes in C:\\Users\\jnovak\\Notes."), std::string::npos) << kept_path.body;
    EXPECT_EQ(kept_path.body.find("{{" + path.parameter + "}}"), std::string::npos);
    EXPECT_TRUE(std::holds_alternative<Parsed>(parse_skill_md(join_skill_md(kept_path))));

    // An unreadable answer is an error.
    EXPECT_EQ(draft_skill(t, [](const std::string&, const std::string&) -> Result<std::string> { return std::string("sorry"); }).error().code,
              "draft_failed");
}

// ---- skills.save: the agent's save of a private Skill (add-agent-created-skills tasks 2.1, 2.2, 2.4, 2.9) ------------

namespace {

// A save environment on a temporary folder, with a provenance ledger and a set of other-tier Skills.
struct AgentEnv {
    TempDir dir;
    std::set<std::string> ledger;               // names the agent created
    std::map<std::string, std::string> tiers;   // usable Skills of other tiers by name
    int reloads = 0;
    AgentSkillEnv env() {
        AgentSkillEnv e;
        e.private_dir = dir.path / "private";
        e.previous_dir = dir.path / "previous";
        e.other_tier = [this](const std::string& n) -> std::optional<std::string> {
            const auto it = tiers.find(n);
            if (it == tiers.end()) return std::nullopt;
            return it->second;
        };
        e.created_by_agent = [this](const std::string& n) { return ledger.count(n) > 0; };
        e.record = [this](const std::string& n, const std::string&, bool) { ledger.insert(n); };
        e.user_names = [] { return std::vector<std::string>{"Anna Novakova"}; };
        e.reload = [this] { ++reloads; };
        return e;
    }
};

json agent_args(const std::string& name, const std::string& version = "1.0.0", bool update = false,
                const std::string& instructions = "Collect the notes and summarise them.\n") {
    return json{{"name", name}, {"description", "Writes the weekly report"}, {"instructions", instructions},
                {"version", version}, {"update", update}};
}

// Parses, checks and writes one save, the way the tool does.
Result<json> run_save(const AgentSkillEnv& env, const json& args) {
    auto a = parse_agent_args(args);
    if (!a) return a.error();
    auto p = prepare_agent_save(env, *a);
    if (!p) return p.error();
    return commit_agent_save(env, *p, "thread-1");
}

Error refusal(const AgentSkillEnv& env, const json& args) {
    auto r = run_save(env, args);
    EXPECT_FALSE(r);
    return r ? Error{} : r.error();
}

}  // namespace

TEST(AgentSkillSave, CreatesAPrivateSkillWithoutAPath) {
    AgentEnv t;
    const auto env = t.env();
    auto r = run_save(env, agent_args("weekly-report"));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ((*r)["status"], "created");
    EXPECT_EQ((*r)["tier"], "private");
    const auto folder = env.private_dir / "weekly-report";
    ASSERT_TRUE(fs::exists(folder / "SKILL.md"));
    EXPECT_TRUE(t.ledger.count("weekly-report") > 0);
    EXPECT_EQ(t.reloads, 1);
    auto parsed = parse_skill_md(read(folder / "SKILL.md"), false);
    ASSERT_TRUE(std::holds_alternative<Parsed>(parsed));
    EXPECT_EQ(std::get<Parsed>(parsed).manifest.name, "weekly-report");
    EXPECT_EQ(std::get<Parsed>(parsed).manifest.version, "1.0.0");
}

TEST(AgentSkillSave, RefusesAnyLocationArgument) {
    for (const char* key : {"path", "folder", "directory"}) {
        json j = agent_args("weekly-report");
        j[key] = "C:\\Users\\x";
        EXPECT_FALSE(parse_agent_args(j)) << key;
    }
}

TEST(AgentSkillSave, InvalidFrontmatterNamesTheField) {
    AgentEnv t;
    const auto env = t.env();
    const auto bad_name = refusal(env, agent_args("Weekly Report"));
    EXPECT_EQ(bad_name.code, "invalid");
    EXPECT_NE(bad_name.message.find("name"), std::string::npos);
    json no_description = agent_args("weekly-report");
    no_description["description"] = "";
    EXPECT_NE(refusal(env, no_description).message.find("description"), std::string::npos);
    EXPECT_FALSE(fs::exists(env.private_dir / "weekly-report"));
}

TEST(AgentSkillSave, SecretInInstructionsIsRefusedWithoutTheValue) {
    AgentEnv t;
    const auto env = t.env();
    const std::string key = "sk-proj-AbC123dEf456GhI789jKl012";
    const auto err = refusal(env, agent_args("weekly-report", "1.0.0", false, "Call the API with api_key: " + key + "\n"));
    EXPECT_EQ(err.code, "secret");
    EXPECT_NE(err.message.find("instructions"), std::string::npos);
    EXPECT_EQ(err.message.find(key), std::string::npos);
    EXPECT_FALSE(fs::exists(env.private_dir / "weekly-report"));
}

TEST(AgentSkillSave, SecretRulesAcceptPlainProse) {
    EXPECT_FALSE(has_secret("Ask the user for the password reset steps."));
    EXPECT_FALSE(has_secret("Read the token name from the config and use {{api_key}} as a placeholder."));
    EXPECT_FALSE(has_secret("Summarise the notes in 40 short lines of plain Markdown text for the team."));
    EXPECT_TRUE(has_secret("password = hunter2hunter2"));
    EXPECT_TRUE(has_secret("-----BEGIN RSA PRIVATE KEY-----"));
    EXPECT_TRUE(has_secret("AKIAIOSFODNN7EXAMPLE1 is the key"));
}

TEST(AgentSkillSave, BinaryFileIsRefused) {
    AgentEnv t;
    const auto env = t.env();
    json j = agent_args("weekly-report");
    j["files"] = json::array({{{"path", "assets/logo.bin"}, {"content", std::string("PNG\0\x01\x02", 6)}}});
    EXPECT_EQ(refusal(env, j).code, "binary");
    EXPECT_FALSE(fs::exists(env.private_dir / "weekly-report"));
}

TEST(AgentSkillSave, UnsafeFilePathIsRefused) {
    AgentEnv t;
    const auto env = t.env();
    for (const char* path : {"../escape.md", "SKILL.md", "other/notes.md", "scripts/../../x.md", "C:\\x.md"}) {
        json j = agent_args("weekly-report");
        j["files"] = json::array({{{"path", path}, {"content", "text"}}});
        EXPECT_EQ(refusal(env, j).code, "unsafe_path") << path;
    }
    EXPECT_FALSE(fs::exists(env.private_dir / "weekly-report"));
}

TEST(AgentSkillSave, TextFilesUnderTheResourceFoldersAreWritten) {
    AgentEnv t;
    const auto env = t.env();
    json j = agent_args("weekly-report");
    j["files"] = json::array({{{"path", "references/template.md"}, {"content", "# Template\n"}},
                              {{"path", "scripts/run.ps1"}, {"content", "Write-Output 1\n"}}});
    ASSERT_TRUE(run_save(env, j));
    const auto files = list_files(env.private_dir / "weekly-report");
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].path, "references/template.md");
    EXPECT_EQ(read(env.private_dir / "weekly-report" / "references" / "template.md"), "# Template\n");
}

TEST(AgentSkillSave, FileLimitsAreTheEditorsPackageLimits) {
    AgentEnv t;
    const auto env = t.env();
    json j = agent_args("weekly-report");
    const std::string big(21u * 1024u * 1024u, 'a');  // above the 20 MB unpacked limit
    j["files"] = json::array({{{"path", "assets/big.txt"}, {"content", big}}});
    EXPECT_EQ(refusal(env, j).code, "too_large");
    json many = agent_args("many-files");
    many["files"] = json::array();
    for (int i = 0; i < 200; ++i) many["files"].push_back({{"path", "assets/f" + std::to_string(i) + ".txt"}, {"content", "x"}});
    EXPECT_EQ(refusal(env, many).code, "too_large");
}

TEST(AgentSkillSave, NameInUseByAnotherTierIsRefusedNamingTheTier) {
    AgentEnv t;
    t.tiers["weekly-report"] = "org";
    const auto env = t.env();
    const auto err = refusal(env, agent_args("weekly-report"));
    EXPECT_EQ(err.code, "name_in_use");
    EXPECT_NE(err.message.find("org"), std::string::npos);
    EXPECT_FALSE(fs::exists(env.private_dir / "weekly-report"));
}

TEST(AgentSkillSave, UsersOwnSkillIsNeverReplaced) {
    AgentEnv t;
    const auto env = t.env();
    write(env.private_dir / "weekly-report" / "SKILL.md", "---\nname: weekly-report\ndescription: Mine\nversion: 1.0.0\n---\n\nMy own text.\n");
    const std::string before = read(env.private_dir / "weekly-report" / "SKILL.md");
    EXPECT_EQ(refusal(env, agent_args("weekly-report", "2.0.0", true)).code, "name_in_use");
    EXPECT_EQ(read(env.private_dir / "weekly-report" / "SKILL.md"), before);
    EXPECT_FALSE(fs::exists(env.previous_dir / "weekly-report"));
}

TEST(AgentSkillSave, ExistingAgentSkillNeedsUpdateAndAHigherVersion) {
    AgentEnv t;
    const auto env = t.env();
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.0.0")));
    EXPECT_EQ(refusal(env, agent_args("weekly-report", "1.1.0", false)).code, "exists");
    EXPECT_EQ(refusal(env, agent_args("weekly-report", "1.0.0", true)).code, "version");
    EXPECT_EQ(refusal(env, agent_args("weekly-report", "0.9.0", true)).code, "version");
    auto ok = run_save(env, agent_args("weekly-report", "1.1.0", true));
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ((*ok)["status"], "updated");
}

TEST(AgentSkillSave, UpdateKeepsTheReplacedVersionAndOnlyTheLatest) {
    AgentEnv t;
    const auto env = t.env();
    json v1 = agent_args("weekly-report", "1.0.0", false, "First text.\n");
    v1["files"] = json::array({{{"path", "references/a.md"}, {"content", "old file\n"}}});
    ASSERT_TRUE(run_save(env, v1));
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.1.0", true, "Second text.\n")));
    const auto kept = env.previous_dir / "weekly-report";
    EXPECT_NE(read(kept / "SKILL.md").find("First text."), std::string::npos);
    EXPECT_TRUE(fs::exists(kept / "references" / "a.md"));  // the replaced files are kept too
    EXPECT_TRUE(has_previous(env, "weekly-report"));
    // The update removed the replaced resource files from the current version.
    EXPECT_FALSE(fs::exists(env.private_dir / "weekly-report" / "references" / "a.md"));

    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.2.0", true, "Third text.\n")));
    EXPECT_NE(read(kept / "SKILL.md").find("Second text."), std::string::npos);
    EXPECT_EQ(read(kept / "SKILL.md").find("First text."), std::string::npos);
}

TEST(AgentSkillSave, RestoreSwapsCurrentAndKeptVersions) {
    AgentEnv t;
    const auto env = t.env();
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.0.0", false, "First text.\n")));
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.1.0", true, "Second text.\n")));
    ASSERT_TRUE(restore_previous(env, "weekly-report"));
    EXPECT_NE(read(env.private_dir / "weekly-report" / "SKILL.md").find("First text."), std::string::npos);
    EXPECT_NE(read(env.previous_dir / "weekly-report" / "SKILL.md").find("Second text."), std::string::npos);
    ASSERT_TRUE(restore_previous(env, "weekly-report"));  // and back again
    EXPECT_NE(read(env.private_dir / "weekly-report" / "SKILL.md").find("Second text."), std::string::npos);
    EXPECT_FALSE(restore_previous(env, "no-such-skill"));
}

TEST(AgentSkillSave, DeletingTheKeptVersionRemovesIt) {
    AgentEnv t;
    const auto env = t.env();
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.0.0")));
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.1.0", true)));
    ASSERT_TRUE(has_previous(env, "weekly-report"));
    ASSERT_TRUE(forget_previous(env, "weekly-report"));
    EXPECT_FALSE(has_previous(env, "weekly-report"));
}

TEST(AgentSkillSave, ApprovalPreviewListsFindingsFilesAndChanges) {
    AgentEnv t;
    const auto env = t.env();
    ASSERT_TRUE(run_save(env, agent_args("weekly-report", "1.0.0", false, "Send to anna@example.com.\n")));
    json j = agent_args("weekly-report", "1.1.0", true, "Send to anna@example.com.\nSave under C:\\Users\\anna\\out.\n");
    j["files"] = json::array({{{"path", "scripts/run.ps1"}, {"content", "Write-Output 1\n"}}});
    auto a = parse_agent_args(j);
    ASSERT_TRUE(a);
    auto p = prepare_agent_save(env, *a);
    ASSERT_TRUE(p) << p.error().message;
    const json preview = agent_save_preview(*p);
    EXPECT_EQ(preview["instructions"], j["instructions"]);
    EXPECT_EQ(preview["files"][0]["path"], "scripts/run.ps1");
    EXPECT_TRUE(preview["update"].get<bool>());
    std::set<std::string> kinds;
    for (const auto& f : preview["findings"]) kinds.insert(f["kind"].get<std::string>() + ":" + f["original"].get<std::string>());
    EXPECT_TRUE(kinds.count("email:anna@example.com") > 0);
    bool path_found = false;
    for (const auto& f : preview["findings"]) path_found = path_found || f["kind"] == "path";
    EXPECT_TRUE(path_found);
    bool added = false, removed = false;
    for (const auto& line : preview["changes"]) {
        const std::string s = line.get<std::string>();
        added = added || s == "+ Save under C:\\Users\\anna\\out.";
        removed = removed || s == "- version: \"1.0.0\"";
    }
    EXPECT_TRUE(added);
    EXPECT_TRUE(removed);
}

TEST(AgentSkillSave, PersonalDataIsFoundNotReplaced) {
    AgentEnv t;
    const auto env = t.env();
    auto a = parse_agent_args(agent_args("weekly-report", "1.0.0", false, "Report from Anna Novakova to ops@example.com.\n"));
    ASSERT_TRUE(a);
    auto p = prepare_agent_save(env, *a);
    ASSERT_TRUE(p);
    EXPECT_FALSE(p->findings.empty());
    ASSERT_TRUE(commit_agent_save(env, *p, "thread-1"));
    EXPECT_NE(read(env.private_dir / "weekly-report" / "SKILL.md").find("ops@example.com"), std::string::npos);
}

TEST(AgentSkillSave, ChangedLinesAreMarkedAgainstTheOldText) {
    const auto lines = changed_lines("a\nb\nc\n", "a\nB\nc\nd\n");
    const std::vector<std::string> expected = {"- b", "+ B", "+ d"};
    EXPECT_EQ(lines, expected);
}
