#include "ui/generated_table.h"

#include <algorithm>
#include <numeric>
#include <string_view>

namespace wb::ui {
namespace {

std::string_view cell_text(const json& cell) {
    if (cell.is_string()) return cell.get_ref<const std::string&>();
    if (cell.is_object()) {
        const auto it = cell.find("text");
        if (it != cell.end() && it->is_string()) return it->get_ref<const std::string&>();
    }
    return {};
}

}  // namespace

GeneratedTableState generated_table_state_from_json(const json& value, size_t column_count) {
    GeneratedTableState result;
    if (!value.is_object()) return result;
    if (const auto it = value.find("sort_column"); it != value.end()) {
        if (const auto n = json_int(*it); n && *n >= 0 && static_cast<unsigned long long>(*n) < column_count)
            result.sort_column = static_cast<size_t>(*n);
    }
    result.descending = jbool(value, "descending");
    result.filter = jstr(value, "filter");
    if (result.filter.size() > 4096) result.filter.clear();
    return result;
}

json generated_table_state_to_json(const GeneratedTableState& state) {
    json out = {{"descending", state.descending}, {"filter", state.filter}};
    if (state.sort_column) out["sort_column"] = *state.sort_column;
    return out;
}

std::vector<size_t> generated_table_rows(const json& table, const GeneratedTableState& state) {
    std::vector<size_t> order;
    if (!table.is_object() || !table.contains("columns") || !table["columns"].is_array() || !table.contains("rows") ||
        !table["rows"].is_array() || table["rows"].size() > 500) return order;
    const auto& rows = table["rows"];
    const size_t columns = table["columns"].size();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].is_array() || rows[i].size() != columns) return {};
        bool match = state.filter.empty();
        for (const auto& cell : rows[i]) if (cell_text(cell).find(state.filter) != std::string_view::npos) match = true;
        if (match) order.push_back(i);
    }
    if (state.sort_column && *state.sort_column < columns) {
        const size_t col = *state.sort_column;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const auto left = cell_text(rows[a][col]), right = cell_text(rows[b][col]);
            return state.descending ? left > right : left < right;
        });
    }
    return order;
}

}  // namespace wb::ui
