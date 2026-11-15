#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/json.h"

namespace wb::ui {

struct GeneratedChartPoint {
    std::string label;
    double value = 0;
    double position = 0;  // normalized from bottom to top
};

struct GeneratedChartData {
    std::string unit;
    double zero = 0;
    std::vector<GeneratedChartPoint> points;
};

// A bounded, overflow-safe scale shared by the visual and its data alternative.
std::optional<GeneratedChartData> generated_chart_data(const json& component);

}  // namespace wb::ui
