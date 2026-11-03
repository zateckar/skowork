// The virtual list element (add-native-ui design D2): the Thread list with 10,000 Threads and similar lists. Only the
// rows in view (plus a few above and below) exist as nodes; the others are heights in a VirtualList. The list
// takes keyboard focus and moves its selection with the arrow, Page, Home and End keys; each row is a list item for
// UI Automation (D8). Portable: no Win32.
#pragma once

#include <functional>
#include <map>
#include <optional>

#include "ui/layout.h"

namespace wb::ui {

class ListView;

class ListRow : public Node {
public:
    ListRow(ListView& list, size_t index);
    size_t index() const { return index_; }
    int selected_state() const override;
    void select() override;

private:
    ListView& list_;
    size_t index_;
};

class ListView : public Node {
public:
    // Fills a new row for an index: sets its name and adds its content. Rows are rebuilt when they scroll back into
    // view or after item_changed().
    using RowFactory = std::function<void(size_t index, ListRow& row)>;

    ListView(std::string automation_id, RowFactory factory, float estimated_row_height = 40.f);

    void set_count(size_t count);  // drops all rows and selection
    size_t count() const { return heights_.size(); }
    void item_changed(size_t index);  // the row is rebuilt at the next layout
    void items_changed();             // every row is rebuilt at the next layout

    std::optional<size_t> selected() const { return selected_; }
    // Selects a row and scrolls it into view; on_select is called when the selection changed.
    void set_selected(std::optional<size_t> index);
    std::function<void(size_t)> on_select;
    std::function<void(size_t)> on_activate;  // Enter on the selected row

    ListRow* row(size_t index) const;  // the realised row, if in view
    const VirtualList& heights() const { return heights_; }
    void scroll_to_index(size_t index);

    bool arrange_children(Tree& tree, const Rect& inner) override;
    bool on_key(const KeyEvent& e) override;
    Rect focus_rect() const override;

private:
    RowFactory factory_;
    VirtualList heights_;
    float estimate_;
    std::map<size_t, ListRow*> rows_;
    std::optional<size_t> selected_;
    float viewport_h_ = 0;
};

}  // namespace wb::ui
