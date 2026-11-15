#include "ui/generated_chart.h"

#include <algorithm>
#include <cmath>

namespace wb::ui {

std::optional<GeneratedChartData> generated_chart_data(const json& component) {
    if (!component.is_object() || (jstr(component, "type") != "bar" && jstr(component, "type") != "line") ||
        !component.contains("unit") || !component["unit"].is_string() || !component.contains("points") ||
        !component["points"].is_array() || component["points"].size() > 500) return std::nullopt;
    GeneratedChartData result;
    result.unit = jstr(component, "unit");
    double magnitude = 0;
    for (const auto& point : component["points"]) {
        if (!point.is_object() || !point.contains("label") || !point["label"].is_string() ||
            !point.contains("value") || !point["value"].is_number()) return std::nullopt;
        const double value = point["value"].get<double>();
        if (!std::isfinite(value)) return std::nullopt;
        result.points.push_back({jstr(point, "label"), value, 0});
        magnitude = std::max(magnitude, std::abs(value));
    }
    if (magnitude == 0) {
        result.zero = 0.5;
        for (auto& point : result.points) point.position = result.zero;
        return result;
    }
    double low = 0, high = 0;
    for (const auto& point : result.points) {
        const double value = point.value / magnitude;
        low = std::min(low, value);
        high = std::max(high, value);
    }
    const double span = high - low;
    result.zero = -low / span;
    for (auto& point : result.points) point.position = (point.value / magnitude - low) / span;
    return result;
}

}  // namespace wb::ui
