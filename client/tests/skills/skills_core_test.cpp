// Portable Skills tests (client/skills; add-skills tasks 2.1-2.3): SKILL.md and its frontmatter, the load tiers with
// precedence and locking, the tool scope and the interrupt_on resolutions.
#include "skills/skill.h"
#include "skills/yaml.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::skills;

namespace {

Parsed ok(std::string_view text, bool require_version = false) {
    auto r = parse_skill_md(text, require_version);
    if (auto* e = std::get_if<ParseError>(&r)) ADD_FAILURE() << e->field << ": " << e->message;
    return std::get_if<Parsed>(&r) ? std::get<Parsed>(r) : Parsed{};
}

ParseError bad(std::string_view text, bool require_version = false) {
    auto r = parse_skill_md(text, require_version);
    if (std::get_if<Parsed>(&r)) ADD_FAILURE() << "parsed: " << text;
    return std::get_if<ParseError>(&r) ? std::get<ParseError>(r) : ParseError{};
}

}  // namespace

TEST(SkillYaml, MappingsListsScalarsAndBlocks) {
    const auto y = parse_yaml(
        "name: weekly-report   # comment\n"
        "description: \"Builds the weekly: status\"\n"
        "quoted: 'it''s'\n"
        "tools: [files.read, \"mail.search\"]\n"
        "requires_mcp:\n"
        "  - jira-corp\n"
        "  - confluence\n"
        "flat:\n"
        "- a\n"
        "- b\n"
        "interrupt_on:\n"
        "  mail.draft: [approve, reject]\n"
        "  shell.run: true\n"
        "triggers:\n"
        "  - kind: schedule\n"
        "    cron: \"0 8 * * 1\"\n"
        "    name: Weekly\n"
        "notes: |\n"
        "  # Heading, not a comment\n"
        "\n"
        "  second paragraph\n"
        "folded: >\n"
        "  one\n"
        "  two\n"
        "map: {a: 1, b: [x, y]}\n"
        "empty:\n"
        "flag: false\n");
    ASSERT_TRUE(y.ok()) << y.error().message;
    const json& j = *y;
    EXPECT_EQ(j["name"], "weekly-report");
    EXPECT_EQ(j["description"], "Builds the weekly: status");
    EXPECT_EQ(j["quoted"], "it's");
    EXPECT_EQ(j["tools"], json::array({"files.read", "mail.search"}));
    EXPECT_EQ(j["requires_mcp"], json::array({"jira-corp", "confluence"}));
    EXPECT_EQ(j["flat"], json::array({"a", "b"}));
    EXPECT_EQ(j["interrupt_on"]["mail.draft"], json::array({"approve", "reject"}));
    EXPECT_EQ(j["interrupt_on"]["shell.run"], true);
    EXPECT_EQ(j["triggers"][0]["kind"], "schedule");
    EXPECT_EQ(j["triggers"][0]["cron"], "0 8 * * 1");
    EXPECT_EQ(j["triggers"][0]["name"], "Weekly");
    EXPECT_EQ(j["notes"], "# Heading, not a comment\n\nsecond paragraph\n");
    EXPECT_EQ(j["folded"], "one two\n");
    EXPECT_EQ(j["map"]["b"], json::array({"x", "y"}));
    EXPECT_EQ(j["map"]["a"], "1");  // plain scalars stay text
    EXPECT_TRUE(j["empty"].is_null());
    EXPECT_EQ(j["flag"], false);
}

TEST(SkillYaml, RefusesWhatItDoesNotSupport) {
    for (const char* text : {"a: 1\na: 2\n", "  indented: x\n", "a: [1, 2\n", "a: &anchor x\n", "a: \"unclosed\n", "- a\n- b\n",
                             "a: x\n b: y\n", "a:\n\t- x\n", "a: \"\\q\"\n"}) {
        EXPECT_FALSE(parse_yaml(text).ok()) << text;
    }
    std::string deep = "a: ";
    for (int i = 0; i < 40; ++i) deep += "[";
    EXPECT_FALSE(parse_yaml(deep).ok());
    EXPECT_FALSE(parse_yaml(std::string(70000, 'x')).ok());
}

TEST(SkillMd, ValidSkillWithEveryField) {
    const Parsed p = ok(
        "\xEF\xBB\xBF---\r\n"
        "name: supplier-triage\r\n"
        "description: Triage supplier mail\r\n"
        "version: 1.2.0\r\n"
        "tools: [mail.search, mail.draft]\r\n"
        "risk: write-local\r\n"
        "network: none\r\n"
        "requires_mcp: [jira-corp]\r\n"
        "interrupt_on:\r\n"
        "  mail.draft: [approve, edit, reject]\r\n"
        "license: MIT\r\n"
        "metadata:\r\n"
        "  team: purchasing\r\n"
        "---\r\n"
        "# Steps\r\nDo it.\r\n");
    const Manifest& m = p.manifest;
    EXPECT_EQ(m.name, "supplier-triage");
    EXPECT_EQ(m.version, "1.2.0");
    EXPECT_TRUE(m.version_declared);
    ASSERT_TRUE(m.tools.has_value());
    EXPECT_EQ(*m.tools, (std::vector<std::string>{"mail.search", "mail.draft"}));
    EXPECT_EQ(m.risk, "write-local");
    EXPECT_EQ(m.network, "none");
    EXPECT_EQ(m.requires_mcp, std::vector<std::string>{"jira-corp"});
    ASSERT_EQ(m.interrupt_on.size(), 1u);
    EXPECT_EQ(m.interrupt_on[0].resolutions, (std::vector<std::string>{"approve", "approve_edited", "reject"}));
    // Unknown fields are preserved and ignored.
    EXPECT_EQ(m.frontmatter["license"], "MIT");
    EXPECT_EQ(m.frontmatter["metadata"]["team"], "purchasing");
    EXPECT_EQ(p.body, "# Steps\r\nDo it.\r\n");
}

TEST(SkillMd, AgentSkillsCompatibility) {
    // Only name and description: version 0.0.0, the default tool set (no tools field).
    const Parsed p = ok("---\nname: pdf\ndescription: Work with PDF files\n---\nUse the tools.\n");
    EXPECT_EQ(p.manifest.version, "0.0.0");
    EXPECT_FALSE(p.manifest.version_declared);
    EXPECT_FALSE(p.manifest.tools.has_value());
    EXPECT_EQ(p.manifest.risk, "read");
    EXPECT_EQ(p.manifest.network, "proxy");
    // The registry tiers need the version.
    EXPECT_EQ(bad("---\nname: pdf\ndescription: d\n---\n", true).field, "version");
}

TEST(SkillMd, ValidationNamesTheField) {
    EXPECT_EQ(bad("no frontmatter").field, "frontmatter");
    EXPECT_EQ(bad("---\nname: x\n").field, "frontmatter");
    EXPECT_EQ(bad("---\nname: x\ndescription: [\n---\n").field, "frontmatter");
    EXPECT_EQ(bad("---\ndescription: d\n---\n").field, "name");
    EXPECT_EQ(bad("---\nname: Weekly_Report\ndescription: d\n---\n").field, "name");
    EXPECT_EQ(bad("---\nname: -x\ndescription: d\n---\n").field, "name");
    EXPECT_EQ(bad("---\nname: x\n---\n").field, "description");
    EXPECT_EQ(bad("---\nname: x\ndescription: d\nversion: 1.0\n---\n").field, "version");
    EXPECT_EQ(bad("---\nname: x\ndescription: d\nrisk: high\n---\n").field, "risk");
    EXPECT_EQ(bad("---\nname: x\ndescription: d\nnetwork: open\n---\n").field, "network");
    EXPECT_EQ(bad("---\nname: x\ndescription: d\ntools: {a: b}\n---\n").field, "tools");
    EXPECT_EQ(bad("---\nname: x\ndescription: d\ninterrupt_on:\n  mail.send: [maybe]\n---\n").field, "interrupt_on");
    EXPECT_EQ(bad("---\nname: x\ndescription: d\ntriggers:\n  - cron: x\n---\n").field, "triggers");
}

TEST(SkillMd, InterruptOnCannotLoosen) {
    const Parsed p = ok("---\nname: x\ndescription: d\ninterrupt_on:\n  files.read: false\n  shell.run: true\n---\n");
    ASSERT_EQ(p.manifest.interrupt_on.size(), 1u);  // false is ignored
    EXPECT_EQ(p.manifest.interrupt_on[0].tool, "shell.run");
    EXPECT_TRUE(p.manifest.interrupt_on[0].resolutions.empty());
    const Parsed list = ok("---\nname: x\ndescription: d\ninterrupt_on: [mail.*, files.write]\n---\n");
    EXPECT_EQ(list.manifest.interrupt_on.size(), 2u);
}

TEST(SkillMd, Versions) {
    EXPECT_TRUE(valid_version("1.0.0"));
    EXPECT_TRUE(valid_version("10.20.30-beta.1"));
    EXPECT_FALSE(valid_version("1.0"));
    EXPECT_FALSE(valid_version("01.0.0"));
    EXPECT_LT(compare_versions("1.2.0", "1.10.0"), 0);
    EXPECT_LT(compare_versions("1.0.0-rc.1", "1.0.0"), 0);
    EXPECT_EQ(compare_versions("2.0.0", "2.0.0"), 0);
}

namespace {

Loaded skill(Tier tier, const std::string& name, const std::string& version = "1.0.0") {
    Loaded l;
    l.tier = tier;
    l.name = name;
    l.folder = std::string(to_string(tier)) + "/" + name;
    Parsed p;
    p.manifest.name = name;
    p.manifest.description = "d";
    p.manifest.version = version;
    l.skill = p;
    return l;
}

Loaded broken(Tier tier, const std::string& name) {
    Loaded l;
    l.tier = tier;
    l.name = name;
    l.error = ParseError{"description", "SKILL.md needs a description."};
    return l;
}

}  // namespace

TEST(SkillTiers, PrecedenceAndShadowing) {
    const std::vector<Loaded> all = {skill(Tier::org, "weekly"), skill(Tier::priv, "weekly"), skill(Tier::builtin, "weekly"),
                                     skill(Tier::group, "triage"), skill(Tier::shared, "triage"), skill(Tier::builtin, "pdf")};
    const auto r = resolve_tiers(all, {});
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r.at("weekly").active->tier, Tier::priv);
    ASSERT_EQ(r.at("weekly").others.size(), 2u);
    EXPECT_EQ(r.at("weekly").others[0].skill->tier, Tier::org);
    EXPECT_EQ(r.at("weekly").others[0].state, "shadowed");
    EXPECT_EQ(r.at("triage").active->tier, Tier::shared);
    EXPECT_EQ(r.at("pdf").active->tier, Tier::builtin);
}

TEST(SkillTiers, LockedOrgSkillWinsAndTheShadowIsConflicting) {
    const std::vector<Loaded> all = {skill(Tier::org, "expenses"), skill(Tier::priv, "expenses"), skill(Tier::builtin, "expenses")};
    const auto r = resolve_tiers(all, {"expenses"});
    EXPECT_EQ(r.at("expenses").active->tier, Tier::org);
    std::map<Tier, std::string> states;
    for (const auto& o : r.at("expenses").others) states[o.skill->tier] = o.state;
    EXPECT_EQ(states[Tier::priv], "conflicting");
    EXPECT_EQ(states[Tier::builtin], "shadowed");
    // Locking a name without an org copy changes nothing.
    EXPECT_EQ(resolve_tiers({skill(Tier::priv, "x")}, {"x"}).at("x").active->tier, Tier::priv);
}

TEST(SkillTiers, InvalidCopiesNeverWin) {
    const std::vector<Loaded> all = {broken(Tier::priv, "weekly"), skill(Tier::org, "weekly"), broken(Tier::priv, "lonely")};
    const auto r = resolve_tiers(all, {});
    EXPECT_EQ(r.at("weekly").active->tier, Tier::org);
    EXPECT_EQ(r.at("weekly").others[0].state, "invalid");
    EXPECT_EQ(r.at("lonely").active, nullptr);
    // Two versions in one tier (an update kept for rollback is not loaded; this is the defensive order): newest first.
    const std::vector<Loaded> versions = {skill(Tier::group, "g", "1.2.0"), skill(Tier::group, "g", "1.10.0")};
    const auto two = resolve_tiers(versions, {});
    EXPECT_EQ(two.at("g").active->skill->manifest.version, "1.10.0");
}

TEST(SkillScope, IntersectionAndDefaultSet) {
    const std::vector<ToolFacts> tools = {
        {"files.read", "read", true},     {"files.write", "write-local", true}, {"shell.run", "external", false},
        {"mail.search", "read", true},    {"mail.draft", "write-local", true},  {"web.fetch", "read", false},
    };
    Manifest m;
    m.tools = std::vector<std::string>{"files.read", "shell.run", "mail.*", "nope.tool"};
    const Scope s = tool_scope(m, tools);
    EXPECT_EQ(s.tools, (std::vector<std::string>{"files.read", "mail.search", "mail.draft"}));
    EXPECT_EQ(s.unavailable, (std::vector<std::string>{"shell.run", "nope.tool"}));
    // No tools field: the permitted read-class tools only.
    Manifest none;
    EXPECT_EQ(tool_scope(none, tools).tools, (std::vector<std::string>{"files.read", "mail.search"}));
}

TEST(SkillScope, ResolutionsIntersect) {
    Manifest m;
    m.interrupt_on = {{"mail.*", {"approve", "reject", "respond_instead"}}, {"mail.draft", {"approve", "reject"}}, {"shell.run", {}}};
    EXPECT_EQ(allowed_resolutions(m, "mail.draft"), (std::vector<std::string>{"approve", "reject"}));
    EXPECT_EQ(allowed_resolutions(m, "mail.send"), (std::vector<std::string>{"approve", "reject", "respond_instead"}));
    EXPECT_TRUE(allowed_resolutions(m, "shell.run").empty());
    EXPECT_TRUE(allowed_resolutions(m, "files.read").empty());
    Manifest contradictory;
    contradictory.interrupt_on = {{"x", {"approve"}}, {"x", {"respond_instead"}}};
    EXPECT_EQ(allowed_resolutions(contradictory, "x"), std::vector<std::string>{"reject"});
    EXPECT_TRUE(tool_matches("mcp:jira/*", "mcp:jira/search"));
    EXPECT_FALSE(tool_matches("mail.*", "files.read"));
}
