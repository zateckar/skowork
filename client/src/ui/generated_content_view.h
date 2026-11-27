// The display components of generated surfaces and the Content template header (add-content-templates D2, D3):
// markdown, facts, callout, timeline, tasks and draft. None of them is a control: links open in the window, and the
// draft's Copy puts its body on the local clipboard and never sends anything.
#pragma once

#include "base/json.h"
#include "ui/elements.h"
#include "ui/generated_citations.h"

namespace wb::ui {

// True for the six display component types.
bool is_generated_display_component(const std::string& type);

// Adds a validated display component to parent.
void add_generated_display_component(Ui& ui, Node& parent, const json& component, const GeneratedCitationResolver& resolver);

// The header of a surface that names a Content template and/or a title: the template's icon and localized name, then
// the title. Adds nothing when the surface has neither.
void add_generated_template_header(Ui& ui, Node& parent, const json& surface);

}  // namespace wb::ui
