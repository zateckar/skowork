// Portable tests of the client/mcp-client protocol core (add-core-tools-and-mcp tasks 3.1, 3.4).
#include "mcp/protocol.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::mcp;

TEST(McpProtocol, ClassifiesJsonRpcMessages) {
    EXPECT_EQ(parse_message(R"({"jsonrpc":"2.0","id":3,"result":{"x":1}})").kind, Message::Kind::response);
    EXPECT_EQ(parse_message(R"({"jsonrpc":"2.0","id":"a","method":"ping"})").kind, Message::Kind::request);
    EXPECT_EQ(parse_message(R"({"jsonrpc":"2.0","method":"notifications/progress","params":{}})").kind, Message::Kind::notification);
    const Message err = parse_message(R"({"jsonrpc":"2.0","id":4,"error":{"code":-32601,"message":"nope"}})");
    ASSERT_EQ(err.kind, Message::Kind::response);
    ASSERT_TRUE(err.error.has_value());
    EXPECT_EQ(err.error->code, -32601);
    EXPECT_EQ(parse_message("[1,2]").kind, Message::Kind::invalid);
    EXPECT_EQ(parse_message("{not json").kind, Message::Kind::invalid);
    EXPECT_EQ(parse_message(R"({"id":{"x":1},"result":1})").kind, Message::Kind::invalid);
    EXPECT_EQ(parse_message(R"({"method":5})").kind, Message::Kind::invalid);
}

TEST(McpProtocol, LineFramerSplitsAndBoundsLines) {
    LineFramer f(16);
    std::vector<std::string> out;
    EXPECT_TRUE(f.feed("{\"a\":1}\r", out));
    EXPECT_TRUE(f.feed("\n\n{\"b\"", out));
    EXPECT_TRUE(f.feed(":2}\n", out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "{\"a\":1}");
    EXPECT_EQ(out[1], "{\"b\":2}");
    EXPECT_FALSE(f.feed(std::string(40, 'x'), out));
    EXPECT_TRUE(f.failed());
    EXPECT_FALSE(f.feed("{}\n", out));
}

TEST(McpProtocol, ParsesToolPagesAndSkipsBadNames) {
    std::string cursor;
    const json page = {{"tools", json::array({{{"name", "search"}, {"description", "d"}, {"annotations", {{"readOnlyHint", true}}}},
                                               {{"name", "bad name"}},
                                               {{"name", "../x"}},
                                               {{"description", "no name"}}})},
                       {"nextCursor", "c2"}};
    auto tools = parse_tools_page(page, cursor);
    ASSERT_TRUE(tools.ok());
    ASSERT_EQ(tools->size(), 1u);
    EXPECT_EQ((*tools)[0].name, "search");
    EXPECT_TRUE((*tools)[0].read_only_hint);
    EXPECT_EQ((*tools)[0].input_schema["type"], "object");
    EXPECT_EQ(cursor, "c2");
    EXPECT_FALSE(parse_tools_page(json{{"tools", 1}}, cursor).ok());
}

TEST(McpProtocol, CallResultsBecomeText) {
    const json r = {{"content", json::array({{{"type", "text"}, {"text", "hello"}},
                                             {{"type", "image"}, {"mimeType", "image/png"}, {"data", "AAAA"}},
                                             {{"type", "resource"}, {"resource", {{"uri", "page://7"}, {"text", "body"}}}},
                                             {{"type", "resource_link"}, {"uri", "file:///x"}, {"name", "x"}}})}};
    const CallText c = call_result_text(r);
    EXPECT_FALSE(c.is_error);
    EXPECT_NE(c.text.find("hello"), std::string::npos);
    EXPECT_NE(c.text.find("[image: image/png, not shown]"), std::string::npos);
    EXPECT_EQ(c.text.find("AAAA"), std::string::npos);
    EXPECT_NE(c.text.find("[resource page://7]\nbody"), std::string::npos);
    EXPECT_NE(c.text.find("[resource: x <file:///x>]"), std::string::npos);
    EXPECT_EQ(c.item, "page://7");

    const CallText s = call_result_text(json{{"content", json::array()}, {"structuredContent", {{"n", 1}}}, {"isError", true}});
    EXPECT_TRUE(s.is_error);
    EXPECT_NE(s.text.find("\"n\": 1"), std::string::npos);
    EXPECT_TRUE(call_result_text(json(5)).is_error);
}

TEST(McpProtocol, ItemIdentifierFromResultOrArguments) {
    CallText none;
    EXPECT_EQ(item_identifier(json{{"key", "DEMO-1"}}, none), "DEMO-1");
    EXPECT_EQ(item_identifier(json{{"page_id", 42}}, none), "42");
    EXPECT_EQ(item_identifier(json{{"query", "x"}}, none), "");
    CallText withItem;
    withItem.item = "jira://issue/DEMO-2";
    EXPECT_EQ(item_identifier(json{{"key", "DEMO-1"}}, withItem), "jira://issue/DEMO-2");
}

TEST(McpProtocol, ExcludedToolIsNeverSelected) {
    // Scenario: Excluded tool (delete_issue excluded for jira-corp).
    std::string why;
    EXPECT_FALSE(tool_selected("delete_issue", {}, {"delete_issue"}, &why));
    EXPECT_NE(why.find("delete_issue"), std::string::npos);
    EXPECT_TRUE(tool_selected("search_issues", {}, {"delete_issue"}));
    EXPECT_FALSE(tool_selected("delete_all", {"*"}, {"delete_*"}));  // exclude wins over include
    EXPECT_FALSE(tool_selected("other", {"search_*"}, {}, &why));
    EXPECT_EQ(why, "not in the catalog's include list");
    EXPECT_TRUE(tool_pattern_match("a*b*c", "aXXbYYc"));
    EXPECT_FALSE(tool_pattern_match("a*b", "ab_"));
    EXPECT_FALSE(tool_pattern_match("Search", "search"));
}

TEST(McpProtocol, CatalogEntriesAreValidated) {
    const std::string sha(64, 'a');
    std::vector<std::string> problems;
    const json servers = json::array({
        {{"id", "jira-corp"}, {"name", "Jira"}, {"kind", "remote"}, {"url", "https://jira.example/mcp"}, {"auth", "user-token"}},
        {{"id", "old-auth"}, {"kind", "remote"}, {"url", "https://x"}, {"auth", "audience-token"}},
        {{"id", "local-tool"}, {"kind", "local"}, {"package", sha}, {"args", {"bin\\server.exe", "--stdio"}}, {"env", {{"MODE", "ro"}}}},
        {{"id", "evil"}, {"kind", "local"}, {"package", sha}, {"args", {"..\\..\\cmd.exe"}}},
        {{"id", "abs"}, {"kind", "local"}, {"package", sha}, {"args", {"C:\\Windows\\cmd.exe"}}},
        {{"id", "nohash"}, {"kind", "local"}, {"package", "abc"}, {"args", {"x.exe"}}},
        {{"id", "bad id!"}, {"kind", "remote"}, {"url", "https://x"}},
        {{"id", "jira-corp"}, {"kind", "remote"}, {"url", "https://dup"}},
    });
    const auto entries = parse_entries(&servers, &problems);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].auth, "user-token");
    EXPECT_EQ(entries[1].env.size(), 1u);
    EXPECT_EQ(problems.size(), 6u);  // audience-token is no longer an authentication mode
    EXPECT_TRUE(safe_relative_path("bin/server.exe"));
    EXPECT_FALSE(safe_relative_path("/server.exe"));
    EXPECT_FALSE(safe_relative_path("a/./b"));
    EXPECT_FALSE(safe_relative_path("a:b"));
}

TEST(McpProtocol, InitializeHandshakeMessages) {
    const json p = initialize_params("2025-03-26", "1.2.3");
    EXPECT_EQ(p["protocolVersion"], "2025-03-26");
    EXPECT_EQ(p["clientInfo"]["name"], "Skowork");
    EXPECT_EQ(initialize_params("", "1")["protocolVersion"], kDefaultProtocolVersion);
    auto info = parse_initialize(json{{"protocolVersion", "2025-06-18"}, {"capabilities", {{"tools", json::object()}}}, {"serverInfo", {{"name", "s"}}}});
    ASSERT_TRUE(info.ok());
    EXPECT_TRUE(info->has_tools);
    EXPECT_EQ(info->name, "s");
    EXPECT_FALSE(parse_initialize(json::object()).ok());
}
