// Portable policy tests (add-policy-and-sandbox tasks 2.1, 3.2, 3.4, 3.6): the decision function as a table, the glob
// matcher, path syntax and path rules, egress rules, and the client defaults against the generated schema.
#include "policy/core.h"
#include "policy/decide.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace wb;
using namespace wb::policy;

namespace {

EffectivePolicy make(json policy, json src = json::object(), long long ver = 7) {
    PolicyBundle b;
    b.sub = "dev1";
    b.ver = ver;
    b.exp = 4102444800;  // 2100
    b.policy = std::move(policy);
    b.src = std::move(src);
    b.groups = {"developers", "everyone"};
    return EffectivePolicy(std::move(b));
}

CallFacts call(std::string tool, std::string risk, bool unattended = false) {
    CallFacts f;
    f.tool = std::move(tool);
    f.risk = std::move(risk);
    f.unattended = unattended;
    return f;
}

struct Case {
    const char* name;
    json policy;
    CallFacts facts;
    Decision expect;
    const char* rule;  // expected rule, or nullptr
};

}  // namespace

TEST(PolicyDecide, Table) {
    SkillFloor writes{{"files.write"}, ""};
    SkillFloor wl{{}, "write-local"};
    CallFacts skill_write = call("files.write", "write-local");
    skill_write.skill = writes;
    CallFacts skill_external = call("calendar.create", "external");
    skill_external.skill = wl;

    const json devs = {{"tools", {{"rules", {{"browser.*", "allow"}, {"browser.eval", "deny"}}}}}};
    const std::vector<Case> cases = {
        {"tool rule overrides class rule", {{"tools", {{"risk", {{"external", "allow"}}}, {"rules", {{"calendar.create", "approve"}}}}}},
         call("calendar.create", "external"), Decision::approve, "tools.rules[\"calendar.create\"]"},
        {"most specific pattern: wildcard allows", devs, call("browser.click", "external"), Decision::allow, "tools.rules[\"browser.*\"]"},
        {"most specific pattern: exact denies", devs, call("browser.eval", "external"), Decision::deny, "tools.rules[\"browser.eval\"]"},
        {"default read allow", json::object(), call("files.read", "read"), Decision::allow, "tools.risk[\"read\"]"},
        {"default write-local approve", json::object(), call("files.write", "write-local"), Decision::approve, nullptr},
        {"default external approve", json::object(), call("web.fetch", "external"), Decision::approve, nullptr},
        {"default destructive deny", json::object(), call("files.delete", "destructive"), Decision::deny, "tools.risk[\"destructive\"]"},
        {"unknown risk class is destructive", json::object(), call("x.y", "weird"), Decision::deny, nullptr},
        {"class rule from policy", {{"tools", {{"risk", {{"destructive", "approve"}}}}}}, call("files.delete", "destructive"), Decision::approve, nullptr},
        {"skill interrupt_on", {{"tools", {{"risk", {{"write-local", "allow"}}}}}}, skill_write, Decision::approve, "skill.interrupt_on[files.write]"},
        {"call above skill risk", {{"tools", {{"risk", {{"external", "allow"}}}}}}, skill_external, Decision::approve, "skill.risk"},
        {"mail.send floor in an interactive Run", {{"tools", {{"rules", {{"mail.send", "allow"}}}}}}, call("mail.send", "external"), Decision::approve, "tool:mail.send"},
        {"mail.send unattended outside allowed risk", {{"tools", {{"rules", {{"mail.send", "allow"}}}}}}, call("mail.send", "external", true), Decision::approve, nullptr},
        {"unattended external needs approval", {{"tools", {{"risk", {{"external", "allow"}}}}}}, call("web.fetch", "external", true), Decision::approve, "unattended.allowed_risk"},
        {"unattended read stays allowed", json::object(), call("files.read", "read", true), Decision::allow, nullptr},
        {"unattended rule lifts the mail.send floor",
         {{"tools", {{"risk", {{"external", "allow"}}}}}, {"unattended", {{"rules", {{"mail.send", "allow"}}}}}},
         call("mail.send", "external", true), Decision::allow, nullptr},
        {"unattended rule does not lift the floor in interactive Runs",
         {{"tools", {{"risk", {{"external", "allow"}}}}}, {"unattended", {{"rules", {{"mail.send", "allow"}}}}}},
         call("mail.send", "external", false), Decision::approve, nullptr},
        {"unattended rule denies", {{"tools", {{"risk", {{"read", "allow"}}}}}, {"unattended", {{"rules", {{"computer.*", "deny"}}}}}},
         call("computer.observe", "read", true), Decision::deny, "unattended.rules[\"computer.*\"]"},
        {"unattended rules do not apply to interactive Runs", {{"unattended", {{"rules", {{"computer.*", "deny"}}}}}},
         call("computer.observe", "read", false), Decision::allow, nullptr},
        // client/computer-use Unattended Computer Use: the approve floor holds in Unattended runs only, and only an
        // explicit policy.unattended.rules allow lifts it (an allowed Risk class does not).
        {"computer action in an interactive Run", {{"tools", {{"risk", {{"external", "allow"}}}}}}, call("computer.click", "external"),
         Decision::allow, nullptr},
        {"computer action unattended: approve floor",
         {{"tools", {{"risk", {{"external", "allow"}}}}}, {"unattended", {{"allowed_risk", {"read", "external"}}}}},
         call("computer.click", "external", true), Decision::approve, "tool:computer.click"},
        {"unattended rule lifts the computer floor",
         {{"tools", {{"risk", {{"external", "allow"}}}}}, {"unattended", {{"rules", {{"computer.click", "allow"}}}}}},
         call("computer.click", "external", true), Decision::allow, nullptr},
        {"computer observe unattended has no floor", {{"unattended", {{"allowed_risk", {"read"}}}}}, call("computer.observe", "read", true),
         Decision::allow, nullptr},
        {"equal specificity: the stricter wins", {{"tools", {{"rules", {{"a.*", "approve"}, {"*.b", "allow"}}}}}}, call("a.b", "read"), Decision::approve, nullptr},
        {"MCP pattern", {{"tools", {{"rules", {{"mcp:jira-corp/*", "deny"}}}}}}, call("mcp:jira-corp/search", "read"), Decision::deny, nullptr},
    };
    for (const auto& c : cases) {
        const EffectivePolicy p = make(c.policy, {{"tools.rules[\"browser.eval\"]", "developers"}});
        const Verdict v = decide(&p, c.facts);
        EXPECT_EQ(v.decision, c.expect) << c.name << " (" << v.rule << ": " << v.reason << ")";
        if (c.rule) EXPECT_EQ(v.rule, c.rule) << c.name;
        EXPECT_EQ(v.policy_version, 7) << c.name;
    }
}

TEST(PolicyDecide, RuleSourceNamesTheGroup) {
    const EffectivePolicy p = make({{"tools", {{"rules", {{"browser.eval", "deny"}}}}}}, {{"tools.rules[\"browser.eval\"]", "developers"}});
    const Verdict v = decide(&p, call("browser.eval", "external"));
    EXPECT_EQ(v.source, "developers");
    EXPECT_EQ(decide(&p, call("files.delete", "destructive")).source, "default");
}

TEST(PolicyDecide, FailClosedWithoutPolicy) {
    CallFacts own = call("ask_user", "read");
    own.own_data = true;
    EXPECT_EQ(decide(nullptr, own).decision, Decision::allow);
    EXPECT_EQ(decide(nullptr, call("files.read", "read")).decision, Decision::deny);
    CallFacts own_write = call("threads.rename", "write-local");
    own_write.own_data = true;
    EXPECT_EQ(decide(nullptr, own_write).decision, Decision::deny);
    EXPECT_EQ(decide(nullptr, call("web.fetch", "external")).rule, "fail-closed");
}

TEST(PolicyDecide, ArgumentChecksDeny) {
    const EffectivePolicy p = make({{"tools", {{"risk", {{"read", "allow"}}}}}});
    CallFacts f = call("files.read", "read");
    f.resources.push_back({Resource::Kind::read_path, "C:\\Users\\a\\.ssh\\id_rsa"});
    const Verdict v = decide(&p, f, declared_tool_floors(), [](const Resource&) {
        return RuleVerdict{false, "filesystem.deny[\"**\\\\.ssh\\\\**\"]", "the path matches a denied pattern"};
    });
    EXPECT_EQ(v.decision, Decision::deny);
    EXPECT_EQ(v.rule, "filesystem.deny[\"**\\\\.ssh\\\\**\"]");
    // A call already denied by rule never reaches the argument checks; an allowed one with passing checks stays allowed.
    EXPECT_EQ(decide(&p, f, declared_tool_floors(), [](const Resource&) { return RuleVerdict{true, "x", ""}; }).decision, Decision::allow);
}

TEST(PolicyGlob, Matching) {
    EXPECT_TRUE(glob_match("**\\.ssh\\**", "C:\\Users\\a\\.SSH\\id_rsa"));
    EXPECT_TRUE(glob_match("**\\*.kdbx", "D:\\vault\\db.KDBX"));
    EXPECT_FALSE(glob_match("D:\\DEV\\*", "D:\\DEV\\x\\y"));
    EXPECT_TRUE(glob_match("D:\\DEV\\**", "d:\\dev\\x\\y"));
    EXPECT_TRUE(glob_match("D:/DEV/**", "D:\\DEV\\x"));
    EXPECT_TRUE(glob_match("*.corp.local", "gitlab.corp.local"));
    EXPECT_TRUE(glob_match("*.corp.local", "a.b.corp.local"));
    EXPECT_FALSE(glob_match("*.corp.local", "corp.local"));
    EXPECT_TRUE(glob_match("browser.*", "browser.click"));
    EXPECT_FALSE(glob_match("browser.*", "mail.send"));
    EXPECT_TRUE(glob_match("a?c", "abc"));
    EXPECT_FALSE(glob_match("a?c", "a\\c"));
    EXPECT_TRUE(glob_match("**", ""));
    EXPECT_FALSE(glob_match("", "x"));
    EXPECT_GT(pattern_specificity("browser.eval"), pattern_specificity("browser.*"));
    EXPECT_GT(pattern_specificity("browser.e*"), pattern_specificity("browser.*"));
}

TEST(PolicyPaths, SyntaxEscapesAreRefused) {
    for (const char* bad : {"\\\\.\\PhysicalDrive0", "\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\x", "\\??\\C:\\x", "C:\\x\\file.txt:secret",
                            "C:\\x\\file.txt::$DATA", "\\\\server\\share\\f:ads", "C:\\x\\CON", "C:\\x\\nul.txt", "C:\\x\\COM1",
                            "relative\\path", "C:relative", "\\\\?\\Volume{1234}\\x", "C:\\x\ny", "", "\\\\"}) {
        EXPECT_TRUE(path_syntax_problem(bad).has_value()) << bad;
    }
    for (const char* ok : {"C:\\Users\\a\\file.txt", "\\\\server\\share\\dir", "\\\\?\\C:\\long\\path", "\\\\?\\UNC\\server\\share\\x",
                           "C:\\", "D:\\console\\x", "C:\\x\\com10"}) {
        EXPECT_FALSE(path_syntax_problem(ok).has_value()) << ok << ": " << path_syntax_problem(ok).value_or("");
    }
    EXPECT_EQ(normalize_final_path("\\\\?\\C:\\a\\b\\"), "C:\\a\\b");
    EXPECT_EQ(normalize_final_path("\\\\?\\UNC\\srv\\share\\x"), "\\\\srv\\share\\x");
    EXPECT_EQ(normalize_final_path("C:"), "C:\\");
    EXPECT_TRUE(path_within("C:\\a\\b\\c", "c:\\A\\b"));
    EXPECT_TRUE(path_within("C:\\a\\b", "C:\\a\\b\\"));
    EXPECT_FALSE(path_within("C:\\a\\bc", "C:\\a\\b"));
    EXPECT_TRUE(path_within("C:\\a", "C:\\"));
}

TEST(PolicyPaths, Rules) {
    PathRules r;
    r.read = {"C:\\Users\\a\\Documents\\**"};
    r.write = {"D:\\DEV\\**"};
    r.deny = {"**\\.ssh\\**", "**\\*.pfx"};
    r.workspace = "C:\\Data\\Workbench\\workspace";
    r.data_root = "C:\\Data\\Workbench";
    r.grants = {{"E:\\Shared", false}, {"E:\\Mine", true}};
    EXPECT_TRUE(check_path("C:\\Users\\a\\Documents\\x.docx", Access::read, r).allowed);
    EXPECT_FALSE(check_path("C:\\Users\\a\\Documents\\x.docx", Access::write, r).allowed);
    EXPECT_TRUE(check_path("D:\\DEV\\proj\\a.cpp", Access::read, r).allowed);  // writable implies readable
    const auto ssh = check_path("C:\\Users\\a\\Documents\\.ssh\\id_rsa", Access::read, r);  // Denied subfolder
    EXPECT_FALSE(ssh.allowed);
    EXPECT_EQ(ssh.rule, "filesystem.deny[\"**\\\\.ssh\\\\**\"]");
    EXPECT_FALSE(check_path("D:\\DEV\\cert.PFX", Access::write, r).allowed);
    EXPECT_TRUE(check_path("C:\\Data\\Workbench\\workspace\\out.txt", Access::write, r).allowed);
    EXPECT_FALSE(check_path("C:\\Data\\Workbench\\app.db", Access::read, r).allowed);
    EXPECT_EQ(check_path("C:\\Data\\Workbench\\app.db", Access::read, r).rule, "data-root");
    EXPECT_TRUE(check_path("E:\\Shared\\a.txt", Access::read, r).allowed);
    EXPECT_FALSE(check_path("E:\\Shared\\a.txt", Access::write, r).allowed);
    EXPECT_TRUE(check_path("E:\\Mine\\a.txt", Access::write, r).allowed);
    EXPECT_FALSE(check_path("F:\\other.txt", Access::read, r).allowed);
    // Folder of an allowed pattern: the folder itself is covered, and so is a denied folder.
    EXPECT_TRUE(check_path("C:\\Users\\a\\Documents", Access::read, r).allowed);
    EXPECT_FALSE(check_path("C:\\Users\\a", Access::read, r).allowed);
    EXPECT_FALSE(check_path("C:\\Users\\a\\Documents\\.ssh", Access::read, r).allowed);
    EXPECT_TRUE(check_path("C:\\Users\\a\\Documents\\.sshd", Access::read, r).allowed);
    // No filesystem policy: only the workspace (Filesystem Defaults).
    PathRules none;
    none.workspace = r.workspace;
    none.data_root = r.data_root;
    EXPECT_TRUE(check_path("C:\\Data\\Workbench\\workspace\\a", Access::write, none).allowed);
    EXPECT_FALSE(check_path("C:\\Users\\a\\Documents\\x.docx", Access::read, none).allowed);
}

TEST(PolicyEgress, Rules) {
    const std::vector<std::string> allow = {"*.corp.local", "pypi.org"}, deny = {"*.pastebin.com", "secret.corp.local"};
    EXPECT_TRUE(check_egress_url("https://gitlab.corp.local/x", allow, deny).allowed);
    EXPECT_TRUE(check_egress_url("https://PyPI.org./simple", allow, deny).allowed);
    EXPECT_FALSE(check_egress_url("https://secret.corp.local/", allow, deny).allowed);
    EXPECT_EQ(check_egress_url("https://secret.corp.local/", allow, deny).rule, "network.egress_deny[\"secret.corp.local\"]");
    EXPECT_FALSE(check_egress_url("https://example.com/", allow, deny).allowed);
    EXPECT_FALSE(check_egress_url("ftp://pypi.org/", allow, deny).allowed);
    EXPECT_FALSE(check_egress_url("file:///C:/x", allow, deny).allowed);
    EXPECT_FALSE(check_egress_url("https://graph.microsoft.com/v1.0/me", {}, {}).allowed);  // empty lists deny everything
}

TEST(PolicyEgress, HostsMustBeAsciiNamesOrAddresses) {
    const std::vector<std::string> allow = {"*.corp.local", "10.*", "*"};
    // Valid names and literals are matched as usual.
    for (const char* h : {"gitlab.corp.local", "xn--bcher-kva.corp.local", "_dmarc.corp.local", "10.1.2.3", "fe80::1", "GITLAB.corp.local."})
        EXPECT_TRUE(check_egress_host(h, allow, {}).allowed) << h;
    // Anything a network stack might decode or IDNA-map into another host is denied, even under an allow-all rule.
    for (const char* h : {"evil.example%2f.corp.local", "evil.example%00.corp.local", "evil.example\xEF\xBC\x8F.corp.local",
                          "evil.example\xE2\x81\x84.corp.local", "b\xC3\xBC" "cher.corp.local", "evil example.corp.local",
                          "evil.example\\.corp.local", "evil.example/.corp.local", "a..corp.local", ".corp.local", "", "[::1]",
                          "fe80::1%25eth0", "user@evil.example"}) {
        const auto v = check_egress_host(h, allow, {});
        EXPECT_FALSE(v.allowed) << h;
        EXPECT_EQ(v.rule, "network") << h;
    }
}

TEST(PolicySchema, ClientDefaultsMatchTheGeneratedSchema) {
    std::ifstream in(WORKBENCH_POLICY_FIXTURE, std::ios::binary);
    ASSERT_TRUE(in) << "missing " << WORKBENCH_POLICY_FIXTURE << "; run npm run policy:schema in control-plane";
    std::stringstream ss;
    ss << in.rdbuf();
    const auto fixture = parse_json(ss.str());
    ASSERT_TRUE(fixture.has_value());
    std::map<std::string, json> keys;
    for (const auto& k : (*fixture)["keys"]) keys[k["path"].get<std::string>()] = k;
    for (const auto& [path, value] : client_policy_defaults().items()) {
        ASSERT_TRUE(keys.count(path)) << path << " is not a key of the policy schema";
        ASSERT_TRUE(keys[path].contains("default")) << path << " has no default in the schema";
        EXPECT_EQ(keys[path]["default"], value) << path;
    }
    // Every key the decision function reads exists in the schema.
    for (const char* k : {"tools.rules", "tools.risk", "unattended.rules", "unattended.allowed_risk", "network.egress_allow",
                          "network.egress_deny", "filesystem.read", "filesystem.write", "filesystem.deny", "filesystem.grantable"})
        EXPECT_TRUE(keys.count(k)) << k;
    EXPECT_EQ((*fixture)["schema"]["properties"]["tools"]["properties"]["shell"]["properties"]["mode"]["enum"],
              json({"off", "sandboxed", "allow"}));
}

TEST(PolicyBundle, PayloadClaims) {
    const json ok = {{"sub", "dev1"}, {"uid", "s1"}, {"ver", 3}, {"iat", 1}, {"exp", 2}, {"schema", 1}, {"groups", {"a"}},
                     {"policy", json::object()}, {"src", json::object()}};
    auto b = bundle_from_payload(ok);
    ASSERT_TRUE(b) << b.error().message;
    EXPECT_EQ(b->ver, 3);
    for (const char* k : {"sub", "ver", "exp", "policy", "schema"}) {
        json bad = ok;
        bad.erase(k);
        EXPECT_FALSE(bundle_from_payload(bad)) << k;
    }
    json zero = ok;
    zero["ver"] = 0;
    EXPECT_FALSE(bundle_from_payload(zero));
}
