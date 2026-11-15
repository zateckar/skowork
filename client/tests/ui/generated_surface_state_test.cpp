#include <gtest/gtest.h>

#include "ui/generated_surface_state.h"
#include "ui/generated_surface.h"
#include "ui/generated_save_queue.h"

using namespace wb::ui;

TEST(GeneratedSurfaceViewState, RestoresOnlyFieldsAndExplorationBoundToTheSurface) {
    auto surface = parse_generated_surface(R"({"version":1,"id":"s","summary":"S","components":[{"type":"section","id":"details","label":"Details","components":[{"type":"table","id":"rows","label":"Rows","columns":["Name"],"rows":[["Atlas"]]},{"type":"switch","id":"notify","label":"Notify","value":false}]}]})");
    ASSERT_TRUE(surface);
    GeneratedSurfaceViewState state;
    state.sections["details"] = std::make_shared<bool>(false);
    state.tables["rows"] = std::make_shared<GeneratedTableState>();
    state.tables["rows"]->sort_column = 0;
    state.tables["rows"]->filter = "Atlas";
    (*state.draft)["notify"] = true;
    auto saved = generated_surface_view_state_to_json(state);
    saved["sections"]["unknown"] = true;
    auto restored = generated_surface_view_state_from_json(*surface, saved);
    ASSERT_TRUE(restored.sections.count("details"));
    EXPECT_FALSE(*restored.sections.at("details"));
    EXPECT_FALSE(restored.sections.count("unknown"));
    ASSERT_TRUE(restored.tables.count("rows"));
    EXPECT_EQ(restored.tables.at("rows")->sort_column, 0u);
    EXPECT_EQ(restored.tables.at("rows")->filter, "Atlas");
    EXPECT_EQ((*restored.draft)["notify"], true);
    saved["draft"]["unknown"] = "hidden";
    restored = generated_surface_view_state_from_json(*surface, saved);
    EXPECT_TRUE(restored.draft->empty());
}

TEST(GeneratedSaveQueue, SerializesCoalescesAndRetriesLatestState) {
    std::vector<wb::json> sent;
    std::vector<GeneratedSaveQueue::Done> pending;
    int errors = 0;
    auto writer = std::make_shared<GeneratedSaveQueue>([&](const wb::json& value, GeneratedSaveQueue::Done done) {
        sent.push_back(value);
        pending.push_back(std::move(done));
    }, [&] { ++errors; });
    writer->save({{"value", 1}});
    writer->save({{"value", 2}});
    writer->save({{"value", 3}});
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(writer->latest()->at("value"), 3);
    auto finish_first = std::move(pending[0]);
    finish_first(true);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1]["value"], 3);
    auto fail_second = std::move(pending[1]);
    fail_second(false);
    EXPECT_EQ(errors, 1);
    EXPECT_TRUE(writer->needs_retry());
    writer->retry();
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[2]["value"], 3);
    auto finish_third = std::move(pending[2]);
    finish_third(true);
    EXPECT_FALSE(writer->needs_retry());
    EXPECT_FALSE(writer->pending());
}

TEST(GeneratedSaveQueue, SurvivesViewDestructionAndIgnoresDuplicateCompletion) {
    std::vector<wb::json> sent;
    std::vector<GeneratedSaveQueue::Done> replies;
    auto writer = std::make_shared<GeneratedSaveQueue>([&](const wb::json& value, GeneratedSaveQueue::Done done) {
        sent.push_back(value);
        replies.push_back(std::move(done));
    });
    std::weak_ptr<GeneratedSaveQueue> weak = writer;
    writer->save({{"value", 1}});
    writer->save({{"value", 2}});
    writer.reset();
    ASSERT_FALSE(weak.expired());
    auto first = std::move(replies[0]);
    first(true);
    ASSERT_EQ(sent.size(), 2u);
    first(false);
    ASSERT_FALSE(weak.expired());
    EXPECT_TRUE(weak.lock()->pending());
    auto second = std::move(replies[1]);
    second(true);
    first = {};
    second = {};
    EXPECT_TRUE(weak.expired());
}

TEST(GeneratedSaveQueue, AbandonSuppressesLateFailureAndPendingEdits) {
    std::vector<GeneratedSaveQueue::Done> replies;
    int errors = 0;
    auto writer = std::make_shared<GeneratedSaveQueue>([&](const wb::json&, GeneratedSaveQueue::Done done) {
        replies.push_back(std::move(done));
    }, [&] { ++errors; });
    writer->save({{"value", 1}});
    writer->save({{"value", 2}});
    writer->abandon();
    EXPECT_FALSE(writer->pending());
    EXPECT_FALSE(writer->needs_retry());
    ASSERT_EQ(replies.size(), 1u);
    replies.front()(false);
    EXPECT_EQ(errors, 0);
    EXPECT_EQ(replies.size(), 1u);
}
