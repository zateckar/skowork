#include "ui/generated_citations.h"

#include "ui/text16.h"

namespace wb::ui {

void add_generated_citations(Ui& ui, Node& parent, const json& citations, const GeneratedCitationResolver& resolve,
                             const std::string& id_prefix) {
    if (!citations.is_array()) return;
    for (size_t i = 0; i < citations.size(); ++i) {
        if (!citations[i].is_string()) continue;
        const auto id = citations[i].get<std::string>();
        const auto citation = resolve ? resolve(id) : std::nullopt;
        if (citation && citation->open) {
            auto* link = parent.emplace<Button>(ui, id_prefix + "-citation-" + std::to_string(i),
                                                u"[" + to_utf16(id) + u"] " + citation->title, Button::Kind::subtle);
            link->role = Role::link;
            link->name = ui.strings.get("generated.citation.open", {{"number", to_utf16(id)}, {"title", citation->title}});
            link->on_click = citation->open;
        } else {
            parent.emplace<Label>(ui, ui.strings.get("generated.citation.unverified", {{"number", to_utf16(id)}}),
                                  TextStyle::caption, Paint::text_secondary, true, id_prefix + "-unverified-" + std::to_string(i));
        }
    }
}

}  // namespace wb::ui
