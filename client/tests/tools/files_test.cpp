// client/core-tools file tools on Windows (add-core-tools-and-mcp tasks 1.1-1.3): path checks through
// client/policy-enforcement, deny filtering, binary refusal, truncation, atomic writes and exact-once edits.
#include "base/encoding.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "policy/paths.h"
#include "support/junction.h"
#include "tools/files.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wb;
using namespace wb::tools;
using runtime::ToolContext;
using runtime::ToolOutput;

namespace {

void put(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string get(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string s(const std::filesystem::path& p) { return narrow(p.wstring()); }

// A data root with a workspace, a permitted docs folder (readable), an out folder (writable), a secret folder
// inside docs that policy denies, and a folder outside every rule.
struct FileFixture {
    std::filesystem::path root = std::filesystem::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
    std::filesystem::path data = root / L"data", ws = data / L"workspace", docs = root / L"docs", out = root / L"out",
                          outside = root / L"outside";
    policy::PathRules rules;
    std::vector<std::filesystem::path> junctions;
    std::vector<runtime::ToolDef> tools;
    Extractor extract;

    FileFixture() {
        std::filesystem::create_directories(ws);
        std::filesystem::create_directories(out);
        put(docs / L"readme.txt", "line one\nline two\nline three\n");
        put(docs / L"src" / L"main.cpp", "int main() {\n  return 42;\n}\n");
        put(docs / L"src" / L"util.cpp", "// util\nint helper() { return 42; }\n");
        put(docs / L"secret" / L"keys.txt", "password=hunter2 return 42\n");
        put(outside / L"private.txt", "private data\n");
        rules = policy::path_rules(nullptr, s(ws), s(data), {});
        const std::string r = *policy::canonicalize(s(root));
        rules.read = {r + "\\docs\\**", r + "\\out\\**"};
        rules.write = {r + "\\out\\**"};
        rules.deny = {"**\\secret\\**"};
        rebuild();
    }
    ~FileFixture() {
        for (const auto& j : junctions) ::RemoveDirectoryW(j.c_str());
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void rebuild() {
        FileToolsEnv env;
        env.rules = [this] { return rules; };
        env.workspace = rules.workspace;
        env.extract = extract;
        tools = file_tools(env);
    }
    const runtime::ToolDef& tool(const std::string& id) {
        for (const auto& t : tools)
            if (t.id == id) return t;
        throw std::runtime_error("no tool " + id);
    }
    ToolOutput call(const std::string& id, const json& args) {
        ToolContext ctx;
        return tool(id).run(ctx, args);
    }
    bool junction(const std::filesystem::path& link, const std::filesystem::path& target) {
        auto final_target = policy::canonicalize(s(target));
        if (!final_target) return false;
        junctions.push_back(link);
        return test::make_junction(link, widen(*final_target));
    }
};

}  // namespace

TEST(FileTools, DeclareRiskClassesAndResources) {
    FileFixture f;
    for (const char* id : {"files.read", "files.list", "files.glob", "files.grep"}) {
        EXPECT_EQ(f.tool(id).risk, runtime::Risk::read) << id;
        EXPECT_FALSE(f.tool(id).provenance.trusted) << id << ": file content is untrusted";
    }
    EXPECT_EQ(f.tool("files.write").risk, runtime::Risk::write_local);
    EXPECT_EQ(f.tool("files.edit").risk, runtime::Risk::write_local);
    // Relative paths are anchored at the workspace for the policy's argument check.
    const auto res = f.tool("files.write").resources_of(json{{"path", "notes\\a.txt"}, {"content", ""}});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].kind, runtime::ToolResource::Kind::write_path);
    EXPECT_EQ(res[0].value, f.rules.workspace + "\\notes\\a.txt");
    const auto list = f.tool("files.list").resources_of(json::object());
    EXPECT_EQ(list[0].value, f.rules.workspace);
}

TEST(FileTools, ReadPermittedFileWithAndWithoutRange) {
    FileFixture f;
    auto all = f.call("files.read", {{"path", s(f.docs / L"readme.txt")}});
    ASSERT_TRUE(all.ok) << all.content;
    EXPECT_EQ(all.content, "line one\nline two\nline three\n");
    ASSERT_TRUE(all.provenance.has_value());
    EXPECT_EQ(all.provenance->source_kind, "files");
    EXPECT_FALSE(all.provenance->trusted);
    EXPECT_NE(all.provenance->source_id.find("readme.txt"), std::string::npos);

    auto range = f.call("files.read", {{"path", s(f.docs / L"readme.txt")}, {"offset", 2}, {"limit", 1}});
    ASSERT_TRUE(range.ok) << range.content;
    EXPECT_EQ(range.content, "2\tline two\n");
    auto beyond = f.call("files.read", {{"path", s(f.docs / L"readme.txt")}, {"offset", 9}});
    EXPECT_FALSE(beyond.ok);
}

TEST(FileTools, PathsOutsideTheRulesAreDeniedWithoutData) {
    FileFixture f;
    auto r = f.call("files.read", {{"path", s(f.outside / L"private.txt")}});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.content.find("denied by policy"), std::string::npos) << r.content;
    EXPECT_EQ(r.content.find("private data"), std::string::npos);
    auto secret = f.call("files.read", {{"path", s(f.docs / L"secret" / L"keys.txt")}});
    EXPECT_FALSE(secret.ok);
    EXPECT_EQ(secret.content.find("hunter2"), std::string::npos);
}

TEST(FileTools, JunctionEscapeIsDeniedWithoutReadingData) {
    FileFixture f;
    ASSERT_TRUE(f.junction(f.docs / L"escape", f.outside));
    auto r = f.call("files.read", {{"path", s(f.docs / L"escape" / L"private.txt")}});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.content.find("denied by policy"), std::string::npos) << r.content;
    EXPECT_EQ(r.content.find("private data"), std::string::npos);
    // The listing walk does not follow it either, and grep finds nothing behind it.
    auto g = f.call("files.grep", {{"pattern", "private"}, {"path", s(f.docs)}});
    ASSERT_TRUE(g.ok) << g.content;
    EXPECT_EQ(g.content.find("private data"), std::string::npos);
    EXPECT_NE(g.content.find("linked folders not followed"), std::string::npos) << g.content;
}

TEST(FileTools, DeniedPathsAreOmittedFromListingsWithACount) {
    FileFixture f;
    auto g = f.call("files.glob", {{"pattern", "*.txt"}, {"path", s(f.docs)}});
    ASSERT_TRUE(g.ok) << g.content;
    EXPECT_NE(g.content.find("readme.txt"), std::string::npos);
    EXPECT_EQ(g.content.find("keys.txt"), std::string::npos);
    EXPECT_NE(g.content.find("1 entries omitted because policy denies"), std::string::npos) << g.content;

    auto l = f.call("files.list", {{"path", s(f.docs)}});
    ASSERT_TRUE(l.ok) << l.content;
    EXPECT_NE(l.content.find("[dir]  src\\"), std::string::npos) << l.content;
    EXPECT_NE(l.content.find("[file] readme.txt"), std::string::npos);
    EXPECT_EQ(l.content.find("secret"), std::string::npos);
    EXPECT_NE(l.content.find("1 entries omitted"), std::string::npos);

    auto paths = f.call("files.glob", {{"pattern", "src\\*.cpp"}, {"path", s(f.docs)}});
    ASSERT_TRUE(paths.ok);
    EXPECT_NE(paths.content.find("2 matches"), std::string::npos) << paths.content;
}

TEST(FileTools, GrepReturnsMatchingLinesAndSkipsDeniedFiles) {
    FileFixture f;
    auto g = f.call("files.grep", {{"pattern", "return\\s+42"}, {"path", s(f.docs)}, {"glob", "*.cpp"}});
    ASSERT_TRUE(g.ok) << g.content;
    EXPECT_NE(g.content.find("main.cpp:2:   return 42;"), std::string::npos) << g.content;
    EXPECT_NE(g.content.find("util.cpp:2: int helper()"), std::string::npos);
    EXPECT_EQ(g.content.find("hunter2"), std::string::npos);
    auto bad = f.call("files.grep", {{"pattern", "("}, {"path", s(f.docs)}});
    EXPECT_FALSE(bad.ok);
    auto one = f.call("files.grep", {{"pattern", "TWO"}, {"case_insensitive", true}, {"path", s(f.docs / L"readme.txt")}});
    ASSERT_TRUE(one.ok);
    EXPECT_NE(one.content.find("readme.txt:2: line two"), std::string::npos) << one.content;
}

TEST(FileTools, BinaryFilesAreRefused) {
    FileFixture f;
    put(f.docs / L"tool.exe", std::string("MZ\x90\0\x03\0\0\0\x04\0\0\0\xff\xff", 14) + std::string(1000, '\0'));
    auto r = f.call("files.read", {{"path", s(f.docs / L"tool.exe")}});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.content.find("binary file not supported"), std::string::npos) << r.content;
    // A document without an extractor is refused too; with one, its text is returned.
    put(f.docs / L"report.docx", std::string("PK\x03\x04\0\0", 6));
    auto doc = f.call("files.read", {{"path", s(f.docs / L"report.docx")}});
    EXPECT_FALSE(doc.ok);
    f.extract = [](const std::string& kind, const std::string& bytes, ToolContext&) -> Result<std::string> {
        return "# Report (" + kind + ", " + std::to_string(bytes.size()) + " bytes)\n";
    };
    f.rebuild();
    doc = f.call("files.read", {{"path", s(f.docs / L"report.docx")}});
    ASSERT_TRUE(doc.ok) << doc.content;
    EXPECT_EQ(doc.content, "# Report (docx, 6 bytes)\n");
}

TEST(FileTools, LargeFilesAreTruncatedWithANotice) {
    FileFixture f;
    std::string log;
    for (int i = 1; log.size() < 5 * 1024 * 1024; ++i) log += "2026-10-22 12:00:00 INFO line " + std::to_string(i) + " of the log\n";
    put(f.docs / L"big.log", log);
    auto r = f.call("files.read", {{"path", s(f.docs / L"big.log")}});
    ASSERT_TRUE(r.ok) << r.content.substr(0, 200);
    const size_t notice = r.content.find("[Truncated: showing lines 1-");
    ASSERT_NE(notice, std::string::npos);
    EXPECT_LE(notice, 256u * 1024u);
    EXPECT_GT(notice, 250u * 1024u);
    EXPECT_NE(r.content.find(std::to_string(log.size()) + " bytes"), std::string::npos) << r.content.substr(notice);
    EXPECT_NE(r.content.find("offset="), std::string::npos);
    // The next range continues where the notice says.
    const size_t at = r.content.find("offset=", notice) + 7;
    const long long next = std::stoll(r.content.substr(at));
    auto more = f.call("files.read", {{"path", s(f.docs / L"big.log")}, {"offset", next}, {"limit", 2}});
    ASSERT_TRUE(more.ok);
    EXPECT_EQ(more.content, std::to_string(next) + "\t2026-10-22 12:00:00 INFO line " + std::to_string(next) + " of the log\n" +
                                std::to_string(next + 1) + "\t2026-10-22 12:00:00 INFO line " + std::to_string(next + 1) + " of the log\n");
}

TEST(FileTools, WriteCreatesAndReplacesAtomicallyInsidePermittedFolders) {
    FileFixture f;
    auto w = f.call("files.write", {{"path", s(f.out / L"a.txt")}, {"content", "first"}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"a.txt"), "first");
    w = f.call("files.write", {{"path", s(f.out / L"a.txt")}, {"content", "second version"}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"a.txt"), "second version");
    // No temporary file is left behind.
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(f.out)) n += e.is_regular_file() ? 1 : 0;
    EXPECT_EQ(n, 1u);
    // Relative paths land in the workspace.
    ASSERT_TRUE(f.call("files.write", {{"path", "draft.md"}, {"content", "# Draft"}}).ok);
    EXPECT_EQ(get(f.ws / L"draft.md"), "# Draft");
}

TEST(FileTools, WriteOutsidePermittedFoldersIsDeniedAndCreatesNothing) {
    FileFixture f;
    auto w = f.call("files.write", {{"path", s(f.docs / L"new.txt")}, {"content", "x"}});
    EXPECT_FALSE(w.ok);
    EXPECT_NE(w.content.find("denied by policy"), std::string::npos) << w.content;
    EXPECT_FALSE(std::filesystem::exists(f.docs / L"new.txt"));
    w = f.call("files.write", {{"path", s(f.outside / L"private.txt")}, {"content", "overwritten"}});
    EXPECT_FALSE(w.ok);
    EXPECT_EQ(get(f.outside / L"private.txt"), "private data\n");
    // A junction inside the writable folder does not lead out of it.
    ASSERT_TRUE(f.junction(f.out / L"escape", f.outside));
    w = f.call("files.write", {{"path", s(f.out / L"escape" / L"x.txt")}, {"content", "x"}});
    EXPECT_FALSE(w.ok);
    EXPECT_FALSE(std::filesystem::exists(f.outside / L"x.txt"));
    // A hard link to a file outside is refused.
    ASSERT_TRUE(::CreateHardLinkW((f.out / L"hard.txt").c_str(), (f.outside / L"private.txt").c_str(), nullptr));
    w = f.call("files.write", {{"path", s(f.out / L"hard.txt")}, {"content", "via link"}});
    EXPECT_FALSE(w.ok);
    EXPECT_EQ(get(f.outside / L"private.txt"), "private data\n");
}

TEST(FileTools, EditReplacesExactlyOneSpan) {
    FileFixture f;
    put(f.out / L"config.ini", "[a]\nvalue=1\n[b]\nvalue=1\nname=x\n");
    auto amb = f.call("files.edit", {{"path", s(f.out / L"config.ini")}, {"old_text", "value=1"}, {"new_text", "value=2"}});
    EXPECT_FALSE(amb.ok);
    EXPECT_NE(amb.content.find("ambiguous match"), std::string::npos) << amb.content;
    EXPECT_EQ(get(f.out / L"config.ini"), "[a]\nvalue=1\n[b]\nvalue=1\nname=x\n");
    auto missing = f.call("files.edit", {{"path", s(f.out / L"config.ini")}, {"old_text", "nope"}, {"new_text", ""}});
    EXPECT_FALSE(missing.ok);
    auto ok = f.call("files.edit", {{"path", s(f.out / L"config.ini")}, {"old_text", "[b]\nvalue=1"}, {"new_text", "[b]\nvalue=2"}});
    ASSERT_TRUE(ok.ok) << ok.content;
    EXPECT_NE(ok.content.find("line 3"), std::string::npos) << ok.content;
    EXPECT_EQ(get(f.out / L"config.ini"), "[a]\nvalue=1\n[b]\nvalue=2\nname=x\n");
    // Outside the write rules: denied even though the file is readable.
    auto ro = f.call("files.edit", {{"path", s(f.docs / L"readme.txt")}, {"old_text", "line one"}, {"new_text", "changed"}});
    EXPECT_FALSE(ro.ok);
    EXPECT_EQ(get(f.docs / L"readme.txt"), "line one\nline two\nline three\n");
}

TEST(FileTools, EditKeepsLineEndingsAndEncoding) {
    FileFixture f;
    put(f.out / L"win.txt", "alpha\r\nbeta\r\ngamma\r\n");
    auto crlf = f.call("files.edit", {{"path", s(f.out / L"win.txt")}, {"old_text", "alpha\nbeta"}, {"new_text", "alpha\nBETA"}});
    ASSERT_TRUE(crlf.ok) << crlf.content;
    EXPECT_EQ(get(f.out / L"win.txt"), "alpha\r\nBETA\r\ngamma\r\n");
    // UTF-16LE with BOM stays UTF-16LE.
    const std::string utf16("\xFF\xFEh\0\xE9\0l\0l\0o\0", 12);  // "héllo"
    put(f.out / L"u16.txt", utf16);
    auto r = f.call("files.read", {{"path", s(f.out / L"u16.txt")}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.content, "h\xC3\xA9llo");
    ASSERT_TRUE(f.call("files.edit", {{"path", s(f.out / L"u16.txt")}, {"old_text", "llo"}, {"new_text", "y"}}).ok);
    EXPECT_EQ(get(f.out / L"u16.txt"), std::string("\xFF\xFEh\0\xE9\0y\0", 8));
}
