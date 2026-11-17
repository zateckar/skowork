#pragma once

#include <optional>
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
struct GeneratedSurfaceBlock {
    size_t begin = 0, end = 0;  // byte offsets in the original message, including the fences
    json surface;
};
// Discovery alone grants no authority: callers must also establish the durable assistant Thread context.
std::vector<GeneratedSurfaceBlock> discover_generated_surfaces(std::string_view markdown);
}  // namespace wb::ui
