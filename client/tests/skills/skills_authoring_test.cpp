// Skill authoring (client/skills Skill Authoring, Save Thread As Skill, Skill Publishing; add-skills tasks 4.1, 4.2,
// 4.4, design D10): SKILL.md in two parts, private Skills and their files on disk, the package zip, and drafts from
// sample Threads whose names, email addresses and paths become parameters.
#include "skills/authoring.h"
#include "skills/catalog.h"

#include <gtest/gtest.h>

#include <fstream>
#include <random>

using namespace wb;
using namespace wb::skills;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("workbench-authoring-" + std::to_string(rd()));
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
