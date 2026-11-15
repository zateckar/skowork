#include "ui/generated_diagram_view.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "ui/text16.h"

namespace wb::ui {
namespace {
struct DiagramNode { std::string id; std::u16string label; json citations; };
struct DiagramEdge { size_t from, to; json citations; };

class GeneratedDiagramPlot : public Node {
public:
    GeneratedDiagramPlot(Ui& ui, std::string id, std::u16string title, std::vector<DiagramNode> nodes,
                         std::vector<DiagramEdge> edges)
        : Node(std::move(id)), ui_(ui), nodes_(std::move(nodes)), edges_(std::move(edges)) {
        role = Role::image;
        name = std::move(title);
        height = Sizing::fixed;
        fixed_h = 30.f + static_cast<float>((nodes_.size() + 3) / 4) * 76.f;
    }

    void draw(Canvas& canvas) override {
        const Rect f = frame().inset(4);
        if (f.empty() || nodes_.empty()) return;
        const size_t columns = std::min<size_t>(4, nodes_.size());
        const float step = f.w / static_cast<float>(columns);
        const float box_width = std::max(1.f, step - 12.f);
        std::vector<Rect> boxes;
        boxes.reserve(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i)
            boxes.push_back({f.x + static_cast<float>(i % columns) * step + 6.f,
                             f.y + 22.f + static_cast<float>(i / columns) * 76.f, box_width, 42.f});
        canvas.push_clip(f);
        for (const auto& edge : edges_) {
            const Rect& a = boxes[edge.from];
            const Rect& b = boxes[edge.to];
            if (edge.from == edge.to) {
                const float x = a.x + a.w / 2, y = a.y;
                canvas.line(x - 8, y, x - 8, y - 12, Paint::text_secondary);
                canvas.line(x - 8, y - 12, x + 8, y - 12, Paint::text_secondary);
                canvas.line(x + 8, y - 12, x + 8, y, Paint::text_secondary);
                canvas.line(x + 8, y, x + 5, y - 5, Paint::text_secondary);
                canvas.line(x + 8, y, x + 11, y - 5, Paint::text_secondary);
                continue;
            }
            const float ax = a.x + a.w / 2, ay = a.y + a.h / 2, bx = b.x + b.w / 2, by = b.y + b.h / 2;
            const float dx = bx - ax, dy = by - ay, length = std::sqrt(dx * dx + dy * dy);
            if (length <= 0) continue;
            const float ux = dx / length, uy = dy / length;
            const auto inset = [&](const Rect& box) {
                const float tx = std::abs(ux) > 0.0001f ? box.w / (2 * std::abs(ux)) : length;
                const float ty = std::abs(uy) > 0.0001f ? box.h / (2 * std::abs(uy)) : length;
                return std::min(tx, ty);
            };
            const float begin = inset(a), end = inset(b);
            const float x = bx - ux * end, y = by - uy * end;
            canvas.line(ax + ux * begin, ay + uy * begin, x, y, Paint::text_secondary);
            canvas.line(x, y, x - ux * 7 + uy * 4, y - uy * 7 - ux * 4, Paint::text_secondary);
            canvas.line(x, y, x - ux * 7 - uy * 4, y - uy * 7 + ux * 4, Paint::text_secondary);
        }
        for (size_t i = 0; i < boxes.size(); ++i) {
            canvas.round(boxes[i], 4, Paint::accent_surface, Paint::border);
            canvas.push_clip(boxes[i]);
            auto* text = ui_.layout(nodes_[i].label, TextStyle::body, std::max(1.f, boxes[i].w - 8), true);
            canvas.text(text, boxes[i].x + 4, boxes[i].y + 4, Paint::text);
            canvas.pop_clip();
        }
        canvas.pop_clip();
    }

private:
    Ui& ui_;
    std::vector<DiagramNode> nodes_;
    std::vector<DiagramEdge> edges_;
};
}  // namespace

GeneratedDiagramView::GeneratedDiagramView(Ui& ui, const json& component, GeneratedCitationResolver resolver)
    : Box(jstr(component, "id"), Paint::surface, 6, Paint::border) {
    role = Role::group;
    name = to_utf16(jstr(component, "label"));
    padding = Insets::all(8);
    gap = 6;
    emplace<Label>(ui, name, TextStyle::body_strong);
    if (component.contains("citations")) add_generated_citations(ui, *this, component["citations"], resolver, automation_id());
    std::vector<DiagramNode> nodes;
    std::vector<DiagramEdge> edges;
    std::map<std::string, size_t> indexes;
    for (const auto& item : component["nodes"]) {
        const auto id = jstr(item, "id");
        indexes.emplace(id, nodes.size());
        nodes.push_back({id, to_utf16(jstr(item, "label")), item.value("citations", json::array())});
    }
    for (const auto& item : component["edges"]) {
        const auto from = indexes.find(jstr(item, "from")), to = indexes.find(jstr(item, "to"));
        if (from != indexes.end() && to != indexes.end()) edges.push_back({from->second, to->second, item.value("citations", json::array())});
    }
    emplace<GeneratedDiagramPlot>(ui, automation_id() + "-plot", name, nodes, edges);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.citations.empty()) emplace<Label>(ui, node.label, TextStyle::body);
        else {
            auto* group = emplace<Box>();
            group->emplace<Label>(ui, node.label, TextStyle::body);
            add_generated_citations(ui, *group, node.citations, resolver, automation_id() + "-node-" + std::to_string(i));
        }
    }
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto& edge = edges[i];
        const auto text = nodes[edge.from].label + u" → " + nodes[edge.to].label;
        if (edge.citations.empty()) emplace<Label>(ui, text, TextStyle::body);
        else {
            auto* group = emplace<Box>();
            group->emplace<Label>(ui, text, TextStyle::body);
            add_generated_citations(ui, *group, edge.citations, resolver, automation_id() + "-edge-" + std::to_string(i));
        }
    }
}

}  // namespace wb::ui
