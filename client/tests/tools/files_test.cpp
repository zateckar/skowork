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
#include <thread>
#include <vector>

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
    std::filesystem::path root = std::filesystem::path(local_appdata()) / L"skowork-test" / widen(random_urlsafe(6));
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
    ToolOutput call_in(const std::string& thread, const std::string& id, const json& args) {
        ToolContext ctx;
        ctx.thread_id = thread;
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

// fix-stream-gaps-and-parallel-file-edits D3: parallel calls on one file run one at a time, so every edit applies.
TEST(FileTools, ParallelEditsOfOneFileAllApply) {
    FileFixture f;
    constexpr int kLines = 12;
    std::string original;
    for (int i = 0; i < kLines; ++i) original += "line " + std::to_string(i) + " old\n";
    for (int round = 0; round < 5; ++round) {
        put(f.out / L"shared.txt", original);
        std::vector<ToolOutput> results(kLines);
        std::vector<std::thread> threads;
        for (int i = 0; i < kLines; ++i) {
            threads.emplace_back([&, i] {
                results[i] = f.call("files.edit", {{"path", s(f.out / L"shared.txt")},
                                                   {"old_text", "line " + std::to_string(i) + " old"},
                                                   {"new_text", "line " + std::to_string(i) + " new"}});
            });
            threads.emplace_back([&] { (void)f.call("files.read", {{"path", s(f.out / L"shared.txt")}}); });
        }
        for (auto& t : threads) t.join();
        for (int i = 0; i < kLines; ++i) ASSERT_TRUE(results[i].ok) << "round " << round << " edit " << i << ": " << results[i].content;
        std::string expected;
        for (int i = 0; i < kLines; ++i) expected += "line " + std::to_string(i) + " new\n";
        ASSERT_EQ(get(f.out / L"shared.txt"), expected) << "round " << round;
    }
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

// ---- client/core-tools Edit All Occurrences (add-edit-replace-all) ----

TEST(FileTools, ReplaceAllReplacesEveryOccurrenceInOneWrite) {
    FileFixture f;
    put(f.out / L"app.js", "oldName();\nconst x = oldName;\noldName.y = 1;\n// oldName\nexport { oldName };\n");
    auto r = f.call("files.edit", {{"path", s(f.out / L"app.js")}, {"old_text", "oldName"}, {"new_text", "newName"}, {"replace_all", true}});
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(r.content.find("Replaced 5 occurrences"), std::string::npos) << r.content;
    EXPECT_EQ(get(f.out / L"app.js"), "newName();\nconst x = newName;\nnewName.y = 1;\n// newName\nexport { newName };\n");
    // Non-overlapping, left to right; the replacement is not searched again.
    put(f.out / L"aaa.txt", "aaaa");
    r = f.call("files.edit", {{"path", s(f.out / L"aaa.txt")}, {"old_text", "aa"}, {"new_text", "aab"}, {"replace_all", true}});
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_EQ(get(f.out / L"aaa.txt"), "aabaab");
}

TEST(FileTools, ReplaceAllWithoutAMatchFailsAndKeepsTheFile) {
    FileFixture f;
    put(f.out / L"app.js", "a();\n");
    auto r = f.call("files.edit", {{"path", s(f.out / L"app.js")}, {"old_text", "zzz"}, {"new_text", "y"}, {"replace_all", true}});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.content.find("not found"), std::string::npos) << r.content;
    EXPECT_EQ(get(f.out / L"app.js"), "a();\n");
}

TEST(FileTools, ReplaceAllKeepsUtf16Encoding) {
    FileFixture f;
    const std::string utf16("\xFF\xFE" "a\0b\0a\0", 8);  // "aba"
    put(f.out / L"u16.txt", utf16);
    auto r = f.call("files.edit", {{"path", s(f.out / L"u16.txt")}, {"old_text", "a"}, {"new_text", "c"}, {"replace_all", true}});
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_EQ(get(f.out / L"u16.txt"), std::string("\xFF\xFE" "c\0b\0c\0", 8));
}

// ---- client/core-tools File Append (bound-tool-call-size) ----

TEST(FileTools, AppendWritesAFileInParts) {
    FileFixture f;
    ASSERT_TRUE(f.call_in("t1", "files.write", {{"path", s(f.out / L"big.html")}, {"content", "A"}}).ok);
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"big.html")}, {"content", "B"}, {"append", true}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"big.html"), "AB");
    // The file stays the agent's own: a later replace needs no read first.
    w = f.call_in("t1", "files.write", {{"path", s(f.out / L"big.html")}, {"content", "C"}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"big.html"), "C");
}

TEST(FileTools, AppendCreatesAMissingFile) {
    FileFixture f;
    auto w = f.call("files.write", {{"path", s(f.out / L"new.txt")}, {"content", "first part"}, {"append", true}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"new.txt"), "first part");
}

TEST(FileTools, AppendKeepsEncodingNeedsNoReadAndRespectsPolicy) {
    FileFixture f;
    // An unread file of the user's is appended to (nothing in it is replaced), in its own encoding.
    put(f.out / L"u16.txt", std::string("\xFF\xFE" "a\0", 4));
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"u16.txt")}, {"content", "b"}, {"append", true}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"u16.txt"), std::string("\xFF\xFE" "a\0b\0", 6));
    // Without append the same call still replaces only after a read.
    EXPECT_FALSE(f.call_in("t1", "files.write", {{"path", s(f.out / L"u16.txt")}, {"content", "x"}}).ok);
    // Outside the writable folders nothing is appended.
    w = f.call("files.write", {{"path", s(f.outside / L"private.txt")}, {"content", "more"}, {"append", true}});
    EXPECT_FALSE(w.ok);
    EXPECT_EQ(get(f.outside / L"private.txt"), "private data\n");
}

// ---- client/core-tools Overwrite Protection and Seen File Content (protect-unread-overwrites) ----

TEST(FileTools, OverwriteWithoutAReadFailsAndKeepsTheFile) {
    FileFixture f;
    put(f.out / L"notes.txt", "the user's notes\n");
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "replaced"}});
    EXPECT_FALSE(w.ok);
    EXPECT_NE(w.content.find("has not been read in this Thread"), std::string::npos) << w.content;
    EXPECT_EQ(get(f.out / L"notes.txt"), "the user's notes\n");
}

TEST(FileTools, OverwriteAfterAFullReadInTheSameThread) {
    FileFixture f;
    put(f.out / L"notes.txt", "the user's notes\n");
    ASSERT_TRUE(f.call_in("t1", "files.read", {{"path", s(f.out / L"notes.txt")}}).ok);
    // Another Thread has not seen it.
    EXPECT_FALSE(f.call_in("t2", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "other"}}).ok);
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "replaced"}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"notes.txt"), "replaced");
    // The agent's own write counts as seen: a second write needs no read.
    w = f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "again"}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"notes.txt"), "again");
}

TEST(FileTools, OverwriteFailsWhenTheFileChangedAfterTheRead) {
    FileFixture f;
    put(f.out / L"notes.txt", "version one\n");
    ASSERT_TRUE(f.call_in("t1", "files.read", {{"path", s(f.out / L"notes.txt")}}).ok);
    put(f.out / L"notes.txt", "the user's version two\n");
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "replaced"}});
    EXPECT_FALSE(w.ok);
    EXPECT_NE(w.content.find("changed after it was last read"), std::string::npos) << w.content;
    EXPECT_EQ(get(f.out / L"notes.txt"), "the user's version two\n");
    // Same size and the same last-write time, different bytes: the hash still tells.
    put(f.out / L"same.txt", "aaaa\n");
    ASSERT_TRUE(f.call_in("t1", "files.read", {{"path", s(f.out / L"same.txt")}}).ok);
    WIN32_FILE_ATTRIBUTE_DATA before{};
    ASSERT_TRUE(::GetFileAttributesExW((f.out / L"same.txt").c_str(), GetFileExInfoStandard, &before));
    put(f.out / L"same.txt", "bbbb\n");
    {
        HANDLE h = ::CreateFileW((f.out / L"same.txt").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(h, INVALID_HANDLE_VALUE);
        ASSERT_TRUE(::SetFileTime(h, nullptr, nullptr, &before.ftLastWriteTime));
        ::CloseHandle(h);
    }
    EXPECT_FALSE(f.call_in("t1", "files.write", {{"path", s(f.out / L"same.txt")}, {"content", "x"}}).ok);
    EXPECT_EQ(get(f.out / L"same.txt"), "bbbb\n");
}

TEST(FileTools, RangedAndTruncatedReadsDoNotCountAsSeen) {
    FileFixture f;
    put(f.out / L"notes.txt", "one\ntwo\nthree\n");
    ASSERT_TRUE(f.call_in("t1", "files.read", {{"path", s(f.out / L"notes.txt")}, {"offset", 1}, {"limit", 2}}).ok);
    EXPECT_FALSE(f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "x"}}).ok);
    EXPECT_EQ(get(f.out / L"notes.txt"), "one\ntwo\nthree\n");
    // Larger than the read limit: a full read is impossible, and the error says to use files.edit or a new file.
    std::string big;
    while (big.size() < 300 * 1024) big += "a line of a large log file\n";
    put(f.out / L"big.log", big);
    ASSERT_TRUE(f.call_in("t1", "files.read", {{"path", s(f.out / L"big.log")}}).ok);
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"big.log")}, {"content", "x"}});
    EXPECT_FALSE(w.ok);
    EXPECT_NE(w.content.find("cannot be replaced with files.write"), std::string::npos) << w.content;
    EXPECT_EQ(get(f.out / L"big.log").size(), big.size());
}

TEST(FileTools, DocumentsAreNeverReplacedWithFilesWrite) {
    FileFixture f;
    put(f.out / L"report.docx", std::string("PK\x03\x04 not really a document", 26));
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"report.docx")}, {"content", "text"}});
    EXPECT_FALSE(w.ok);
    EXPECT_NE(w.content.find("cannot be replaced with files.write"), std::string::npos) << w.content;
    EXPECT_EQ(get(f.out / L"report.docx").substr(0, 2), "PK");
}

TEST(FileTools, AnEditOfAnUnseenFileDoesNotMakeItSeen) {
    FileFixture f;
    put(f.out / L"notes.txt", "alpha\nbeta\n");
    ASSERT_TRUE(f.call_in("t1", "files.edit", {{"path", s(f.out / L"notes.txt")}, {"old_text", "beta"}, {"new_text", "BETA"}}).ok);
    EXPECT_FALSE(f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "x"}}).ok);
    // After a full read, edits keep the file seen.
    ASSERT_TRUE(f.call_in("t1", "files.read", {{"path", s(f.out / L"notes.txt")}}).ok);
    ASSERT_TRUE(f.call_in("t1", "files.edit", {{"path", s(f.out / L"notes.txt")}, {"old_text", "alpha"}, {"new_text", "ALPHA"}}).ok);
    ASSERT_TRUE(f.call_in("t1", "files.write", {{"path", s(f.out / L"notes.txt")}, {"content", "x"}}).ok);
    EXPECT_EQ(get(f.out / L"notes.txt"), "x");
}

TEST(FileTools, ACreatedFileRecordedForTheThreadCanBeReplaced) {
    FileFixture f;
    put(f.out / L"index.html", "<h1>Starter</h1>");
    auto seen = memory_seen_files();
    FileToolsEnv env;
    env.rules = [&f] { return f.rules; };
    env.workspace = f.rules.workspace;
    env.seen = seen;
    f.tools = file_tools(env);
    record_seen_file(*seen, "t1", s(f.out / L"index.html"));  // as apps.create does for its scaffold
    auto w = f.call_in("t1", "files.write", {{"path", s(f.out / L"index.html")}, {"content", "<h1>App</h1>"}});
    ASSERT_TRUE(w.ok) << w.content;
    EXPECT_EQ(get(f.out / L"index.html"), "<h1>App</h1>");
}
