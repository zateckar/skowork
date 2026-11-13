// The Skill catalogue on disk (client/skills Skill Load Tiers, Skill MCP Requirements; add-skills task 2.2): every tier,
// precedence and shadowing, locked org Skills, invalid copies, pinned registry versions and requires_mcp.
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
        path = fs::temp_directory_path() / ("skowork-skills-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string skill_md(const std::string& name, const std::string& description, const std::string& extra = "") {
    return "---\nname: " + name + "\ndescription: " + description + "\n" + extra + "---\nDo it.\n";
}

const json* entry(const json& list, const std::string& name) {
    for (const auto& e : list)
        if (jstr(e, "name") == name) return &e;
    return nullptr;
}

struct Fixture {
    TempDir tmp;
    std::vector<std::string> locked;
    std::set<std::string> mcp_missing;
    CatalogEnv env() {
        CatalogEnv e;
        e.builtin_dir = tmp.path / "builtin";
        e.data_dir = tmp.path / "data";
        e.locked = [this] { return locked; };
        e.mcp_unavailable = [this](const std::string& s) -> std::optional<std::string> {
            if (mcp_missing.count(s)) return std::string("not in the catalog");
            return std::nullopt;
        };
        e.tools = [] {
            return std::vector<ToolFacts>{{"files.read", "read", true}, {"shell.run", "external", false}};
        };
        return e;
    }
    fs::path priv(const std::string& name) { return tmp.path / "data" / "private" / name; }
    fs::path reg(const std::string& tier, const std::string& name, const std::string& version) {
        return tmp.path / "data" / tier / name / version;
    }
};

}  // namespace

TEST(SkillCatalog, EveryTierLoadsAndAMinimalPrivateSkillGetsVersionZero) {
    Fixture f;
    write(f.tmp.path / "builtin" / "summarize" / "SKILL.md", skill_md("summarize", "Summarise a document"));
    write(f.reg("org", "expense-report", "1.0.0") / "SKILL.md", skill_md("expense-report", "Expenses", "version: 1.0.0\n"));
    write(f.reg("group", "bom-check", "2.1.0") / "SKILL.md", skill_md("bom-check", "BOM check", "version: 2.1.0\n"));
    write(f.reg("group", "bom-check", "2.1.0") / "install.json", R"({"source":"Purchasing"})");
    write(f.reg("shared", "triage", "0.3.0") / "SKILL.md", skill_md("triage", "Triage", "version: 0.3.0\n"));
    write(f.priv("notes") / "SKILL.md", skill_md("notes", "My notes"));
    Catalog c(f.env());
    const auto usable = c.usable();
    std::map<std::string, std::string> tiers;
    for (const auto& u : usable) tiers[u.skill.manifest.name] = to_string(u.tier);
    EXPECT_EQ(tiers, (std::map<std::string, std::string>{{"bom-check", "group"}, {"expense-report", "org"}, {"notes", "private"},
                                                          {"summarize", "builtin"}, {"triage", "shared"}}));
    const auto notes = c.find("notes");
    ASSERT_TRUE(notes);
    EXPECT_EQ(notes->skill.manifest.version, "0.0.0");
    EXPECT_EQ(notes->skill.body, "Do it.\n");
    const json list = c.list();
    ASSERT_NE(entry(list, "bom-check"), nullptr);
    EXPECT_EQ(jstr(*entry(list, "bom-check"), "source"), "Purchasing");
    // The default Skill tool set: the permitted read-class tools.
    EXPECT_EQ((*entry(list, "notes"))["tools"], json::array({"files.read"}));
}

TEST(SkillCatalog, PrivateShadowsOrgAndTheViewSaysSo) {
    Fixture f;
    write(f.reg("org", "weekly-report", "1.0.0") / "SKILL.md", skill_md("weekly-report", "Org version", "version: 1.0.0\n"));
    write(f.priv("weekly-report") / "SKILL.md", skill_md("weekly-report", "My version"));
    Catalog c(f.env());
    ASSERT_TRUE(c.find("weekly-report"));
    EXPECT_EQ(c.find("weekly-report")->tier, Tier::priv);
    const json list = c.list();
    const json* e = entry(list, "weekly-report");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(jstr(*e, "tier"), "private");
    ASSERT_EQ((*e)["others"].size(), 1u);
    EXPECT_EQ(jstr((*e)["others"][0], "tier"), "org");
    EXPECT_EQ(jstr((*e)["others"][0], "state"), "shadowed");
}

TEST(SkillCatalog, LockedOrgSkillWinsAndThePrivateCopyIsConflicting) {
    Fixture f;
    f.locked = {"weekly-report"};
    write(f.reg("org", "weekly-report", "1.0.0") / "SKILL.md", skill_md("weekly-report", "Org version", "version: 1.0.0\n"));
    write(f.reg("shared", "weekly-report", "3.0.0") / "SKILL.md", skill_md("weekly-report", "Shared", "version: 3.0.0\n"));
    write(f.priv("weekly-report") / "SKILL.md", skill_md("weekly-report", "My version"));
    Catalog c(f.env());
    EXPECT_EQ(c.find("weekly-report")->tier, Tier::org);
    const json list = c.list();
    const json* e = entry(list, "weekly-report");
    ASSERT_NE(e, nullptr);
    ASSERT_EQ((*e)["others"].size(), 2u);
    for (const auto& o : (*e)["others"]) EXPECT_EQ(jstr(o, "state"), "conflicting");
    // Without the lock the private copy wins again.
    f.locked.clear();
    EXPECT_EQ(c.find("weekly-report")->tier, Tier::priv);
}

TEST(SkillCatalog, InvalidCopiesNeverWinAndShowTheirField) {
    Fixture f;
    write(f.priv("no-desc") / "SKILL.md", "---\nname: no-desc\n---\nbody\n");
    write(f.priv("Bad_Name") / "SKILL.md", skill_md("Bad_Name", "x"));
    write(f.priv("other") / "SKILL.md", skill_md("different", "Mismatched folder"));
    write(f.priv("empty") / "README.md", "no SKILL.md here");
    write(f.reg("org", "fallback", "1.0.0") / "SKILL.md", skill_md("fallback", "Org copy", "version: 1.0.0\n"));
    write(f.priv("fallback") / "SKILL.md", "---\nname: fallback\n---\n");
    // A registry Skill must declare its version, and it must be the installed one.
    write(f.reg("shared", "unversioned", "1.0.0") / "SKILL.md", skill_md("unversioned", "x"));
    write(f.reg("shared", "mismatch", "1.0.0") / "SKILL.md", skill_md("mismatch", "x", "version: 1.0.1\n"));
    Catalog c(f.env());
    const json list = c.list();
    std::set<std::string> usable;
    for (const auto& u : c.usable()) usable.insert(u.skill.manifest.name);
    EXPECT_EQ(usable, std::set<std::string>{"fallback"});
    EXPECT_EQ(c.find("fallback")->tier, Tier::org);
    ASSERT_NE(entry(list, "no-desc"), nullptr);
    EXPECT_EQ(jstr(*entry(list, "no-desc"), "state"), "invalid");
    EXPECT_EQ(jstr((*entry(list, "no-desc"))["error"], "field"), "description");
    EXPECT_EQ(jstr((*entry(list, "Bad_Name"))["error"], "field"), "name");
    EXPECT_EQ(jstr((*entry(list, "other"))["error"], "field"), "name");
    EXPECT_EQ(jstr((*entry(list, "empty"))["error"], "field"), "SKILL.md");
    EXPECT_EQ(jstr((*entry(list, "unversioned"))["error"], "field"), "version");
    EXPECT_EQ(jstr((*entry(list, "mismatch"))["error"], "field"), "version");
    const json* fb = entry(list, "fallback");
    ASSERT_EQ((*fb)["others"].size(), 1u);
    EXPECT_EQ(jstr((*fb)["others"][0], "state"), "invalid");
}

TEST(SkillCatalog, PinnedVersionIsUsedElseTheHighest) {
    Fixture f;
    for (const std::string v : {"1.0.0", "1.2.0", "1.10.0"})
        write(f.reg("org", "report", v) / "SKILL.md", skill_md("report", "v" + v, "version: " + v + "\n"));
    Catalog c(f.env());
    EXPECT_EQ(c.find("report")->skill.manifest.version, "1.10.0");
    write(f.tmp.path / "data" / "org" / "report" / "pinned", "1.2.0\n");
    c.reload();
    EXPECT_EQ(c.find("report")->skill.manifest.version, "1.2.0");
    // A pin to a version that is not installed falls back to the highest.
    write(f.tmp.path / "data" / "org" / "report" / "pinned", "9.9.9");
    c.reload();
    EXPECT_EQ(c.find("report")->skill.manifest.version, "1.10.0");
}

TEST(SkillCatalog, MissingMcpServerMakesTheSkillUnavailableWithItsName) {
    Fixture f;
    f.mcp_missing = {"jira-corp"};
    write(f.reg("group", "jira-triage", "1.0.0") / "SKILL.md",
          skill_md("jira-triage", "Triage Jira", "version: 1.0.0\nrequires_mcp: [jira-corp, confluence]\n"));
    Catalog c(f.env());
    EXPECT_FALSE(c.find("jira-triage"));
    const json list = c.list();
    const json* e = entry(list, "jira-triage");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(jstr(*e, "state"), "unavailable");
    EXPECT_EQ(jstr(*e, "unavailable"), "requires jira-corp");
    f.mcp_missing.clear();
    EXPECT_TRUE(c.find("jira-triage"));
}

TEST(SkillCatalog, DeclaredToolPolicyForbidsIsShownAsUnavailable) {
    Fixture f;
    write(f.priv("cleanup") / "SKILL.md", skill_md("cleanup", "Clean up", "tools: [files.read, shell.run]\n"));
    Catalog c(f.env());
    const json list = c.list();
    const json* e = entry(list, "cleanup");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ((*e)["tools"], json::array({"files.read"}));
    EXPECT_EQ((*e)["unavailable_tools"], json::array({"shell.run"}));
}

TEST(SkillCatalog, NonAsciiFolderPathsRoundTrip) {
    Fixture f;
    const fs::path dir = f.priv("zprava");
    write(dir / "SKILL.md", skill_md("zprava", "Týdenní zpráva"));
    Catalog c(f.env());
    ASSERT_TRUE(c.find("zprava"));
    EXPECT_EQ(c.find("zprava")->skill.manifest.description, "Týdenní zpráva");
    EXPECT_EQ(utf8_path(path_utf8(dir)), dir);
}
