// The Thread list (client/work-inbox Smart Views, Folders, Pinned Threads And Date Grouping, Triage Actions), with the
// Chats as entries among the Threads (Chat Entries; add-chats-in-inbox design D3).
#include <algorithm>

#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

std::u16string view_name(Shell& s, const ViewId& view) {
    if (view.starts_with("folder:")) {
        for (const Folder& f : s.folders())
            if ("folder:" + f.id == view) return ui::to_utf16(f.name);
        return s.t("nav.folders");
    }
    return s.t("views." + view);
}

// The content of a row: draws a group header or a Thread entry and handles the pointer for the row.
class RowContent : public Node {
public:
    RowContent(Shell& s, ui::ListView& list, size_t index, const ListRow& row)
        : Node(row.kind == ListRow::header ? "group-" + std::string(group_key(row.group)) : "thread-" + row.item->id),
          shell_(s), list_(list), index_(index), row_(row) {
        width = Sizing::fill;
        height = Sizing::fill;
        if (row.item) {
            entry_ = *row.item;
            // The preview is one line: line breaks and runs of spaces become single spaces.
            std::string one;
            for (char ch : entry_.preview) {
                const bool space = ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ';
                if (space && (one.empty() || one.back() == ' ')) continue;
                one += space ? ' ' : ch;
            }
            entry_.preview = one;
            when_ = format_relative(entry_.last_activity, s.now(), s.ui().strings, [](long long ms) {
                const std::string iso = iso_from_millis(ms);
                return ui::to_utf16(iso.substr(0, 10));
            });
        }
    }
    int cursor() const override { return 1; }
    bool on_pointer(const ui::PointerEvent& e) override {
        if (e.kind == ui::PointerEvent::down) {
            list_.set_selected(index_);
            if (e.button == 1 && row_.item) shell_.show_thread_menu(entry_.id, frame());
            return true;
        }
        if (e.kind == ui::PointerEvent::up && e.button == 0 && frame().contains(e.x, e.y)) {
            if (row_.kind == ListRow::header) toggle_group();
            else shell_.open_thread(entry_.id);
            return true;
        }
        return false;
    }
    void toggle_group() {
        Shell& s = shell_;  // refreshing the list rebuilds its rows, this one included
        auto& c = s.collapsed();
        if (c.contains(row_.group)) c.erase(row_.group);
        else c.insert(row_.group);
        s.refresh_list();
        s.window().render();
    }
    void draw(ui::Canvas& c) override {
        ui::Ui& u = shell_.ui();
        const ui::Rect f = frame();
        const bool selected = list_.selected() == index_;
        // The selected row: the selection colour with the accent bar, as in the navigation (improve-desktop-look).
        if (selected) c.round(f.inset(2), 4, Paint::selection);
        else if (hovered_ || pressed_) c.round(f.inset(2), 4, Paint::text, Paint::none, 0.05f);
        if (selected && !u.theme.high_contrast && row_.kind != ListRow::header) c.round({f.x + 2, f.y + 12, 3, f.h - 24}, 1.5f, Paint::accent);
        else if (!selected && row_.kind != ListRow::header) c.line(f.x + 12, f.bottom() - 0.5f, f.right() - 12, f.bottom() - 0.5f, Paint::border);
        const Paint ink = selected && u.theme.high_contrast ? Paint::selection_text : Paint::text;
        const Paint dim = selected && u.theme.high_contrast ? Paint::selection_text : Paint::text_secondary;
        if (row_.kind == ListRow::header) {
            const char16_t chevron = row_.collapsed ? ui::icon::chevron_right : ui::icon::chevron_down;
            c.text(u.layout(std::u16string(1, chevron), TextStyle::icon, 100.f, false), f.x + 12, f.y + 9, dim);
            const std::u16string label = shell_.t(std::string("list.groups.") + group_key(row_.group)) + u"  " +
                                         ui::to_utf16(std::to_string(row_.count));
            IDWriteTextLayout* l = u.layout(label, TextStyle::caption, std::max(1.f, f.w - 44), false);
            DWRITE_TEXT_METRICS m{};
            if (l) l->GetMetrics(&m);
            c.text(l, f.x + 36, f.y + (f.h - m.height) / 2, dim);
            return;
        }
        const float x0 = f.x + 24, right = f.right() - 12;
        if (entry_.unread) c.round({f.x + 9, f.y + 17, 8, 8}, 4, Paint::accent);
        // First line: title and time.
        IDWriteTextLayout* when = u.layout(when_, TextStyle::caption, 200.f, false);
        DWRITE_TEXT_METRICS wm{};
        if (when) when->GetMetrics(&wm);
        c.text(when, right - wm.width, f.y + 11, dim);
        const std::u16string title = entry_.title.empty() ? shell_.t("list.untitled") : ui::to_utf16(entry_.title);
        IDWriteTextLayout* tl = u.layout(title, entry_.unread ? TextStyle::body_strong : TextStyle::body,
                                         std::max(1.f, right - wm.width - 8 - x0), false);
        c.text(tl, x0, f.y + 8, ink);
        // Second line: marks and preview.
        float x = x0;
        const float y2 = f.y + 36;
        auto pill = [&](const std::u16string& text, Paint fill, Paint color) {
            IDWriteTextLayout* pl = u.layout(text, TextStyle::caption, 200.f, false);
            DWRITE_TEXT_METRICS pm{};
            if (pl) pl->GetMetrics(&pm);
            c.round({x, y2, pm.width + 12, pm.height + 2}, (pm.height + 2) / 2, fill, u.theme.high_contrast ? Paint::text : Paint::none);
            c.text(pl, x + 6, y2 + 1, color);
            x += pm.width + 12 + 6;
        };
        if (entry_.pinned) {
            c.text(u.layout(std::u16string(1, ui::icon::pin), TextStyle::icon, 100.f, false), x, y2 + 1, dim);
            x += 22;
        }
        if (entry_.is_chat()) {
            // A Chat: its badge, the other user's Presence in a direct Chat, and the unread messages.
            pill(shell_.t("list.chat"), Paint::accent_surface, ink);
            if (entry_.chat_kind == "direct") {
                const Paint dot = entry_.presence == "online" ? Paint::success
                                  : entry_.presence == "busy" ? Paint::error
                                  : entry_.presence == "away" ? Paint::warning
                                                              : Paint::text_secondary;
                c.round({x, y2 + 5, 8, 8}, 4, dot);
                x += 14;
            }
            if (entry_.unread_count > 0) pill(ui::to_utf16(std::to_string(entry_.unread_count)), Paint::accent, Paint::accent_text);
        } else if (entry_.origin_kind != "user") {
            pill(shell_.t(origin_badge_key(entry_)), Paint::accent_surface, ink);
        }
        if (entry_.has_action) pill(shell_.t("list.needs_action"), Paint::warning_background, Paint::text);
        if (entry_.running) pill(shell_.t("list.running"), Paint::info_background, Paint::text);
        else if (entry_.last_run_state == "failed") pill(shell_.t("list.failed"), Paint::error_background, Paint::text);
        if (!entry_.preview.empty() && right > x + 8)
            c.text(u.layout(ui::to_utf16(entry_.preview), TextStyle::caption, right - x, false), x, y2 + 1, dim);
    }

private:
    Shell& shell_;
    ui::ListView& list_;
    size_t index_;
    ListRow row_;
    ThreadEntry entry_;
    std::u16string when_;
};

}  // namespace

ThreadListPane::ThreadListPane(Shell& shell) : Box("thread-list-pane", Paint::background), shell_(shell) {
    width = Sizing::fixed;
    fixed_w = kListDefault;
    height = Sizing::fill;
    padding = {0, 8, 0, 0};
    auto* head = emplace<Node>("thread-list-head");
    head->axis = ui::Axis::row;
    head->padding.right = 8;
    title_ = head->emplace<ui::Label>(shell.ui(), u"", TextStyle::subtitle, Paint::text, true, "thread-list-title");
    title_->role = ui::Role::heading;
    title_->padding = ui::Insets::xy(16, 6);
    folder_button_ = head->emplace<ui::IconButton>(shell.ui(), "thread-list-folder-menu", ui::icon::more, shell.t("folder.options"));
    folder_button_->visible = false;
    folder_button_->on_click = [this] {
        const ViewId& v = shell_.view();
        if (v.starts_with("folder:")) shell_.show_folder_menu(v.substr(7), folder_button_->frame());
    };
    search_button_ = head->emplace<ui::IconButton>(shell.ui(), "thread-list-search", ui::icon::search, shell.t("search.open"));
    search_button_->on_click = [this] {
        if (search_open()) shell_.close_search();
        else shell_.open_search();
    };
    head->emplace<ui::CaptionSpace>();  // a single pane on a narrow window reaches the window buttons
    search_ = emplace<SearchView>(shell);
    search_->visible = false;
    empty_ =emplace<ui::Label>(shell.ui(), u"", TextStyle::body, Paint::text_secondary, true, "thread-list-empty");
    empty_->padding = ui::Insets::xy(16, 12);
    list_ = emplace<ui::ListView>("thread-list", [this](size_t i, ui::ListRow& row) { build_row(i, row); },
                                  kEntryRowHeight);
    list_->on_activate = [this](size_t i) {
        if (i >= rows_.size()) return;
        if (rows_[i].kind == ListRow::header) {
            if (ui::ListRow* r = list_->row(i); r && !r->children().empty())
                static_cast<RowContent*>(r->children().front().get())->toggle_group();
        } else {
            shell_.open_thread(rows_[i].item->id);
        }
    };
}

void ThreadListPane::rebuild() {
    const std::u16string vn = view_name(shell_, shell_.view());
    title_->set_text(vn);
    folder_button_->visible = shell_.view().starts_with("folder:");
    list_->name = shell_.t("list.label", {{"view", vn}});
    const std::string sel = selected_thread();
    std::optional<Group> sel_group;  // a selected group header stays selected when it expands or collapses
    if (const auto s = list_->selected(); s && *s < rows_.size() && rows_[*s].kind == ListRow::header)
        sel_group = rows_[*s].group;
    const float scroll = list_->scroll_y();
    entries_ = shell_.entries();  // the rows point into this copy, which lives as long as they do
    rows_ = list_rows(entries_, shell_.now(), shell_.collapsed());
    list_->set_count(rows_.size());
    if (ui::Tree* tr = Node::tree()) tr->scroll_to(*list_, scroll);
    if (!sel.empty()) select_thread(sel);
    if (sel_group)
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].kind == ListRow::header && rows_[i].group == *sel_group) list_->set_selected(i);
    const bool empty = shell_.entries().empty() && !shell_.list_loading();
    empty_->visible = empty && !search_open();
    if (empty) {
        const ViewId& v = shell_.view();
        empty_->set_text(shell_.t(v.starts_with("folder:") ? std::string("list.empty.folder") : "list.empty." + v));
    }
    relayout();
    invalidate();
}

void ThreadListPane::build_row(size_t i, ui::ListRow& row) {
    if (i >= rows_.size()) return;
    const ListRow& r = rows_[i];
    row.height = Sizing::fixed;
    row.fixed_h = r.kind == ListRow::header ? kHeaderRowHeight : kEntryRowHeight;
    if (r.kind == ListRow::header) {
        row.name = shell_.t(std::string("list.groups.") + group_key(r.group)) + u", " +
                   shell_.count("list.group_count", static_cast<long long>(r.count));
    } else {
        const ThreadEntry& e = *r.item;
        std::u16string n = e.title.empty() ? shell_.t("list.untitled") : ui::to_utf16(e.title);
        if (e.pinned) n += u", " + shell_.t("list.pinned");
        if (e.is_chat()) {
            n += u", " + shell_.t("list.chat");
            if (e.chat_kind == "direct") n += u", " + shell_.t("chats.presence." + (e.presence.empty() ? std::string("offline") : e.presence));
            if (e.unread_count > 0) n += u", " + shell_.count("chats.unread", e.unread_count);
        } else {
            if (e.origin_kind != "user") n += u", " + shell_.t(origin_badge_key(e));
            if (e.unread) n += u", " + shell_.t("list.unread");
            if (e.has_action) n += u", " + shell_.t("list.needs_action");
            if (e.running) n += u", " + shell_.t("list.running");
            else if (e.last_run_state == "failed") n += u", " + shell_.t("list.failed");
        }
        row.name = n;
        row.help = ui::to_utf16(e.preview);
    }
    row.emplace<RowContent>(shell_, *list_, i, r);
    if (i + 10 >= rows_.size() && shell_.list_has_more()) shell_.load_more();
}

bool ThreadListPane::search_open() const { return search_->visible; }

void ThreadListPane::set_search(bool open) {
    if (open == search_open()) return;
    if (open) search_->reset();
    search_->visible = open;
    list_->visible = !open;
    empty_->visible = !open && shell_.entries().empty() && !shell_.list_loading();
    search_button_->name = shell_.t(open ? "search.close" : "search.open");
    search_button_->help = search_button_->name;
    relayout();
    invalidate();
}

void ThreadListPane::select_thread(const std::string& id) {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].kind == ListRow::entry && rows_[i].item->id == id) {
            list_->set_selected(i);
            return;
        }
}

bool ThreadListPane::on_key(const ui::KeyEvent& e) {
    const auto s = list_->selected();
    if (!s || *s >= rows_.size() || e.ctrl || e.alt) return false;
    const ListRow& r = rows_[*s];
    if (r.kind == ListRow::header && (e.key == ui::kKeyLeft || e.key == ui::kKeyRight)) {
        if ((e.key == ui::kKeyLeft) != r.collapsed)
            if (ui::ListRow* row = list_->row(*s); row && !row->children().empty())
                static_cast<RowContent*>(row->children().front().get())->toggle_group();
        return true;
    }
    if (r.kind == ListRow::entry && (e.key == ui::kKeyApps || (e.key == ui::kKeyF10 && e.shift))) {
        const ui::ListRow* row = list_->row(*s);
        shell_.show_thread_menu(r.item->id, row ? row->frame() : list_->frame());
        return true;
    }
    return false;
}

std::string ThreadListPane::selected_thread() const {
    const auto s = list_->selected();
    if (!s || *s >= rows_.size() || rows_[*s].kind != ListRow::entry) return {};
    return rows_[*s].item->id;
}

}  // namespace wb::shell
