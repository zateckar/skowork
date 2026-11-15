#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "ui/generated_chart.h"

using namespace wb::ui;

TEST(GeneratedChart, KeepsLabelsValuesUnitsAndZeroBaseline) {
    const auto data = wb::json::parse(R"({"type":"bar","unit":"hours","points":[{"label":"A","value":-4},{"label":"B","value":8}]})");
    const auto chart = generated_chart_data(data);
    ASSERT_TRUE(chart);
    EXPECT_EQ(chart->unit, "hours");
    ASSERT_EQ(chart->points.size(), 2u);
    EXPECT_EQ(chart->points[0].label, "A");
    EXPECT_DOUBLE_EQ(chart->points[0].value, -4);
    EXPECT_DOUBLE_EQ(chart->points[0].position, 0);
    EXPECT_DOUBLE_EQ(chart->points[1].position, 1);
    EXPECT_NEAR(chart->zero, 1.0 / 3.0, 1e-12);
}

TEST(GeneratedChart, HandlesConstantEmptyAndExtremeValuesSafely) {
    auto data = wb::json::parse(R"({"type":"line","unit":"units","points":[{"label":"same","value":5}]})");
    auto chart = generated_chart_data(data);
    ASSERT_TRUE(chart);
    EXPECT_DOUBLE_EQ(chart->points[0].position, 1);
    EXPECT_DOUBLE_EQ(chart->zero, 0);
    data["points"] = wb::json::array();
    chart = generated_chart_data(data);
    ASSERT_TRUE(chart);
    EXPECT_TRUE(chart->points.empty());
    data["points"] = wb::json::array({{{"label", "low"}, {"value", -std::numeric_limits<double>::max()}},
                                      {{"label", "high"}, {"value", std::numeric_limits<double>::max()}}});
    chart = generated_chart_data(data);
    ASSERT_TRUE(chart);
    EXPECT_TRUE(std::isfinite(chart->zero));
    EXPECT_NEAR(chart->zero, 0.5, 1e-12);
    EXPECT_DOUBLE_EQ(chart->points[0].position, 0);
    EXPECT_DOUBLE_EQ(chart->points[1].position, 1);
}
