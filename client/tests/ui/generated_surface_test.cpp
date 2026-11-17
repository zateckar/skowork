#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "ui/generated_surface.h"

using wb::ui::parse_generated_surface;

// improve-generated-surface-guidance: the quoted ask_user form example is a valid form with one button.
TEST(GeneratedSurface, InstructionsQuoteAValidQuestionFormExample) {
    const auto form = parse_generated_surface(wb::ui::generated_question_form_example());
    ASSERT_TRUE(form.has_value());
    int buttons = 0;
    for (const auto& c : (*form)["components"]) buttons += c["type"] == "button";
    EXPECT_EQ(buttons, 1);
    const std::string instructions(wb::ui::generated_surface_instructions());
    EXPECT_NE(instructions.find(std::string(wb::ui::generated_question_form_example())), std::string::npos);
    EXPECT_NE(instructions.find("ask_user"), std::string::npos);
    EXPECT_NE(instructions.find("next message"), std::string::npos);
}

TEST(GeneratedSurface, AcceptsACompositeSurfaceAndCellCitation) {
    EXPECT_TRUE(parse_generated_surface(wb::ui::generated_surface_example()).has_value());
    const char* example = R"({"version":1,"id":"sample","summary":"Comparison","components":[
      {"type":"section","id":"section","label":"Results","components":[
        {"type":"table","id":"table","label":"Costs","columns":["Name","Cost"],"rows":[["Atlas",{"text":"12","citations":["1"]}]]},
        {"type":"bar","id":"chart","label":"Costs","unit":"hours","points":[{"label":"Atlas","value":12}]},
        {"type":"diagram","id":"graph","label":"Plan","nodes":[{"id":"a","label":"Plan"},{"id":"b","label":"Build"}],"edges":[{"from":"a","to":"b"}]},
        {"type":"multiselect","id":"selection","label":"Choose","options":["A","B"],"value":["B"]},
        {"type":"slider","id":"priority","label":"Priority","min":1,"max":5,"value":3},
        {"type":"button","id":"submit","label":"Continue"}]}]})";
    EXPECT_TRUE(parse_generated_surface(example).has_value());
}

TEST(GeneratedSurface, RejectsUntrustedAndInconsistentInput) {
    const std::vector<std::string> invalid = {
        R"({"version":2,"id":"x","summary":"x","components":[]})",
        R"({"version":1,"id":"not an id","summary":"x","components":[]})",
        R"({"version":1,"id":"x","summary":"x","components":[],"script":"run"})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"button","id":"same","label":"A"},{"type":"button","id":"same","label":"B"}]})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"table","id":"t","label":"T","columns":["A","B"],"rows":[["one"]]}]})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"diagram","id":"g","label":"G","nodes":[{"id":"a","label":"A"}],"edges":[{"from":"a","to":"missing"}]}]})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"select","id":"s","label":"S","options":["A"],"value":"B"}]})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"slider","id":"s","label":"S","min":0,"max":1,"value":2}]})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"button","id":"s","label":"S","citations":["not-a-number"]}]})",
        R"({"version":1,"id":"x","summary":"x","components":[{"type":"webview","id":"s","label":"S","url":"https://example.com"}]})",
        R"({"version":1,"version":1,"id":"x","summary":"x","components":[]})",
        "{\"version\":1,\"id\":\"x\",\"summary\":\"x\",\"components\":" + std::string(30, '[') + std::string(30, ']') + "}"
    };
    for (const auto& input : invalid) EXPECT_FALSE(parse_generated_surface(input).has_value()) << input;
    EXPECT_FALSE(parse_generated_surface(std::string(65537, 'x')).has_value());
    const auto too_long = std::string(4097, 'x');
    const wb::json oversized_label = {{"version", 1}, {"id", "x"}, {"summary", too_long}, {"components", wb::json::array()}};
    EXPECT_FALSE(parse_generated_surface(oversized_label.dump()).has_value());
}

TEST(GeneratedSurface, AcceptsRemainingCatalogAndEnforcesLimits) {
    const char* catalog = R"({"version":1,"id":"full","summary":"Choose","components":[
      {"type":"card","id":"card","label":"Details","components":[{"type":"paragraph","id":"copy","label":"Status","content":"Ready"}]},
      {"type":"line","id":"line","label":"Trend","unit":"days","points":[{"label":"Today","value":1.5}]},
      {"type":"select","id":"select","label":"One","options":["A","B"],"value":"A"},
      {"type":"switch","id":"switch","label":"Enabled","value":false},
      {"type":"text","id":"text","label":"Title","value":""}]})";
    EXPECT_TRUE(parse_generated_surface(catalog).has_value());

    wb::json root = {{"version", 1}, {"id", "limits"}, {"summary", "Limits"}, {"components", wb::json::array()}};
    for (int i = 0; i < 41; ++i)
        root["components"].push_back({{"type", "button"}, {"id", "b" + std::to_string(i)}, {"label", "Submit"}});
    EXPECT_FALSE(parse_generated_surface(root.dump()).has_value());
    root["components"].erase(40);
    EXPECT_TRUE(parse_generated_surface(root.dump()).has_value());

    wb::json child = {{"type", "paragraph"}, {"id", "leaf"}, {"label", "Leaf"}, {"content", "Content"}};
    for (int i = 0; i < 8; ++i)
        child = {{"type", "section"}, {"id", "s" + std::to_string(i)}, {"label", "Section"}, {"components", wb::json::array({child})}};
    root["components"] = wb::json::array({child});
    EXPECT_FALSE(parse_generated_surface(root.dump()).has_value());
}

TEST(GeneratedSurface, DiscoversOnlyClosedTopLevelFences) {
    const std::string surface(wb::ui::generated_surface_example());
    const auto fence = "```skowork-ui\n" + surface + "\n```\n";
    EXPECT_EQ(wb::ui::discover_generated_surfaces("Intro\n\n" + fence + "End").size(), 1u);
    EXPECT_EQ(wb::ui::discover_generated_surfaces("```skowork-ui\r\n" + surface + "\r\n```\r\n").size(), 1u);
    EXPECT_EQ(wb::ui::discover_generated_surfaces("```skowork-ui\r\n{\r\n\"version\":1,\r\n\"id\":\"x\",\r\n\"summary\":\"x\",\r\n\"components\":[]}\r\n```\r\n").size(), 1u);
    EXPECT_TRUE(wb::ui::discover_generated_surfaces("```skowork-ui\n" + surface).empty());
    EXPECT_TRUE(wb::ui::discover_generated_surfaces("> ```skowork-ui\n> " + surface + "\n> ```\n").empty());
    EXPECT_TRUE(wb::ui::discover_generated_surfaces("````text\n" + fence + "````\n").empty());
    EXPECT_TRUE(wb::ui::discover_generated_surfaces("- Example\n\n  ```skowork-ui\n  " + surface + "\n  ```\n").empty());
    EXPECT_TRUE(wb::ui::discover_generated_surfaces(std::string(1024 * 1024, 'a') + fence).empty());
    std::string excessive;
    for (int i = 0; i < 17; ++i) excessive += fence;
    EXPECT_TRUE(wb::ui::discover_generated_surfaces(excessive).empty());
}
