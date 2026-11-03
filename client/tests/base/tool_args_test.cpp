// client/model-access Tool Call Validation And Repair: the argument parser, and a deterministic fuzz smoke test
// (the real fuzz target is client/fuzz/tool_args_fuzz.cpp).
#include "base/json_schema.h"
#include "base/tool_args.h"

#include <gtest/gtest.h>

#include <random>

using namespace wb;

namespace {

json parse_ok(const std::string& text, bool expect_repaired) {
    const auto r = parse_tool_arguments(text);
    EXPECT_TRUE(r.value.has_value()) << text << " -> " << r.error;
    EXPECT_EQ(r.repaired, expect_repaired) << text;
    return r.value.value_or(json());
}

}  // namespace

TEST(ToolArgs, ValidJsonPassesUnchanged) {
    EXPECT_EQ(parse_ok(R"({"path":"a.txt","n":2})", false), (json{{"path", "a.txt"}, {"n", 2}}));
    EXPECT_EQ(parse_ok("", false), json::object());
    EXPECT_EQ(parse_ok("  null ", false), json::object());
}

TEST(ToolArgs, TrailingCommas) {
    EXPECT_EQ(parse_ok(R"({"a":1,"b":[1,2,],})", true), (json{{"a", 1}, {"b", {1, 2}}}));
    EXPECT_EQ(parse_ok("{\"a\": 1 ,\n}", true), (json{{"a", 1}}));
}

TEST(ToolArgs, SingleQuotesAndUnquotedKeys) {
    EXPECT_EQ(parse_ok("{'path': 'it\\'s', \"q\": 'say \"hi\"'}", true), (json{{"path", "it's"}, {"q", "say \"hi\""}}));
    EXPECT_EQ(parse_ok("{path: \"x\", n: 3}", true), (json{{"path", "x"}, {"n", 3}}));
}

TEST(ToolArgs, RawNewlinesAndTabsInStrings) {
    EXPECT_EQ(parse_ok("{\"text\":\"line1\nline2\tend\"}", true), (json{{"text", "line1\nline2\tend"}}));
}

TEST(ToolArgs, LoneBackslashesAreKept) {
    EXPECT_EQ(parse_ok(R"({"path":"C:\Users\x"})", true), (json{{"path", "C:\\Users\\x"}}));
}

TEST(ToolArgs, CodeFences) {
    EXPECT_EQ(parse_ok("```json\n{\"a\": 1}\n```", true), (json{{"a", 1}}));
    EXPECT_EQ(parse_ok("```\n{\"a\": 1,}\n```\n", true), (json{{"a", 1}}));
    EXPECT_EQ(parse_ok("```{\"a\":1}```", true), (json{{"a", 1}}));
}

TEST(ToolArgs, PythonLiterals) {
    EXPECT_EQ(parse_ok("{'a': True, 'b': None, 'c': False}", true), (json{{"a", true}, {"b", nullptr}, {"c", false}}));
}

TEST(ToolArgs, DoubleEncodedArguments) {
    EXPECT_EQ(parse_ok(R"("{\"a\":1}")", true), (json{{"a", 1}}));
}

TEST(ToolArgs, AmbiguousOrBrokenInputFails) {
    for (const char* bad : {"{\"a\":", "{\"a\": \"unterminated}", "[1,2]", "42", "{\"a\": hello}", "{a b}", "{\"a\":1}}", "```json",
                            "{\"a\": tru}"}) {
        const auto r = parse_tool_arguments(bad);
        EXPECT_FALSE(r.value.has_value()) << bad;
        EXPECT_FALSE(r.error.empty()) << bad;
    }
}

TEST(ToolArgs, LimitsAreEnforced) {
    EXPECT_FALSE(parse_tool_arguments(std::string(kMaxToolArgumentBytes + 1, ' ')).value.has_value());
    EXPECT_FALSE(parse_tool_arguments(std::string(100000, '[')).value.has_value());
    std::string deep;
    for (int i = 0; i < kMaxToolArgumentDepth + 5; ++i) deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < kMaxToolArgumentDepth + 5; ++i) deep += "}";
    EXPECT_FALSE(parse_tool_arguments(deep).value.has_value());
}

// Deterministic mutation fuzzing of the parser (the AFL++ target runs the same entry point for longer).
TEST(ToolArgs, MutationSmoke) {
    const std::vector<std::string> seeds = {
        R"({"path":"a.txt","content":"hi\nthere","n":[1,2,3],"o":{"x":true}})",
        "{'a': 'b', c: [1, 2, ], }",
        "```json\n{\"a\": \"x\ty\"}\n```",
        R"("{\"a\":{\"b\":null}}")",
        "{\"s\":\"\\u00e9\\ud83d\\ude00\"}",
    };
    const std::string alphabet = "{}[]\"',:\\ \n\t\r`0123456789-+.eEtruefalsn\x01\xff";
    std::mt19937 rng(12345);
    for (int iter = 0; iter < 30000; ++iter) {
        std::string s = seeds[rng() % seeds.size()];
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int e = 0; e < edits && !s.empty(); ++e) {
            const size_t pos = rng() % s.size();
            switch (rng() % 4) {
            case 0: s.erase(pos, 1 + rng() % 3); break;
            case 1: s.insert(pos, 1, alphabet[rng() % alphabet.size()]); break;
            case 2: s[pos] = alphabet[rng() % alphabet.size()]; break;
            default: s.resize(pos); break;
            }
        }
        const auto r = parse_tool_arguments(s);
        if (r.value) EXPECT_TRUE(r.value->is_object());
        else EXPECT_FALSE(r.error.empty());
    }
}
