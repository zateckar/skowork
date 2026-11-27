#include "ui/generated_surface_view.h"

#include "ui/generated_chart_view.h"
#include "ui/generated_content_view.h"
#include "ui/generated_diagram_view.h"
#include "ui/generated_field_view.h"
#include "ui/generated_section_view.h"
#include "ui/generated_surface.h"
#include "ui/generated_table_view.h"
#include "ui/text16.h"

namespace wb::ui {
namespace {

bool has_citations(const json& value) {
    if (value.is_object() && value.contains("citations")) return true;
    if (value.is_array() || value.is_object())
        for (const auto& child : value) if (has_citations(child)) return true;
    return false;
}

bool supports_components(const json& components, bool allow_interactions) {
    for (const auto& component : components) {
        const auto type = jstr(component, "type");
        if (type == "section" || type == "card") {
            if (!supports_components(component["components"], allow_interactions)) return false;
        } else if (type != "paragraph" && type != "table" && !is_generated_display_component(type) && type != "bar" && type != "line" && type != "diagram" &&
                   !(allow_interactions && (type == "select" || type == "multiselect" || type == "switch" ||
                                            type == "text" || type == "slider" || type == "button"))) return false;
    }
    return true;
}

void compose(Ui& ui, Node& parent, const json& surface, const json& components,
             const std::shared_ptr<GeneratedSurfaceViewState>& state, const std::function<void()>& changed,
             const GeneratedActionView::Submit& submit, const GeneratedCitationResolver& resolver) {
    for (const auto& component : components) {
        const auto type = jstr(component, "type"), id = jstr(component, "id");
        const auto label = to_utf16(jstr(component, "label"));
        if (type == "section" || type == "card") {
            auto& expanded = state->sections[id];
            if (!expanded) expanded = std::make_shared<bool>(true);
            auto* section = parent.emplace<GeneratedSectionView>(ui, id, label, expanded, changed);
            if (component.contains("citations")) add_generated_citations(ui, section->body(), component["citations"], resolver, id);
            compose(ui, section->body(), surface, component["components"], state, changed, submit, resolver);
        } else if (type == "paragraph") {
            auto* paragraph = parent.emplace<Box>(id);
            paragraph->emplace<Label>(ui, label, TextStyle::body_strong);
            paragraph->emplace<Label>(ui, to_utf16(jstr(component, "content")), TextStyle::body);
            if (component.contains("citations")) add_generated_citations(ui, *paragraph, component["citations"], resolver, id);
        } else if (is_generated_display_component(type)) {
            add_generated_display_component(ui, parent, component, resolver);
        } else if (type == "table") {
            auto& table = state->tables[id];
            if (!table) table = std::make_shared<GeneratedTableState>();
            parent.emplace<GeneratedTableView>(ui, component, table, changed, resolver);
        } else if (type == "bar" || type == "line") {
            parent.emplace<GeneratedChartView>(ui, component, resolver);
        } else if (type == "diagram") {
            parent.emplace<GeneratedDiagramView>(ui, component, resolver);
        } else if (type == "select" || type == "multiselect" || type == "switch" || type == "text" || type == "slider") {
            auto* field = parent.emplace<GeneratedFieldView>(ui, component, state->draft, changed);
            if (component.contains("citations")) add_generated_citations(ui, *field, component["citations"], resolver, id);
        } else if (type == "button") {
            const auto sections = [state] { return generated_surface_view_state_to_json(*state)["sections"]; };
            if (!component.contains("citations") || component["citations"].empty())
                parent.emplace<GeneratedActionView>(ui, component, surface, state->draft, submit, sections);
            else {
                auto* group = parent.emplace<Box>();
                group->emplace<GeneratedActionView>(ui, component, surface, state->draft, submit, sections);
                add_generated_citations(ui, *group, component["citations"], resolver, id);
            }
        }
    }
}
}  // namespace

bool generated_surface_presentation_supported(const json& surface, bool allow_interactions, bool resolve_citations) {
    const auto validated = parse_generated_surface(surface.dump());
    return validated && (resolve_citations || !has_citations(*validated)) && supports_components((*validated)["components"], allow_interactions);
}

GeneratedSurfaceView::GeneratedSurfaceView(Ui& ui, const json& surface, std::shared_ptr<GeneratedSurfaceViewState> state,
                                           std::function<void()> changed, GeneratedActionView::Submit submit,
                                           GeneratedCitationResolver resolver, bool accepted)
    : Box(jstr(surface, "id")) {
    role = Role::group;
    name = to_utf16(jstr(surface, "summary"));
    gap = 8;
    if (!generated_surface_presentation_supported(surface, static_cast<bool>(submit), static_cast<bool>(resolver))) return;
    if (!state) state = std::make_shared<GeneratedSurfaceViewState>();
    if (accepted) {
        emplace<Label>(ui, ui.strings.get("generated.action.accepted"), TextStyle::body_strong, Paint::text, true,
                       automation_id() + "-accepted");
        submit = {};
    }
    add_generated_template_header(ui, *this, surface);
    compose(ui, *this, surface, surface["components"], state, changed, submit, resolver);
}

}  // namespace wb::ui
