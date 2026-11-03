#include "ui/list.h"

#include <algorithm>

namespace wb::ui {

ListRow::ListRow(ListView& list, size_t index) : Node("row-" + std::to_string(index)), list_(list), index_(index) {
    role = Role::list_item;
    axis = Axis::row;
}

int ListRow::selected_state() const { return list_.selected() == index_ ? 1 : 0; }

void ListRow::select() { list_.set_selected(index_); }

ListView::ListView(std::string automation_id, RowFactory factory, float estimated_row_height)
    : Node(std::move(automation_id)), factory_(std::move(factory)), estimate_(estimated_row_height) {
    role = Role::list;
    focusable = true;
    scrolls = true;
    clips = true;
    height = Sizing::fill;
}

void ListView::set_count(size_t count) {
    clear_children();
    rows_.clear();
    heights_.reset(count, estimate_);
    selected_.reset();
    set_scroll_y(0);
    relayout();
    invalidate();
}

void ListView::item_changed(size_t index) {
    const auto it = rows_.find(index);
    if (it == rows_.end()) return;
    ListRow* r = it->second;
    rows_.erase(it);
    remove(r);
}

void ListView::items_changed() {
    clear_children();
    rows_.clear();
    relayout();
}

ListRow* ListView::row(size_t index) const {
    const auto it = rows_.find(index);
    return it == rows_.end() ? nullptr : it->second;
}

void ListView::set_selected(std::optional<size_t> index) {
    if (index && *index >= count()) index.reset();
    if (index == selected_) {
        if (index) scroll_to_index(*index);
        return;
    }
    Tree* t = tree();
    auto redraw = [&](size_t i) {
        if (ListRow* r = row(i); r && t) t->invalidate(r->frame().intersect(frame()).inset(-4.f));  // with the focus ring
    };
    if (selected_) redraw(*selected_);
    selected_ = index;
    if (!index) return;
    scroll_to_index(*index);
    redraw(*index);
    if (on_select) on_select(*index);
}

void ListView::scroll_to_index(size_t index) {
    if (index >= count()) return;
    const float top = heights_.offset(index), bottom = top + heights_.height(index);
    float y = scroll_y();
    if (top < y) y = top;
    else if (bottom > y + viewport_h_) y = bottom - viewport_h_;
    if (Tree* t = tree()) t->scroll_to(*this, y);
    else set_scroll_y(y);
}

bool ListView::arrange_children(Tree& tree, const Rect& in) {
    viewport_h_ = in.h;
    float y = std::clamp(scroll_y(), 0.f, std::max(0.f, heights_.total() - in.h));
    VirtualList::Range r = heights_.visible(y, in.h, 2);
    // Rows that left the view go away (focus, hover and capture inside them are forgotten).
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (it->first < r.first || it->first >= r.last) {
            ListRow* gone = it->second;
            it = rows_.erase(it);
            remove(gone);
        } else {
            ++it;
        }
    }
    // Realise and measure the rows in view; measured heights replace the estimate.
    for (size_t i = r.first; i < r.last; ++i) {
        ListRow*& slot = rows_[i];
        if (!slot) {
            auto row = std::make_unique<ListRow>(*this, i);
            factory_(i, *row);
            size_t pos = 0;
            for (const auto& [idx, p] : rows_)
                if (idx < i && p) ++pos;
            slot = static_cast<ListRow*>(insert(pos, std::move(row)));
        }
        heights_.set_height(i, tree.measure(*slot, in.w).h);
    }
    set_content_height(heights_.total());
    y = std::clamp(y, 0.f, max_scroll());
    set_scroll_y(y);
    for (const auto& [i, row] : rows_)
        tree.place(*row, {in.x, in.y + heights_.offset(i) - y, in.w, heights_.height(i)});
    return true;
}

Rect ListView::focus_rect() const {
    if (selected_)
        if (const ListRow* r = row(*selected_)) return r->frame().intersect(frame());
    return frame();
}

bool ListView::on_key(const KeyEvent& e) {
    if (count() == 0 || e.alt) return false;
    const size_t last = count() - 1;
    const size_t cur = selected_.value_or(0);
    const size_t page = std::max<size_t>(1, static_cast<size_t>(viewport_h_ / std::max(1.f, estimate_)) - 1);
    switch (e.key) {
    case kKeyDown: set_selected(selected_ ? std::min(last, cur + 1) : 0); return true;
    case kKeyUp: set_selected(selected_ ? (cur > 0 ? cur - 1 : 0) : 0); return true;
    case kKeyHome: set_selected(0); return true;
    case kKeyEnd: set_selected(last); return true;
    case kKeyPageDown: set_selected(std::min(last, cur + page)); return true;
    case kKeyPageUp: set_selected(cur > page ? cur - page : 0); return true;
    case kKeyEnter:
        if (selected_ && on_activate) {
            on_activate(*selected_);
            return true;
        }
        return false;
    default: return false;
    }
}

}  // namespace wb::ui
