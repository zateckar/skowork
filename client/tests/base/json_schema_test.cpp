#include "base/json_schema.h"

#include <gtest/gtest.h>

using namespace wb;

namespace {

const json kSchema = json::parse(R"({
  "type": "object",
  "properties": {
    "path": {"type": "string", "minLength": 1},
    "mode": {"type": "string", "enum": ["read", "write"]},
    "count": {"type": "integer", "minimum": 1, "maximum": 10},
    "tags": {"type": "array", "items": {"type": "string"}, "maxItems": 2},
    "opt": {"type": ["string", "null"]}
  },
  "required": ["path"],
  "additionalProperties": false
})");

}  // namespace

TEST(JsonSchema, AcceptsAValidObject) {
    EXPECT_TRUE(validate_json(kSchema, json{{"path", "a"}, {"mode", "read"}, {"count", 3}, {"tags", {"x"}}, {"opt", nullptr}}).empty());
    EXPECT_TRUE(validate_json(kSchema, json{{"path", "a"}, {"count", 2.0}}).empty());  // integral float is an integer
}

TEST(JsonSchema, ReportsEachViolation) {
    const auto e = validate_json(kSchema, json{{"mode", "x"}, {"count", 11}, {"tags", {"a", "b", "c"}}, {"extra", 1}});
    EXPECT_GE(e.size(), 5U);
    const auto joined = [&] {
        std::string s;
        for (const auto& m : e) s += m + "\n";
        return s;
    }();
    EXPECT_NE(joined.find("missing required property 'path'"), std::string::npos) << joined;
    EXPECT_NE(joined.find("$.mode"), std::string::npos) << joined;
    EXPECT_NE(joined.find("$.count"), std::string::npos) << joined;
    EXPECT_NE(joined.find("$.tags"), std::string::npos) << joined;
    EXPECT_NE(joined.find("unexpected property 'extra'"), std::string::npos) << joined;
}

TEST(JsonSchema, TypeMismatchesAndNesting) {
    EXPECT_FALSE(validate_json(kSchema, json{{"path", 5}}).empty());
    EXPECT_FALSE(validate_json(kSchema, json{{"path", "a"}, {"count", 1.5}}).empty());
    EXPECT_FALSE(validate_json(kSchema, json{{"path", "a"}, {"tags", {1}}}).empty());
    EXPECT_FALSE(validate_json(kSchema, json::array()).empty());
    EXPECT_FALSE(validate_json(kSchema, json{{"path", ""}}).empty());
}

TEST(JsonSchema, ErrorCountIsCapped) {
    json schema = {{"type", "array"}, {"items", {{"type", "string"}}}};
    json value = json::array();
    for (int i = 0; i < 50; ++i) value.push_back(i);
    EXPECT_EQ(validate_json(schema, value, 5).size(), 5U);
}

TEST(JsonSchema, AnyOfOneOfAndConst) {
    const json schema = {{"anyOf", {json{{"type", "string"}}, json{{"type", "integer"}}}}};
    EXPECT_TRUE(validate_json(schema, "x").empty());
    EXPECT_TRUE(validate_json(schema, 3).empty());
    EXPECT_FALSE(validate_json(schema, true).empty());
    EXPECT_TRUE(validate_json(json{{"const", 7}}, 7).empty());
    EXPECT_FALSE(validate_json(json{{"const", 7}}, 8).empty());
}

TEST(JsonSchema, DeepSchemasDoNotRecurseForever) {
    json schema = {{"type", "object"}};
    json* cur = &schema;
    json value = json::object();
    json* vcur = &value;
    for (int i = 0; i < 200; ++i) {
        (*cur)["properties"] = {{"x", {{"type", "object"}}}};
        cur = &(*cur)["properties"]["x"];
        (*vcur)["x"] = json::object();
        vcur = &(*vcur)["x"];
    }
    EXPECT_TRUE(validate_json(schema, value).empty());
}
