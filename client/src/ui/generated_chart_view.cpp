#include "ui/generated_chart_view.h"

#include <algorithm>
#include <cmath>

#include "ui/text16.h"

namespace wb::ui {
namespace {

class GeneratedChartPlot : public Node {
public:
    GeneratedChartPlot(std::string id, std::u16string title, GeneratedChartData data, bool bar)
        : Node(std::move(id)), data_(std::move(data)), bar_(bar) {
        role = Role::image;
        name = std::move(title);
        height = Sizing::fixed;
        fixed_h = 160;
    }

    void draw(Canvas& canvas) override {
        const Rect f = frame().inset(6);
        if (f.empty()) return;
        canvas.push_clip(f);
        const float baseline = f.y + f.h * static_cast<float>(1 - data_.zero);
        canvas.line(f.x, baseline, f.right(), baseline, Paint::border);
        const size_t count = data_.points.size();
        if (count) {
            const float step = f.w / static_cast<float>(count);
            float previous_x = 0, previous_y = 0;
            for (size_t i = 0; i < count; ++i) {
                const float x = f.x + (static_cast<float>(i) + 0.5f) * step;
                const float y = f.y + f.h * static_cast<float>(1 - data_.points[i].position);
                if (bar_) {
                    const float bar_width = std::max(1.f, step * 0.7f);
                    canvas.fill({x - bar_width / 2, std::min(y, baseline), bar_width, std::abs(y - baseline)}, Paint::accent);
                } else {
                    if (i) canvas.line(previous_x, previous_y, x, y, Paint::accent, 2);
                    canvas.round({x - 2, y - 2, 4, 4}, 2, Paint::accent);
                    previous_x = x;
                    previous_y = y;
                }
            }
        }
        canvas.pop_clip();
    }

private:
    GeneratedChartData data_;
    bool bar_;
};
}  // namespace

GeneratedChartView::GeneratedChartView(Ui& ui, const json& component, GeneratedCitationResolver resolver)
    : Box(jstr(component, "id"), Paint::surface, 6, Paint::border) {
    role = Role::group;
    name = to_utf16(jstr(component, "label"));
    padding = Insets::all(8);
    gap = 6;
    emplace<Label>(ui, name, TextStyle::body_strong);
    if (component.contains("citations")) add_generated_citations(ui, *this, component["citations"], resolver, automation_id());
    auto data = generated_chart_data(component);
    if (!data) return;  // The host only constructs a view after validating the whole surface.
    emplace<GeneratedChartPlot>(automation_id() + "-plot", name, *data, jstr(component, "type") == "bar");
    const auto& points = component["points"];
    for (size_t i = 0; i < data->points.size(); ++i) {
        const auto text = to_utf16(data->points[i].label + ": " + points[i]["value"].dump() + " " + data->unit);
        const auto& point = points[i];
        if (point.contains("citations") && !point["citations"].empty()) {
            auto* group = emplace<Box>();
            group->emplace<Label>(ui, text, TextStyle::body);
            add_generated_citations(ui, *group, point["citations"], resolver, automation_id() + "-point-" + std::to_string(i));
        } else emplace<Label>(ui, text, TextStyle::body);
    }
}

}  // namespace wb::ui
