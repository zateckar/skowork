// The command palette (client/work-inbox Command Palette; add-assistant-feedback-patterns design D3): Ctrl+K opens a
// search field over the window with the commands and Inbox entries that match it. The focus stays in the field; Up and
// Down move the selection, Enter runs it, Escape or a click outside closes the palette and the focus returns.
#include <algorithm>

#include "shell/palette.h"
#include "shell/shell.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

constexpr float kPaletteWidth = 520;
constexpr size_t kMaxShown = 50;

struct PaletteItem {
    std::u16string name, shortcut;
    bool command = false;
    std::function<void()> run;
};

// One result: its name, and a command's shortcut at the end. The selected row has the selection colour.
class PaletteRow : public ui::Node {
public:
    PaletteRow(ui::Ui& ui, std::string id, const PaletteItem& item, bool selected, std::function<void()> choose)
        : Node(std::move(id)), ui_(ui), label_(item.name), shortcut_(item.shortcut), selected_(selected), choose_(std::move(choose)) {
        role = ui::Role::list_item;
        name = item.shortcut.empty() ? item.name : item.name + u", " + item.shortcut;
        padding = ui::Insets::xy(10, 6);
        measure = [this](float) {
            const ui::Size t = ui_.measure_text(label_, TextStyle::body, 100000.f, false);
            return ui::Size{t.w + 20, std::max(20.f, t.h)};
        };
    }
    bool can_invoke() const override { return true; }
    void invoke() override {
        const auto f = choose_;  // choosing closes the palette, this row with it
        if (f) f();
    }
    int selected_state() const override { return selected_ ? 1 : 0; }
    void draw(ui::Canvas& c) override {
        const ui::Rect& f = frame();
        if (selected_) c.round(f, 4, Paint::selection);
        else if (hovered_) c.round(f, 4, Paint::text, Paint::none, 0.06f);
        const Paint text = selected_ ? Paint::selection_text : Paint::text;
        float right = f.right() - padding.right;
        if (!shortcut_.empty()) {
            IDWriteTextLayout* k = ui_.layout(shortcut_, TextStyle::caption, 200.f, false);
            DWRITE_TEXT_METRICS m{};
            if (k) k->GetMetrics(&m);
            right -= m.widthIncludingTrailingWhitespace;
            c.text(k, right, f.y + (f.h - m.height) / 2, selected_ ? Paint::selection_text : Paint::text_secondary);
            right -= 12;
        }
        IDWriteTextLayout* l = ui_.layout(label_, TextStyle::body, std::max(10.f, right - f.x - padding.left), false);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        c.text(l, f.x + padding.left, f.y + (f.h - m.height) / 2, text);
    }

private:
    ui::Ui& ui_;
    std::u16string label_, shortcut_;
    bool selected_;
    std::function<void()> choose_;
};

class CommandPalette : public ui::Box {
public:
    CommandPalette(Shell& shell, std::vector<PaletteItem> items)
        : Box("palette", Paint::surface, 8, Paint::border), shell_(shell), items_(std::move(items)) {
        role = ui::Role::dialog;
        name = shell.t("palette.label");
        width = Sizing::fixed;
        fixed_w = kPaletteWidth;
        padding = ui::Insets::all(8);
        gap = 6;
        ui::TextField::Options o;
        o.placeholder = shell.t("palette.placeholder");
        field_ = emplace<ui::TextField>(shell.ui(), "palette-field", shell.t("palette.label"), o);
        field_->on_change = [this] {
            selected_ = 0;
            filter(true);
        };
        field_->on_key_down = [this](const ui::KeyEvent& e) { return key(e); };
        status_ = emplace<ui::Label>(shell.ui(), u"", TextStyle::caption, Paint::text_secondary, true, "palette-status");
        status_->padding = ui::Insets::xy(4, 0);
        scroll_ = emplace<ui::ScrollView>("palette-scroll");
        scroll_->scrolls = true;
        list_ = scroll_->emplace<Node>("palette-list");
        list_->role = ui::Role::list;
        list_->name = shell.t("palette.results");
        list_->gap = 1;
        entries_.reserve(items_.size());
        for (const PaletteItem& it : items_) entries_.push_back({it.name, it.command});
        filter(false);
    }

private:
    bool key(const ui::KeyEvent& e) {
        if (e.ctrl || e.alt) return false;
        if (e.key == ui::kKeyEscape) {
            close();
            return true;
        }
        if ((e.key == ui::kKeyDown || e.key == ui::kKeyUp) && !shown_.empty()) {
            selected_ = (selected_ + (e.key == ui::kKeyDown ? 1 : shown_.size() - 1)) % shown_.size();
            rebuild();
            shell_.announce(items_[shown_[selected_]].name);
            return true;
        }
        if (e.key == ui::kKeyEnter) {
            if (!shown_.empty()) choose(shown_[selected_]);
            return true;
        }
        return false;
    }

    void filter(bool announce) {
        shown_ = rank_palette(entries_, field_->text(), kMaxShown);
        selected_ = std::min(selected_, shown_.empty() ? 0 : shown_.size() - 1);
        const std::u16string status = shown_.empty() ? shell_.t("palette.none") : shell_.count("search.results", static_cast<long long>(shown_.size()));
        status_->set_text(status);
        rebuild();
        if (announce) shell_.announce(status);
    }

    void rebuild() {
        // Up to about eleven rows show at once; more scroll, and the selected row is kept in view.
        constexpr float kRowsHeight = 360;
        scroll_->height = shown_.size() > 11 ? Sizing::fixed : Sizing::fit;
        scroll_->fixed_h = kRowsHeight;
        list_->clear_children();
        for (size_t i = 0; i < shown_.size(); ++i) {
            const size_t index = shown_[i];
            list_->emplace<PaletteRow>(shell_.ui(), "palette-item-" + std::to_string(i), items_[index], i == selected_,
                                       [this, index] { choose(index); });
        }
        relayout();
        invalidate();
        if (ui::Tree* t = tree(); t && selected_ < list_->children().size()) {
            t->layout_if_needed();
            const ui::Rect row = list_->children()[selected_]->frame(), view = scroll_->frame();
            if (row.y < view.y) t->scroll_to(*scroll_, scroll_->scroll_y() - (view.y - row.y));
            else if (row.bottom() > view.bottom()) t->scroll_to(*scroll_, scroll_->scroll_y() + (row.bottom() - view.bottom()));
        }
    }

    void choose(size_t index) {
        auto run = items_[index].run;
        Shell* sp = &shell_;
        close();
        // After this palette is gone: the command may open a dialog or move the focus.
        sp->window().set_timer(1, [run] {
            if (run) run();
        });
    }

    void close() {
        if (auto* o = dynamic_cast<ui::OverlayLayer*>(parent())) o->close(this);  // destroys this palette
    }

    Shell& shell_;
    std::vector<PaletteItem> items_;
    std::vector<PaletteEntry> entries_;
    std::vector<size_t> shown_;
    size_t selected_ = 0;
    ui::TextField* field_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::ScrollView* scroll_ = nullptr;
    Node* list_ = nullptr;
};

}  // namespace

void Shell::open_palette() {
    if (window_->modal().top()) return;
    for (const auto& c : window_->overlay().children())
        if (c->automation_id() == "palette") return;  // already open
    std::vector<PaletteItem> items;
    auto command = [&](const char* key, const char* keys, std::function<void()> run) {
        items.push_back({t(key), keys ? ui::to_utf16(keys) : std::u16string(), true, std::move(run)});
    };
    command("shortcuts.commands.new_thread", "Ctrl+N", [this] { navigate("#/threads/new"); });
    command("shortcuts.commands.search", "Ctrl+F", [this] { open_search(); });
    command("shortcuts.commands.toggle_rail", "Ctrl+B", [this] { toggle_rail(); });
    command("shortcuts.commands.settings", "Ctrl+,", [this] { navigate("#/settings"); });
    command("shortcuts.commands.help", "Ctrl+/", [this] { show_shortcuts(); });
    for (const char* view : {"unread", "action", "running", "all", "archive"})
        command((std::string("views.") + view).c_str(), nullptr, [this, v = std::string(view)] { navigate(view_hash(v)); });
    if (const json* ag = jobj(status(), "agenda"); ag && jbool(*ag, "available") && jbool(*ag, "enabled"))
        command("views.agenda", nullptr, [this] { navigate("#/agenda"); });
    if (const json* cal = jobj(status(), "calendar"); cal && jbool(*cal, "available"))
        command("views.calendar", nullptr, [this] { navigate("#/calendar"); });
    for (const char* page : {"knowledge", "apps", "status", "about"})
        command((std::string("nav.") + page).c_str(), nullptr, [this, p = std::string(page)] { navigate("#/" + p); });
    for (const ThreadEntry& e : entries_) {
        if (e.title.empty()) continue;
        items.push_back({ui::to_utf16(e.title), {}, false, [this, id = e.id] { open_thread(id); }});
    }
    const ui::Rect win = window_->tree().root().frame();
    const float x = win.x + std::max(8.f, (win.w - kPaletteWidth) / 2);
    window_->overlay().show(std::make_unique<CommandPalette>(*this, std::move(items)), {x, win.y + 36, kPaletteWidth, 0}, true);
    window_->render();
}

}  // namespace wb::shell
