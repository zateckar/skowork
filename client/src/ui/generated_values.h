#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/json.h"

namespace wb::ui {

struct GeneratedFieldValue {
    std::string id;
    std::string label;
    json value;
};

// Validates literal field values against a validated catalog. The host must additionally verify the bound surface,
// revision, visibility and target state before accepting a submission.
std::optional<std::vector<GeneratedFieldValue>> generated_surface_values(const json& surface, const json& draft);

// Requires every section enclosing a field or the submitted button to be expanded. Missing entries mean expanded.
bool generated_surface_sections_allow_action(const json& surface, const json& sections, const std::string& button_id);

}  // namespace wb::ui
