#include <gtest/gtest.h>

#include "ui/generated_values.h"
#include "ui/generated_surface.h"

using namespace wb::ui;

TEST(GeneratedValues, CollectsOnlyDeclaredLabeledFieldsWithValidatedValues) {
    const auto surface = parse_generated_surface(R"({"version":1,"id":"plan","summary":"Plan","components":[{"type":"section","id":"choices","label":"Choices","components":[{"type":"select","id":"cadence","label":"Cadence","options":["Weekly","Monthly"]},{"type":"multiselect","id":"channels","label":"Channels","options":["Mail","Chat"],"value":["Mail"]},{"type":"switch","id":"notify","label":"Notify","value":false},{"type":"text","id":"note","label":"Note","value":""},{"type":"slider","id":"count","label":"Count","min":0,"max":10,"value":2},{"type":"button","id":"submit","label":"Continue"}]}]})");
    ASSERT_TRUE(surface);
    const wb::json draft = {{"cadence", "Weekly"}, {"channels", wb::json::array({"Chat"})}, {"notify", true}, {"note", "Ready"}, {"count", 4}};
    auto result = generated_surface_values(*surface, draft);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 5u);
    EXPECT_EQ((*result)[0].id, "cadence");
    EXPECT_EQ((*result)[0].label, "Cadence");
    EXPECT_EQ((*result)[0].value, "Weekly");
    EXPECT_EQ((*result)[1].value, wb::json::array({"Chat"}));
    auto defaults = generated_surface_values(*surface, wb::json::object());
    ASSERT_TRUE(defaults);
    EXPECT_TRUE((*defaults)[0].value.is_null());
    EXPECT_EQ((*defaults)[1].value, wb::json::array({"Mail"}));
}

TEST(GeneratedValues, RejectsHiddenUnknownAndInvalidDraftValues) {
    const auto surface = parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"select","id":"select","label":"Select","options":["A","B"]},{"type":"multiselect","id":"multi","label":"Multi","options":["A","B"]},{"type":"slider","id":"slider","label":"Slider","min":0,"max":10,"value":5},{"type":"button","id":"send","label":"Send"}]})");
    ASSERT_TRUE(surface);
    EXPECT_FALSE(generated_surface_values(*surface, {{"unknown", true}}));
    EXPECT_FALSE(generated_surface_values(*surface, {{"send", true}}));
    EXPECT_FALSE(generated_surface_values(*surface, {{"select", "C"}}));
    EXPECT_FALSE(generated_surface_values(*surface, {{"multi", wb::json::array({"A", "A"})}}));
    EXPECT_FALSE(generated_surface_values(*surface, {{"slider", 11}}));
    EXPECT_FALSE(generated_surface_values(*surface, {{"slider", "5"}}));
}

TEST(GeneratedValues, SubmissionRequiresSectionsContainingControlsToBeExpanded) {
    const auto surface = parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"section","id":"outer","label":"Outer","components":[{"type":"card","id":"inner","label":"Inner","components":[{"type":"switch","id":"choice","label":"Choice","value":false}]}]},{"type":"section","id":"info","label":"Information","components":[{"type":"paragraph","id":"p","label":"Text","content":"Details"}]},{"type":"button","id":"send","label":"Send"}]})");
    ASSERT_TRUE(surface);
    EXPECT_TRUE(generated_surface_sections_allow_action(*surface, wb::json::object(), "send"));
    EXPECT_TRUE(generated_surface_sections_allow_action(*surface, {{"outer", true}, {"inner", true}, {"info", false}}, "send"));
    EXPECT_FALSE(generated_surface_sections_allow_action(*surface, {{"outer", false}}, "send"));
    EXPECT_FALSE(generated_surface_sections_allow_action(*surface, {{"inner", false}}, "send"));
    EXPECT_FALSE(generated_surface_sections_allow_action(*surface, {{"outer", "true"}}, "send"));
    EXPECT_FALSE(generated_surface_sections_allow_action(*surface, {{"unknown", true}}, "send"));
}
