#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "base/json.h"

namespace wb::ui {
// Untrusted model output. A successful result is data only: it carries no authority to execute actions.
std::optional<json> parse_generated_surface(std::string_view input);
// Added to the model prompt only when a compatible Thread client has enabled the feature.
std::string_view generated_surface_instructions();
std::string_view generated_surface_example();
std::string_view generated_question_form_example();  // the ask_user form example quoted in the instructions
// Content templates (add-content-templates D3): daily_report, meeting_summary, briefing, action_prep, meeting_prep. The
// instructions go into a producer's preamble whether or not the Generated surfaces setting is on; empty for an unknown kind.
std::vector<std::string_view> content_template_kinds();
std::string content_template_instructions(std::string_view kind);
std::string_view content_template_example(std::string_view kind);
// True when a surface holds a control (select, multiselect, switch, text, slider, button). A surface without controls is
// display-only and renders whatever the setting (add-content-templates D4).
bool generated_surface_has_controls(const json& surface);
struct GeneratedSurfaceBlock {
    size_t begin = 0, end = 0;  // byte offsets in the original message, including the fences
    json surface;
};
// Discovery alone grants no authority: callers must also establish the durable assistant Thread context.
std::vector<GeneratedSurfaceBlock> discover_generated_surfaces(std::string_view markdown);
// The message as text for previews, notifications and excerpts (client/work-inbox Surface Text In Previews): a valid
// surface block becomes its summary, any other skowork-ui fence is left out.
std::string generated_surface_text(std::string_view markdown);
// The same for the search index: a valid block becomes every text it shows (summary, title, labels, values, content).
std::string generated_surface_search_text(std::string_view markdown);
}  // namespace wb::ui
