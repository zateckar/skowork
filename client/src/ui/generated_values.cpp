#include "ui/generated_values.h"

#include <cmath>
#include <set>

#include "ui/generated_surface.h"

namespace wb::ui {
namespace {

bool valid_value(const json& component, const std::string& type, const json& value) {
    if (type == "switch") return value.is_boolean();
    if (type == "text") return value.is_string() && value.get_ref<const std::string&>().size() <= 4096;
    if (type == "slider") {
        if (!value.is_number()) return false;
        const double number = value.get<double>();
        return std::isfinite(number) && number >= component["min"].get<double>() && number <= component["max"].get<double>();
    }
    std::set<std::string> options;
    for (const auto& option : component["options"]) options.insert(option.get<std::string>());
    if (type == "select") return value.is_null() || (value.is_string() && options.count(value.get<std::string>()) != 0);
    if (!value.is_array() || value.size() > options.size()) return false;
    std::set<std::string> selected;
    for (const auto& item : value) {
        if (!item.is_string()) return false;
        const auto s = item.get<std::string>();
        if (!options.count(s) || !selected.insert(s).second) return false;
    }
    return true;
}

bool collect(const json& components, const json& draft, std::set<std::string>& seen, std::vector<GeneratedFieldValue>& result) {
    for (const auto& component : components) {
        const auto type = jstr(component, "type");
        if (type == "section" || type == "card") {
            if (!collect(component["components"], draft, seen, result)) return false;
            continue;
        }
        if (type != "select" && type != "multiselect" && type != "switch" && type != "text" && type != "slider") continue;
        const auto id = jstr(component, "id");
        seen.insert(id);
        json value = type == "select" ? json(nullptr) : type == "multiselect" ? json::array() : component["value"];
        if (component.contains("value")) value = component["value"];
        if (draft.contains(id)) value = draft[id];
        if (!valid_value(component, type, value)) return false;
        result.push_back({id, jstr(component, "label"), std::move(value)});
    }
    return true;
}

bool check_sections(const json& components, const json& sections, const std::string& button_id,
                    std::set<std::string>& seen, bool& needed) {
    needed = false;
    for (const auto& component : components) {
        const auto type = jstr(component, "type"), id = jstr(component, "id");
        if (type == "section" || type == "card") {
            bool child_needed = false;
            if (!check_sections(component["components"], sections, button_id, seen, child_needed)) return false;
            seen.insert(id);
            if (sections.contains(id) && (!sections[id].is_boolean() || (child_needed && !sections[id].get<bool>()))) return false;
            needed = needed || child_needed;
        } else if (type == "select" || type == "multiselect" || type == "switch" || type == "text" || type == "slider" ||
                   (type == "button" && id == button_id)) {
            needed = true;
        }
    }
    return true;
}
}  // namespace

std::optional<std::vector<GeneratedFieldValue>> generated_surface_values(const json& surface, const json& draft) {
    if (!draft.is_object() || draft.size() > 40) return std::nullopt;
    const auto validated = parse_generated_surface(surface.dump());
    if (!validated) return std::nullopt;
    std::vector<GeneratedFieldValue> result;
    std::set<std::string> seen;
    if (!collect((*validated)["components"], draft, seen, result)) return std::nullopt;
    for (auto it = draft.begin(); it != draft.end(); ++it) if (!seen.count(it.key())) return std::nullopt;
    return result;
}

bool generated_surface_sections_allow_action(const json& surface, const json& sections, const std::string& button_id) {
    const auto validated = parse_generated_surface(surface.dump());
    if (!validated || !sections.is_object() || sections.size() > 100) return false;
    std::set<std::string> seen;
    bool needed = false;
    if (!check_sections((*validated)["components"], sections, button_id, seen, needed)) return false;
    for (auto it = sections.begin(); it != sections.end(); ++it) if (!seen.count(it.key())) return false;
    return true;
}

}  // namespace wb::ui
