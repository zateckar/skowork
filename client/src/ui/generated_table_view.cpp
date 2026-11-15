#include "ui/generated_table_view.h"

#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::ui {
namespace {
std::u16string display_cell(const json& value) {
    if (value.is_string()) return to_utf16(value.get_ref<const std::string&>());
    return to_utf16(jstr(value, "text"));
}
}  // namespace

GeneratedTableView::GeneratedTableView(Ui& ui, json table, std::shared_ptr<GeneratedTableState> state,
                                       std::function<void()> changed, GeneratedCitationResolver resolver)
    : Box(jstr(table, "id"), Paint::surface, 6, Paint::border), ui_(ui), table_(std::move(table)),
      state_(state ? std::move(state) : std::make_shared<GeneratedTableState>()), changed_(std::move(changed)),
      resolver_(std::move(resolver)) {
    role = Role::group;
    name = to_utf16(jstr(table_, "label"));
    padding = Insets::all(8);
    gap = 6;
    emplace<Label>(ui_, name, TextStyle::body_strong);
    if (table_.contains("citations")) add_generated_citations(ui_, *this, table_["citations"], resolver_, automation_id());
    TextField::Options options;
    options.max_length = 1024;  // At most 4096 UTF-8 bytes when the user enters Unicode.
    auto* filter = emplace<TextField>(ui_, automation_id() + "-filter", ui_.strings.get("generated.table.filter"), options);
    filter->set_text(to_utf16(state_->filter));
    filter->on_change = [this, filter] {
        state_->filter = to_utf8(filter->text());
        refresh_rows();
        auto notify_change = changed_;
        if (notify_change) notify_change();
    };
    auto* header = emplace<Box>();
    header->axis = Axis::row;
    header->gap = 4;
    const auto& columns = table_["columns"];
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto label = to_utf16(columns[i].get_ref<const std::string&>());
        auto* button = header->emplace<Button>(ui_, automation_id() + "-sort-" + std::to_string(i), label, Button::Kind::subtle);
        button->width = Sizing::fill;
        headers_.push_back(button);
        button->on_click = [this, i] {
            if (state_->sort_column && *state_->sort_column == i) state_->descending = !state_->descending;
            else { state_->sort_column = i; state_->descending = false; }
            refresh_headers();
            refresh_rows();
            auto notify_change = changed_;
            if (notify_change) notify_change();
        };
    }
    rows_ = emplace<Box>();
    rows_->gap = 2;
    refresh_headers();
    refresh_rows();
}

void GeneratedTableView::refresh_headers() {
    const auto& columns = table_["columns"];
    for (size_t i = 0; i < headers_.size(); ++i) {
        const auto label = to_utf16(columns[i].get_ref<const std::string&>());
        const bool active = state_->sort_column && *state_->sort_column == i;
        headers_[i]->set_label(label + (active ? (state_->descending ? u" ↓" : u" ↑") : u""));
        const char* key = !active ? "generated.table.sort" :
            (state_->descending ? "generated.table.sort_descending" : "generated.table.sort_ascending");
        headers_[i]->name = ui_.strings.get(key, {{"column", label}});
    }
}

void GeneratedTableView::refresh_rows() {
    rows_->clear_children();
    const auto& rows = table_["rows"];
    for (size_t i : generated_table_rows(table_, *state_)) {
        auto* row = rows_->emplace<Box>();
        row->axis = Axis::row;
        row->gap = 4;
        size_t column = 0;
        for (const auto& cell : rows[i]) {
            const auto value = display_cell(cell);
            Node* container = row;
            const bool has_refs = cell.is_object() && cell.contains("citations") && !cell["citations"].empty();
            if (has_refs) container = row->emplace<Box>();
            auto* label = container->emplace<Label>(ui_, value, TextStyle::body);
            label->name = to_utf16(table_["columns"][column].get_ref<const std::string&>()) + u": " + value;
            if (has_refs) add_generated_citations(ui_, *container, cell["citations"], resolver_,
                                                  automation_id() + "-row-" + std::to_string(i) + "-cell-" + std::to_string(column));
            ++column;
        }
    }
    rows_->relayout();
}

}  // namespace wb::ui
