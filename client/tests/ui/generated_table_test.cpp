#include <gtest/gtest.h>

#include "ui/generated_table.h"

using namespace wb::ui;

TEST(GeneratedTable, SortingAndFilteringAreLocalAndReversible) {
    const wb::json table = wb::json::parse(R"({"columns":["Project","Hours"],"rows":[["Boreal","18"],["Atlas",{"text":"12","citations":["1"]}],["Cedar","7"]]})");
    GeneratedTableState state;
    EXPECT_EQ(generated_table_rows(table, state), (std::vector<size_t>{0, 1, 2}));
    state.sort_column = 0;
    EXPECT_EQ(generated_table_rows(table, state), (std::vector<size_t>{1, 0, 2}));
    state.descending = true;
    EXPECT_EQ(generated_table_rows(table, state), (std::vector<size_t>{2, 0, 1}));
    state.filter = "12";
    EXPECT_EQ(generated_table_rows(table, state), (std::vector<size_t>{1}));
    state.filter = "missing";
    EXPECT_TRUE(generated_table_rows(table, state).empty());
    state.filter.clear();
    state.sort_column.reset();
    EXPECT_EQ(generated_table_rows(table, state), (std::vector<size_t>{0, 1, 2}));
}

TEST(GeneratedTable, RestoredStateIsBoundedAndInvalidColumnsDoNotCrash) {
    const wb::json table = {{"columns", {"Name"}}, {"rows", {{"A"}, {"B"}}}};
    auto state = generated_table_state_from_json({{"sort_column", 99}, {"descending", true}, {"filter", "A"}}, 1);
    EXPECT_FALSE(state.sort_column.has_value());
    EXPECT_EQ(generated_table_rows(table, state), (std::vector<size_t>{0}));
    auto restored = generated_table_state_from_json({{"sort_column", 0}, {"descending", true}, {"filter", "B"}}, 1);
    EXPECT_EQ(generated_table_state_to_json(restored), (wb::json{{"sort_column", 0}, {"descending", true}, {"filter", "B"}}));
    EXPECT_EQ(generated_table_rows(table, restored), (std::vector<size_t>{1}));
    EXPECT_LE(generated_table_state_from_json({{"filter", std::string(9000, 'x')}}, 1).filter.size(), 4096u);
}
