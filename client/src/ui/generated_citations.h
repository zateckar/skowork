#pragma once

#include <functional>
#include <optional>

#include "base/json.h"
#include "ui/elements.h"

namespace wb::ui {

struct GeneratedCitation {
    std::u16string title;
    std::function<void()> open;
};
using GeneratedCitationResolver = std::function<std::optional<GeneratedCitation>(const std::string&)>;

// The host resolves existing Citation IDs; missing IDs are rendered as unverified text without a link.
void add_generated_citations(Ui& ui, Node& parent, const json& citations, const GeneratedCitationResolver& resolve,
                             const std::string& id_prefix);

}  // namespace wb::ui
