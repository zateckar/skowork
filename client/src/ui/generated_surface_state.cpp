#include "ui/generated_surface_state.h"

#include "ui/generated_surface.h"
#include "ui/generated_values.h"

namespace wb::ui {
namespace {
void restore_components(const json& components, const json& saved, GeneratedSurfaceViewState& result) {
    for (const auto& component : components) {
        const auto type = jstr(component, "type"), id = jstr(component, "id");
        if (type == "section" || type == "card") {
            const auto* sections = jobj(saved, "sections");
            if (sections && sections->is_object() && sections->contains(id) && (*sections)[id].is_boolean())
                result.sections[id] = std::make_shared<bool>((*sections)[id].get<bool>());
            restore_components(component["components"], saved, result);
        } else if (type == "table") {
            const auto* tables = jobj(saved, "tables");
            if (tables && tables->is_object() && tables->contains(id))
                result.tables[id] = std::make_shared<GeneratedTableState>(generated_table_state_from_json((*tables)[id], component["columns"].size()));
        }
    }
}
}  // namespace

json generated_surface_view_state_to_json(const GeneratedSurfaceViewState& state) {
    json sections = json::object(), tables = json::object();
    for (const auto& [id, expanded] : state.sections) if (expanded) sections[id] = *expanded;
    for (const auto& [id, table] : state.tables) if (table) tables[id] = generated_table_state_to_json(*table);
    return {{"sections", std::move(sections)}, {"tables", std::move(tables)},
            {"draft", state.draft && state.draft->is_object() ? *state.draft : json::object()}};
}

GeneratedSurfaceViewState generated_surface_view_state_from_json(const json& surface, const json& saved) {
    GeneratedSurfaceViewState result;
    if (!saved.is_object()) return result;
    const auto valid = parse_generated_surface(surface.dump());
    if (!valid) return result;
    restore_components((*valid)["components"], saved, result);
    const auto* draft = jobj(saved, "draft");
    if (draft && draft->is_object() && generated_surface_values(*valid, *draft)) *result.draft = *draft;
    return result;
}

}  // namespace wb::ui
