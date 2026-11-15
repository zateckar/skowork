#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/json.h"

namespace wb::ui {

// Pure local exploration state. It does not send a message or invoke any host action.
struct GeneratedTableState {
    std::optional<size_t> sort_column;
    bool descending = false;
    std::string filter;
};

GeneratedTableState generated_table_state_from_json(const json& value, size_t column_count);
json generated_table_state_to_json(const GeneratedTableState& state);
std::vector<size_t> generated_table_rows(const json& table, const GeneratedTableState& state);

}  // namespace wb::ui
