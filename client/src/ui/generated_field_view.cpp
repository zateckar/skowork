#include "ui/generated_field_view.h"

#include <algorithm>

#include "ui/generated_slider.h"
#include "ui/generated_values.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::ui {

GeneratedFieldView::GeneratedFieldView(Ui& ui, const json& component, std::shared_ptr<json> draft,
                                       std::function<void()> changed)
    : Box(jstr(component, "id")) {
    role = Role::group;
    name = to_utf16(jstr(component, "label"));
    gap = 4;
    if (!draft) draft = std::make_shared<json>(json::object());
    if (!draft->is_object()) *draft = json::object();
    const auto id = jstr(component, "id"), type = jstr(component, "type");
    const json surface = {{"version", 1}, {"id", "field"}, {"summary", "Field"}, {"components", json::array({component})}};
    json input = json::object();
    if (draft->contains(id)) input[id] = (*draft)[id];
    auto values = generated_surface_values(surface, input);
    if (!values) {
        draft->erase(id);
        values = generated_surface_values(surface, json::object());
    }
    if (!values || values->size() != 1) return;
    const json value = values->front().value;
    if (type == "switch") {
        auto* toggle = emplace<Checkbox>(ui, id + "-input", name, value.get<bool>());
        toggle->on_change = [draft, id, changed](bool on) {
            (*draft)[id] = on;
            if (changed) changed();
        };
    } else if (type == "text") {
        emplace<Label>(ui, name, TextStyle::body_strong);
        TextField::Options options;
        options.max_length = 1024;
        auto* text = emplace<TextField>(ui, id + "-input", name, options);
        text->set_text(to_utf16(value.get<std::string>()));
        text->on_change = [draft, id, text, changed] {
            (*draft)[id] = to_utf8(text->text());
            if (changed) changed();
        };
    } else if (type == "slider") {
        auto* slider = emplace<GeneratedSlider>(ui, id + "-input", name, component["min"].get<double>(),
                                                component["max"].get<double>(), value.get<double>());
        slider->on_change = [draft, id, changed](double number) {
            (*draft)[id] = number;
            if (changed) changed();
        };
    } else if (type == "select" || type == "multiselect") {
        emplace<Label>(ui, name, TextStyle::body_strong);
        const auto& options = component["options"];
        for (size_t i = 0; i < options.size(); ++i) {
            const auto option = options[i].get<std::string>();
            const bool selected = type == "select" ? value == option :
                std::find(value.begin(), value.end(), json(option)) != value.end();
            auto* checkbox = emplace<Checkbox>(ui, id + "-option-" + std::to_string(i), to_utf16(option), selected);
            choices_.push_back(checkbox);
            checkbox->on_change = [this, draft, id, type, option, options, i, changed](bool on) {
                if (updating_) return;
                updating_ = true;
                if (type == "select") {
                    if (on) for (size_t j = 0; j < choices_.size(); ++j) if (j != i) choices_[j]->set_checked(false);
                    (*draft)[id] = on ? json(option) : json(nullptr);
                } else {
                    json selected_options = json::array();
                    for (size_t j = 0; j < choices_.size(); ++j) if (choices_[j]->checked()) selected_options.push_back(options[j]);
                    (*draft)[id] = std::move(selected_options);
                }
                updating_ = false;
                auto notify_change = changed;
                if (notify_change) notify_change();
            };
        }
    }
}

}  // namespace wb::ui
