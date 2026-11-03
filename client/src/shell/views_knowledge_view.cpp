// The Knowledge view (client/knowledge-graph Knowledge View, Knowledge Notes, Knowledge Curation, Sharing Knowledge Items;
// add-knowledge-sharing design D4; improve-knowledge-view D1-D6): the search field and the Add menu (import documents
// through the system Open dialog, a folder through the folder picker, a new note), Source tabs with counts, a list pane
// with two-line rows or the search results, and a reader with the open item, its actions, a More menu and collapsed
// Details, the note editor, or Manage imports. Sharing asks first, showing the title, size and the Groups to choose from.
#include "base/strings.h"
#include "platform/win.h"
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

#include <algorithm>

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

const char* const kTabs[] = {"files", "notes", "threads", "mail", "calendar", "meetings"};

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

std::u16string day(const std::string& iso) { return u16(iso.size() >= 10 ? iso.substr(0, 10) : iso); }

bool blank(const std::u16string& s) { return s.find_first_not_of(u" \t\r\n") == std::u16string::npos; }

Node* row_box(Node& parent, const std::string& id = {}) {
    auto* r = id.empty() ? parent.emplace<Node>() : parent.emplace<Node>(id);
    r->axis = ui::Axis::row;
    r->gap = 8;
    return r;
}

std::u16string joined(const json& list) {
    std::u16string out;
    if (list.is_array())
        for (const auto& g : list)
            if (g.is_string()) out += (out.empty() ? u"" : u", ") + u16(g.get<std::string>());
    return out;
}

// The last part of a path, for Manage imports (the full path is the second line).
std::u16string leaf(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const auto cut = p.find_last_of("\\/");
    return u16(cut == std::string::npos ? p : p.substr(cut + 1));
}

// One row of the list (D4): the title, then a muted line with the date, the size and badges. Click or Enter opens it;
// the open item's row is selected.
class Row : public ui::Box {
public:
    Row(ui::Ui& ui, std::string id, std::u16string accessible, bool selected, std::function<void()> open)
        : Box(std::move(id)), ui_(ui), selected_(selected), open_(std::move(open)) {
        role = ui::Role::list_item;
        focusable = static_cast<bool>(open_);
        name = std::move(accessible);
        padding = ui::Insets::xy(12, 8);
        gap = 2;
    }
    bool can_invoke() const override { return static_cast<bool>(open_); }
    void invoke() override {
        // Opening rebuilds the list, which destroys this row: run a copy.
        if (auto open = open_) open();
    }
    int selected_state() const override { return open_ ? (selected_ ? 1 : 0) : -1; }
    void select() override { invoke(); }
    int cursor() const override { return open_ ? 1 : 0; }
    void draw(ui::Canvas& c) override {
        const ui::Rect f = frame();
        if (selected_) c.round(f.inset(1), 4, Paint::selection, Paint::none, ui_.theme.high_contrast ? 1.f : 0.6f);
        else if (pressed_ && open_) c.round(f.inset(1), 4, Paint::text, Paint::none, 0.08f);
        else if (hovered_ && open_) c.round(f.inset(1), 4, Paint::text, Paint::none, 0.04f);
        if (selected_ && !ui_.theme.high_contrast) c.round({f.x + 1, f.y + 10, 3, f.h - 20}, 1.5f, Paint::accent);
        if (!selected_) c.round({f.x + 8, f.bottom() - 1, f.w - 16, 1}, 0, Paint::border);
    }

private:
    ui::Ui& ui_;
    bool selected_;
    std::function<void()> open_;
};

}  // namespace

// The two panes (D1): the list at its width, the splitter and the reader; below 900 DIPs only one of them, the reader
// while something is open in it, with Back.
class KnowledgeView::Split : public Node {
public:
    explicit Split(KnowledgeView& view) : Node("knowledge-split"), view_(view) {
        axis = ui::Axis::row;
        height = Sizing::fill;
    }
    bool narrow() const { return narrow_; }
    ui::Splitter* splitter = nullptr;

    bool arrange_children(ui::Tree& tree, const ui::Rect& in) override {
        narrow_ = in.w < kSinglePaneBelow;
        const bool open = view_.reader_mode_ != Reader::empty;
        Node* list = view_.list_pane_;
        Node* reader = view_.reader_;
        list->visible = !narrow_ || !open;
        reader->visible = !narrow_ || open;
        splitter->visible = !narrow_;
        if (view_.back_) view_.back_->visible = narrow_;
        if (narrow_) {
            tree.place(list->visible ? *list : *reader, in);
            return true;
        }
        const float lw = std::clamp(list->fixed_w, kListMin, std::max(kListMin, std::min(kListMax, in.w - 320.f)));
        const float sw = splitter->fixed_w;
        tree.place(*list, {in.x, in.y, lw, in.h});
        tree.place(*splitter, {in.x + lw, in.y, sw, in.h});
        tree.place(*reader, {in.x + lw + sw, in.y, std::max(0.f, in.w - lw - sw), in.h});
        return true;
    }

private:
    KnowledgeView& view_;
    bool narrow_ = false;
};

KnowledgeView::KnowledgeView(Shell& shell) : Node("knowledge-view"), shell_(shell) {
    height = Sizing::fill;
    gap = 12;
    ui::Ui& u = shell_.ui();
    emplace<ui::Label>(u, shell_.t("knowledge.caption"), TextStyle::caption, Paint::text_secondary, true, "knowledge-body");

    // Header (D2): the search field and the Add menu.
    auto* bar = row_box(*this, "knowledge-bar");
    ui::TextField::Options o;
    o.placeholder = shell_.t("knowledge.search");
    search_ = bar->emplace<ui::TextField>(u, "knowledge-search", shell_.t("knowledge.search"), o);
    search_->width = Sizing::fill;
    search_->on_change = [this] { query_ = search_->text(); };
    search_->on_key_down = [this](const ui::KeyEvent& e) {
        if (e.key == ui::kKeyEnter) {
            search();
            return true;
        }
        if (e.key == ui::kKeyEscape && results_.is_object()) {
            clear_search();
            return true;
        }
        return false;
    };
    auto* go = bar->emplace<ui::IconButton>(u, "knowledge-search-go", ui::icon::search, shell_.t("knowledge.search_go"));
    go->on_click = [this] { search(); };
    add_ = bar->emplace<ui::Button>(u, "knowledge-add", shell_.t("knowledge.add"), ui::Button::Kind::primary, ui::icon::add);
    add_->width = Sizing::fit;
    add_->help = shell_.t("knowledge.add_menu");
    add_->on_click = [this] { show_add_menu(); };

    messages_ = emplace<Node>("knowledge-messages");
    messages_->gap = 4;

    // Source tabs with counts (D3).
    tabs_ = row_box(*this, "knowledge-tabs");
    tabs_->role = ui::Role::group;
    tabs_->name = shell_.t("knowledge.tabs");
    tabs_->gap = 4;

    // The list pane and the reader (D1).
    split_ = static_cast<Split*>(add(std::make_unique<Split>(*this)));
    list_pane_ = split_->emplace<Node>("knowledge-list-pane");
    list_pane_->role = ui::Role::pane;
    list_pane_->name = shell_.t("knowledge.list");
    list_pane_->width = Sizing::fixed;
    list_pane_->fixed_w = std::clamp(shell_.knowledge_list_w, kListMin, kListMax);
    list_pane_->height = Sizing::fill;
    list_pane_->gap = 6;
    list_head_ = list_pane_->emplace<Node>("knowledge-list-head");
    list_head_->gap = 6;
    list_ = list_pane_->emplace<ui::ScrollView>("knowledge-items");
    list_->scrolls = true;
    list_->height = Sizing::fill;
    list_->role = ui::Role::list;
    split_->splitter = split_->emplace<ui::Splitter>("knowledge-splitter", shell_.t("knowledge.list_width"), *list_pane_, kListMin, kListMax);
    split_->splitter->on_resized = [this](float w) { shell_.knowledge_list_w = w; };
    reader_ = split_->emplace<ui::ScrollView>("knowledge-reader");
    reader_->scrolls = true;
    reader_->height = Sizing::fill;
    reader_->role = ui::Role::pane;
    reader_->name = shell_.t("knowledge.reader");
    reader_->padding = ui::Insets::xy(20, 4);
    reader_->gap = 10;

    rebuild();
    changed();
    // Save as note from a Chat (add-save-as-note D3): the Chats view handed over a draft.
    if (shell_.knowledge_draft.is_object()) {
        const json d = std::move(shell_.knowledge_draft);
        shell_.knowledge_draft = nullptr;
        open_draft(d);
    }
}

void KnowledgeView::changed() {
    load_list();
    load_sharing();
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.save_as_note", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        const bool on = r && jbool(*r, "enabled");
        if (on == save_as_note_) return;
        save_as_note_ = on;
        rebuild();
    });
    if (reader_mode_ == Reader::item && item_.is_object()) open_item(jint(item_, "id"), selected_);
}

ui::Label* KnowledgeView::text(Node& parent, const std::u16string& s, Paint color, const std::string& id) {
    return parent.emplace<ui::Label>(shell_.ui(), s, TextStyle::body, color, true, id);
}

ui::Button* KnowledgeView::button(Node& parent, const std::string& id, const std::u16string& label, std::function<void()> f,
                                  ui::Button::Kind kind, char16_t glyph) {
    auto* b = parent.emplace<ui::Button>(shell_.ui(), id, label, kind, glyph);
    b->width = Sizing::fit;
    b->on_click = std::move(f);
    b->enabled = !busy_;
    return b;
}

std::u16string KnowledgeView::size_text(long long n) {
    if (n < 1024) return shell_.t("knowledge.bytes", {{"n", u16(std::to_string(n))}});
    if (n < 1024 * 1024) return shell_.t("knowledge.kb", {{"n", u16(std::to_string((n + 512) / 1024))}});
    const long long tenths = (n * 10 + 512 * 1024) / (1024 * 1024);
    std::u16string v = u16(std::to_string(tenths / 10)) + std::u16string(1, static_cast<char16_t>(shell_.decimal())) + u16(std::to_string(tenths % 10));
    return shell_.t("knowledge.mb", {{"n", v}});
}

// ---- data ----------------------------------------------------------------------------------------------------------

void KnowledgeView::act(const std::string& method, const json& params, std::function<void(const json&)> done) {
    if (busy_) return;
    busy_ = true;
    error_.clear();
    std::weak_ptr<int> alive = alive_;
    shell_.call(method, params, [this, alive, done = std::move(done)](Result<json> r) {
        if (!alive.lock()) return;
        busy_ = false;
        if (!r) {
            error_ = u16(r.error().message);
            shell_.announce(shell_.t("common.error_prefix") + u" " + error_);
        } else if (done) {
            done(*r);
            if (!alive.lock()) return;  // done may leave the view (Save as note opens the Knowledge view)
        }
        rebuild();
    });
    rebuild();
}

void KnowledgeView::load_list() {
    std::weak_ptr<int> alive = alive_;
    const std::string tab = tab_;
    shell_.call("knowledge.browse", {{"source", tab}, {"limit", 200}}, [this, alive, tab](Result<json> r) {
        if (!alive.lock() || tab != tab_) return;
        items_ = r ? (*r)["items"] : json::array();
        if (r && r->contains("counts")) counts_ = (*r)["counts"];
        if (!r) error_ = u16(r.error().message);
        rebuild();
    });
    if (tab == "files")
        shell_.call("knowledge.status", json::object(), [this, alive](Result<json> r) {
            if (!alive.lock()) return;
            folders_ = r ? json{{"folders", (*r)["folders"]}, {"documents", (*r)["documents"]}} : json(nullptr);
            rebuild();
        });
}

void KnowledgeView::load_sharing() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.sharing", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        sharing_ = r ? *r : json(nullptr);
        rebuild();
    });
}

void KnowledgeView::open_item(long long id, const std::string& key) {
    if (!id) return;
    if (reader_mode_ != Reader::item || jint(item_, "id") != id) {
        details_open_ = false;
        item_ = nullptr;
    }
    reader_mode_ = Reader::item;
    selected_ = key;
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.item", {{"id", id}}, [this, alive, id](Result<json> r) {
        if (!alive.lock() || reader_mode_ != Reader::item) return;
        if (r) {
            item_ = *r;
        } else if (jint(item_, "id") == id || item_.is_null()) {
            item_ = nullptr;
            error_ = u16(r.error().message);
            reader_mode_ = Reader::empty;
            selected_.clear();
        }
        rebuild();
    });
    rebuild();
}

void KnowledgeView::open_note(long long note_id) {
    note_id_ = note_id;
    note_origin_ = nullptr;
    reader_mode_ = Reader::editor;
    selected_ = note_id ? "n" + std::to_string(note_id) : std::string();
    note_title_.clear();
    note_text_.clear();
    if (!note_id) {
        rebuild();
        return;
    }
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.note.get", {{"id", note_id}}, [this, alive, note_id](Result<json> r) {
        if (!alive.lock() || note_id_ != note_id || reader_mode_ != Reader::editor) return;
        if (r) {
            note_title_ = u16(jstr(*r, "title"));
            note_text_ = u16(jstr(*r, "text"));
        } else {
            error_ = u16(r.error().message);
        }
        rebuild();
    });
    rebuild();
}

void KnowledgeView::open_draft(const json& draft) {
    note_id_ = 0;
    note_origin_ = draft.value("origin", json(nullptr));
    reader_mode_ = Reader::editor;
    selected_.clear();
    note_title_ = u16(jstr(draft, "title"));
    note_text_ = u16(jstr(draft, "text"));
    notice_.clear();
    rebuild();
    if (ui::Tree* t = Node::tree()) {
        std::function<Node*(Node&)> find = [&](Node& n) -> Node* {
            if (n.automation_id() == "knowledge-note-title") return &n;
            for (const auto& c : n.children())
                if (Node* f = find(*c)) return f;
            return nullptr;
        };
        if (Node* f = find(*reader_)) t->set_focus(f);
    }
    shell_.announce(shell_.t("knowledge.draft_hint"));
}

void KnowledgeView::save_mail_as_note(long long item_id) {
    act("knowledge.note.draft_from_mail", {{"item_id", item_id}}, [this](const json& d) { open_draft(d); });
}

void KnowledgeView::open_origin(const json& origin) {
    if (jstr(origin, "kind") == "chat") {
        shell_.navigate("#/chats/" + jstr(origin, "ref"));
        return;
    }
    const long long id = jint(origin, "item_id");
    if (!id) return;
    results_ = nullptr;
    shown_query_.clear();
    if (tab_ != "mail") {
        tab_ = "mail";
        items_ = nullptr;
        load_list();
    }
    open_item(id, std::to_string(id));
}

void KnowledgeView::close_reader() {
    reader_mode_ = Reader::empty;
    item_ = nullptr;
    rebuild();
    // Back on a narrow window: the keyboard returns to the row that was open.
    if (ui::Tree* t = Node::tree()) {
        std::function<Node*(Node&)> find = [&](Node& n) -> Node* {
            if (n.automation_id() == "knowledge-row-" + selected_) return &n;
            for (const auto& c : n.children())
                if (Node* f = find(*c)) return f;
            return nullptr;
        };
        if (Node* row = selected_.empty() ? nullptr : find(*list_)) t->set_focus(row);
        else t->set_focus(search_);
    }
}

void KnowledgeView::search() {
    if (blank(query_)) {
        clear_search();
        return;
    }
    const std::u16string q = query_;
    act("knowledge.search", {{"query", ui::to_utf8(q)}, {"limit", 20}}, [this, q](const json& r) {
        results_ = r;
        shown_query_ = q;
    });
}

void KnowledgeView::clear_search() {
    results_ = nullptr;
    shown_query_.clear();
    query_.clear();
    search_->set_text(u"");
    rebuild();
}

void KnowledgeView::show_add_menu() {
    auto m = std::make_unique<ui::Menu>("knowledge-add-menu", shell_.t("knowledge.add_menu"));
    m->add_item(shell_.ui(), "knowledge-import-files", shell_.t("knowledge.import_files"), [this] { import_files(); });
    m->add_item(shell_.ui(), "knowledge-import-folder", shell_.t("knowledge.import_folder"), [this] { import_folder(); }, ui::icon::folder);
    m->add_item(shell_.ui(), "knowledge-new-note", shell_.t("knowledge.new_note"), [this] { open_note(0); }, ui::icon::add);
    shell_.window().overlay().show(std::move(m), add_->frame(), true);
}

void KnowledgeView::show_more_menu() {
    if (!more_ || !item_.is_object()) return;
    const long long id = jint(item_, "id");
    const std::string source = jstr(item_, "source");
    const std::string ext = jstr(item_, "ext_id");
    const std::u16string title = u16(jstr(item_, "title"));
    auto m = std::make_unique<ui::Menu>("knowledge-more-menu", shell_.t("knowledge.more"));
    m->add_item(shell_.ui(), "knowledge-item-exclude", shell_.t("knowledge.exclude") + u"…", [this, source, ext, title] {
        ask_confirm("knowledge-exclude", shell_.t("knowledge.exclude_title"), shell_.t("knowledge.exclude_body", {{"name", title}}),
                    shell_.t("knowledge.exclude"), [this, source, ext] {
                        act("knowledge.exclusion.add", {{"kind", "item"}, {"value", source + ":" + ext}}, [this](const json&) {
                            reader_mode_ = Reader::empty;
                            item_ = nullptr;
                            selected_.clear();
                            load_list();
                        });
                    });
    });
    m->add_item(shell_.ui(), "knowledge-item-forget", shell_.t("knowledge.forget") + u"…", [this, id, title] {
        ask_confirm("knowledge-forget", shell_.t("knowledge.forget_title"), shell_.t("knowledge.forget_body", {{"name", title}}),
                    shell_.t("knowledge.forget"), [this, id] {
                        act("knowledge.item.forget", {{"item_id", id}}, [this](const json&) {
                            reader_mode_ = Reader::empty;
                            item_ = nullptr;
                            selected_.clear();
                            load_list();
                        });
                    });
    }, ui::icon::del);
    shell_.window().overlay().show(std::move(m), more_->frame(), true);
}

void KnowledgeView::import_files() {
    json paths = json::array();
    for (const auto& p : shell_.pick_files()) paths.push_back(narrow(p));
    if (paths.empty()) return;
    act("knowledge.document.add", {{"paths", paths}}, [this](const json& r) {
        notice_ = shell_.t("knowledge.imported");
        if (const json* f = jobj(r, "failed"); f && !f->empty()) error_ = u16(jstr((*f)[0], "reason"));
        shell_.announce(notice_);
        tab_ = "files";
        load_list();
    });
}

void KnowledgeView::import_folder() {
    const std::wstring folder = shell_.pick_folder();
    if (folder.empty()) return;
    act("knowledge.folder.add", {{"path", narrow(folder)}}, [this](const json&) {
        notice_ = shell_.t("knowledge.folder_added");
        shell_.announce(notice_);
        tab_ = "files";
        load_list();
    });
}

void KnowledgeView::save_note() {
    if (blank(note_title_) && blank(note_text_)) return;
    json params = {{"id", note_id_}, {"title", ui::to_utf8(note_title_)}, {"text", ui::to_utf8(note_text_)}};
    if (!note_id_ && note_origin_.is_object()) params["origin"] = note_origin_;
    act("knowledge.note.save", params, [this](const json& r) {
        note_id_ = jint(r, "id");
        note_origin_ = nullptr;
        reader_mode_ = Reader::empty;
        selected_ = "n" + std::to_string(note_id_);
        notice_ = shell_.t("knowledge.note_saved");
        shell_.announce(notice_);
        tab_ = "notes";
        load_list();
    });
}

// ---- dialogs -------------------------------------------------------------------------------------------------------

void KnowledgeView::ask_confirm(const std::string& id, const std::u16string& title, const std::u16string& body,
                                const std::u16string& action, std::function<void()> confirmed) {
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), id, title);
    d->body().emplace<ui::Label>(shell_.ui(), body);
    ui::Dialog* dp = d.get();
    d->add_button(id + "-cancel", shell_.t("knowledge.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    d->add_button(id + "-ok", action, ui::Button::Kind::primary, [dp, confirmed = std::move(confirmed)] {
        dp->close();
        confirmed();
    });
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

// Sharing asks first (Sharing Knowledge Items): the title, the size and a choice of the Groups policy permits.
void KnowledgeView::ask_share(const std::string& id, const std::u16string& title, const std::u16string& size,
                              std::function<void(const std::vector<std::string>&)> share, const std::u16string& copied) {
    std::vector<std::string> groups;
    if (sharing_.is_object())
        for (const auto& g : sharing_.value("permitted_groups", json::array()))
            if (g.is_string()) groups.push_back(g.get<std::string>());
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), id, shell_.t("knowledge.share_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("knowledge.share_body", {{"title", title}, {"size", size}}), TextStyle::body,
                                 Paint::text, true, id + "-body");
    if (!copied.empty()) d->body().emplace<ui::Label>(shell_.ui(), copied, TextStyle::body, Paint::warning, true, id + "-copied");
    auto checks = std::make_shared<std::vector<std::pair<std::string, ui::Checkbox*>>>();
    for (const auto& g : groups)
        checks->emplace_back(g, d->body().emplace<ui::Checkbox>(shell_.ui(), id + "-group-" + g, u16(g), groups.size() == 1));
    if (groups.empty())
        d->body().emplace<ui::Label>(shell_.ui(), shell_.t("knowledge.share_none"), TextStyle::body, Paint::text_secondary, true, id + "-none");
    ui::Dialog* dp = d.get();
    d->add_button(id + "-cancel", shell_.t("knowledge.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    d->add_button(id + "-confirm", shell_.t("knowledge.share_confirm"), ui::Button::Kind::primary, [dp, checks, share = std::move(share)] {
        std::vector<std::string> chosen;
        for (const auto& [g, c] : *checks)
            if (c->checked()) chosen.push_back(g);
        if (chosen.empty()) return;
        dp->close();
        share(chosen);
    });
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

// ---- the view ------------------------------------------------------------------------------------------------------

void KnowledgeView::rebuild() {
    // The keyboard stays on the element it was on when that element is built again (a row, a tab, an action).
    std::string focused;
    ui::Tree* tree = Node::tree();
    if (tree && tree->focused())
        for (const Node* n = tree->focused(); n; n = n->parent())
            if (n == this) {
                focused = tree->focused()->automation_id();
                break;
            }
    rebuild_messages();
    rebuild_tabs();
    rebuild_list();
    rebuild_reader();
    if (!tree) return;
    relayout();
    invalidate();
    if (!focused.empty() && tree->focused() != search_) {
        std::function<Node*(Node&)> find = [&](Node& n) -> Node* {
            if (n.automation_id() == focused && n.visible) return &n;
            for (const auto& c : n.children())
                if (Node* f = find(*c)) return f;
            return nullptr;
        };
        if (Node* n = find(*this)) tree->set_focus(n);
    }
}

void KnowledgeView::rebuild_messages() {
    messages_->clear_children();
    if (sharing_.is_object() && !jbool(sharing_, "enabled"))
        messages_->emplace<ui::Label>(shell_.ui(), shell_.t("knowledge.sharing_off"), TextStyle::caption, Paint::text_secondary, true,
                                      "knowledge-sharing-off");
    if (!notice_.empty()) messages_->emplace<ui::Label>(shell_.ui(), notice_, TextStyle::caption, Paint::text_secondary, true, "knowledge-notice");
    if (!error_.empty()) text(*messages_, shell_.t("common.error_prefix") + u" " + error_, Paint::error, "knowledge-error");
    messages_->visible = !messages_->children().empty();
}

void KnowledgeView::rebuild_tabs() {
    tabs_->clear_children();
    for (const char* t : kTabs) {
        const std::string tab = t;
        std::u16string label = shell_.t("knowledge.tab." + tab);
        if (counts_.is_object() && counts_.contains(tab))
            label = shell_.t("knowledge.tab_count", {{"name", label}, {"n", u16(std::to_string(jint(counts_, tab.c_str())))}});
        const bool on = tab == tab_ && !results_.is_object();
        auto* b = button(*tabs_, "knowledge-tab-" + tab, label, [this, tab] {
            if (tab_ == tab && !results_.is_object()) return;
            if (tab_ != tab) {
                tab_ = tab;
                items_ = nullptr;
                load_list();
            }
            results_ = nullptr;
            shown_query_.clear();
            rebuild();
        }, on ? ui::Button::Kind::standard : ui::Button::Kind::subtle);
        b->name = on ? shell_.t("knowledge.tab_selected", {{"name", label}}) : label;
        b->enabled = true;
    }
}

void KnowledgeView::rebuild_list() {
    list_head_->clear_children();
    list_->clear_children();
    ui::Ui& u = shell_.ui();

    // Search results (D2): title, Source and date, and a two-line snippet; each opens its item.
    if (results_.is_object()) {
        list_->name = shell_.t("knowledge.results");
        auto* head = row_box(*list_head_, "knowledge-results-head");
        auto* h = head->emplace<ui::Label>(u, shell_.t("knowledge.results_for", {{"query", shown_query_}}), TextStyle::body_strong,
                                           Paint::text, true, "knowledge-results-title");
        h->role = ui::Role::heading;
        h->padding.top = 6;
        button(*head, "knowledge-search-clear", shell_.t("knowledge.clear_search"), [this] { clear_search(); }, ui::Button::Kind::subtle)
            ->enabled = true;
        const json& passages = results_["passages"];
        if (passages.empty()) text(*list_, shell_.t("knowledge.no_results"), Paint::text_secondary, "knowledge-no-results");
        int n = 0;
        for (const auto& p : passages) {
            const std::string source = jstr(p, "source");
            const std::u16string title = u16(jstr(p, "title"));
            const std::u16string meta = source == "team"
                                            ? shell_.t("knowledge.team_meta", {{"owner", u16(jstr(p, "owner"))}, {"groups", joined(p.value("groups", json::array()))}})
                                            : shell_.t("knowledge.result_meta", {{"source", shell_.t("knowledge.tab." + source)}, {"date", day(jstr(p, "date"))}});
            const std::u16string label = source == "team"
                                             ? shell_.t("knowledge.team_result", {{"title", title}, {"owner", u16(jstr(p, "owner"))},
                                                                                  {"groups", joined(p.value("groups", json::array()))}})
                                             : shell_.t("knowledge.result", {{"title", title}, {"source", shell_.t("knowledge.tab." + source)},
                                                                             {"date", day(jstr(p, "date"))}});
            const long long id = jint(p, "item_id");
            const std::string key = std::to_string(id);
            std::function<void()> open;
            if (id) open = [this, id, key] { open_item(id, key); };
            auto* row = list_->add(std::make_unique<Row>(u, "knowledge-result-" + std::to_string(n), label,
                                                         id && reader_mode_ == Reader::item && selected_ == key, std::move(open)));
            row->emplace<ui::Label>(u, title, TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
            row->emplace<ui::Label>(u, meta, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
            std::string passage = jstr(p, "text");
            if (passage.size() > 220) passage = utf8_truncate(passage, 220) + "…";
            row->emplace<ui::Label>(u, u16(passage), TextStyle::caption, Paint::text_secondary, true)->role = ui::Role::none;
            ++n;
        }
        if (const std::string note = jstr(results_, "team_note"); !note.empty())
            text(*list_, shell_.t("knowledge.team_not_searched"), Paint::text_secondary, "knowledge-team-note");
        return;
    }

    // The rows of the Source tab (D4), with Manage imports above the documents (D6).
    list_->name = shell_.t("knowledge.items");
    if (tab_ == "files") {
        auto* m = button(*list_head_, "knowledge-manage-imports", shell_.t("knowledge.manage_imports"), [this] {
            reader_mode_ = Reader::imports;
            selected_.clear();
            rebuild();
        }, ui::Button::Kind::subtle);
        m->role = ui::Role::link;
        m->enabled = true;
    }
    list_head_->visible = !list_head_->children().empty();
    if (!items_.is_array()) {
        text(*list_, shell_.t("common.loading"), Paint::text_secondary);
        return;
    }
    if (items_.empty()) {
        text(*list_, shell_.t("knowledge.no_items"), Paint::text_secondary, "knowledge-no-items");
        return;
    }
    for (const auto& it : items_) {
        const long long id = jint(it, "id"), note_id = jint(it, "note_id");
        const std::string key = note_id ? "n" + std::to_string(note_id) : std::to_string(id);
        const std::u16string title = u16(jstr(it, "title"));
        const std::u16string meta = shell_.t("knowledge.row_meta", {{"date", day(jstr(it, "date"))}, {"size", size_text(jint(it, "size"))}});
        std::u16string accessible = shell_.t("knowledge.row", {{"title", title}, {"date", day(jstr(it, "date"))}, {"size", size_text(jint(it, "size"))}});
        if (jbool(it, "pending")) accessible += u" · " + shell_.t("knowledge.state_pending");
        if (jbool(it, "shared")) accessible += u" · " + shell_.t("knowledge.state_shared");
        auto* row = list_->add(std::make_unique<Row>(u, "knowledge-row-" + key, accessible, selected_ == key && reader_mode_ != Reader::empty,
                                                     [this, id, note_id, key] {
                                                         if (id) open_item(id, key);
                                                         else open_note(note_id);
                                                     }));
        row->emplace<ui::Label>(u, title, TextStyle::body, Paint::text, false)->role = ui::Role::none;
        auto* line = row_box(*row);
        line->gap = 6;
        line->emplace<ui::Label>(u, meta, TextStyle::caption, Paint::text_secondary, false)->role = ui::Role::none;
        if (jbool(it, "shared")) {
            auto* b = line->emplace<ui::Badge>(u, shell_.t("knowledge.badge_shared"), Paint::surface_alt, Paint::text_secondary);
            b->role = ui::Role::none;
            if (const json* s = jobj(it, "sharing")) b->help = shell_.t("knowledge.shared_with", {{"groups", joined(s->value("groups", json::array()))}});
        }
        if (jbool(it, "pending"))
            line->emplace<ui::Badge>(u, shell_.t("knowledge.badge_pending"), Paint::surface_alt, Paint::text_secondary)->role = ui::Role::none;
    }
}

void KnowledgeView::rebuild_reader() {
    reader_->clear_children();
    more_ = nullptr;
    ui::Ui& u = shell_.ui();
    auto* back = reader_->emplace<ui::Button>(u, "knowledge-back", shell_.t("knowledge.back"), ui::Button::Kind::subtle, ui::icon::back);
    back->width = Sizing::fit;
    back->on_click = [this] { close_reader(); };
    back->visible = split_->narrow();
    back_ = back;
    switch (reader_mode_) {
    case Reader::empty:
        reader_->emplace<ui::Label>(u, shell_.t("knowledge.reader_empty"), TextStyle::body, Paint::text_secondary, true, "knowledge-reader-empty");
        break;
    case Reader::editor: build_editor(*reader_); break;
    case Reader::imports: build_imports(*reader_); break;
    case Reader::item:
        if (item_.is_object()) build_item(*reader_);
        else text(*reader_, shell_.t("common.loading"), Paint::text_secondary);
        break;
    }
}

void KnowledgeView::build_editor(Node& parent) {
    auto* box = parent.emplace<Node>("knowledge-note-editor");
    box->role = ui::Role::group;
    box->name = shell_.t(note_id_ ? "knowledge.edit_note" : "knowledge.new_note");
    box->gap = 8;
    box->emplace<ui::Label>(shell_.ui(), box->name, TextStyle::subtitle)->role = ui::Role::heading;
    if (note_origin_.is_object()) {
        box->emplace<ui::Label>(shell_.ui(),
                                shell_.t(jstr(note_origin_, "kind") == "chat" ? "knowledge.origin_chat" : "knowledge.origin_mail",
                                         {{"title", u16(jstr(note_origin_, "title"))}}),
                                TextStyle::caption, Paint::text_secondary, true, "knowledge-note-origin");
        if (!note_id_)  // a draft: nothing is stored yet
            box->emplace<ui::Label>(shell_.ui(), shell_.t("knowledge.draft_hint"), TextStyle::caption, Paint::text_secondary, true,
                                    "knowledge-note-draft-hint");
    }
    auto* title = box->emplace<ui::TextField>(shell_.ui(), "knowledge-note-title", shell_.t("knowledge.note_title"));
    title->set_text(note_title_);
    ui::TextField* tp = title;
    title->on_change = [this, tp] { note_title_ = tp->text(); };
    ui::TextField::Options o;
    o.multiline = true;
    o.min_lines = 10;
    o.max_lines = 30;
    auto* body = box->emplace<ui::TextField>(shell_.ui(), "knowledge-note-text", shell_.t("knowledge.note_text"), o);
    body->set_text(note_text_);
    ui::TextField* bp = body;
    body->on_change = [this, bp] { note_text_ = bp->text(); };
    auto* actions = row_box(*box);
    button(*actions, "knowledge-note-save", shell_.t("knowledge.save"), [this] { save_note(); }, ui::Button::Kind::primary);
    button(*actions, "knowledge-note-cancel", shell_.t("knowledge.cancel"), [this] {
        reader_mode_ = Reader::empty;
        rebuild();
    });
    if (note_id_) {
        const long long id = note_id_;
        button(*actions, "knowledge-note-delete", shell_.t("knowledge.delete"), [this, id] {
            ask_confirm("knowledge-delete-note", shell_.t("knowledge.delete_note_title"),
                        shell_.t("knowledge.delete_note_body", {{"name", note_title_.empty() ? shell_.t("knowledge.tab.notes") : note_title_}}),
                        shell_.t("knowledge.delete"), [this, id] {
                            act("knowledge.note.delete", {{"id", id}}, [this](const json&) {
                                reader_mode_ = Reader::empty;
                                note_id_ = 0;
                                item_ = nullptr;
                                selected_.clear();
                                load_list();
                            });
                        });
        }, ui::Button::Kind::subtle);
    }
}

// Manage imports (D6): the imported folders, each with sharing and Remove, and the documents imported one by one.
void KnowledgeView::build_imports(Node& parent) {
    ui::Ui& u = shell_.ui();
    parent.emplace<ui::Label>(u, shell_.t("knowledge.folders"), TextStyle::subtitle, Paint::text, true, "knowledge-imports-title")->role =
        ui::Role::heading;
    auto* fl = parent.emplace<Node>("knowledge-folders");
    fl->role = ui::Role::list;
    fl->name = shell_.t("knowledge.folders");
    fl->gap = 2;
    if (!folders_.is_object()) {
        text(*fl, shell_.t("common.loading"), Paint::text_secondary);
        return;
    }
    const bool can_share = sharing_.is_object() && jbool(sharing_, "enabled");
    std::map<std::string, json> rules;
    if (sharing_.is_object())
        for (const auto& r : sharing_.value("rules", json::array())) rules[jstr(r, "folder")] = r.value("groups", json::array());
    const auto entry = [&](const std::string& id, const std::string& path, const std::u16string& state) {
        auto* line = row_box(*fl, id);
        line->role = ui::Role::list_item;
        line->padding = ui::Insets::xy(0, 6);
        auto* names = line->emplace<Node>();
        names->gap = 2;
        names->emplace<ui::Label>(u, leaf(path), TextStyle::body, Paint::text, false)->role = ui::Role::none;
        names->emplace<ui::Label>(u, u16(path) + (state.empty() ? u"" : u" · " + state), TextStyle::caption, Paint::text_secondary, true)->role =
            ui::Role::none;
        line->name = leaf(path) + u", " + u16(path) + (state.empty() ? u"" : u", " + state);
        return line;
    };
    const json folders = folders_.value("folders", json::array()), documents = folders_.value("documents", json::array());
    if (folders.empty() && documents.empty()) text(*fl, shell_.t("knowledge.imports_empty"), Paint::text_secondary, "knowledge-imports-empty");
    for (const auto& f : folders) {
        if (!f.is_string()) continue;
        const std::string path = f.get<std::string>();
        const auto rule = rules.find(path);
        auto* line = entry("knowledge-folder-row-" + path, path,
                           rule != rules.end() ? shell_.t("knowledge.folder_shared", {{"groups", joined(rule->second)}}) : std::u16string());
        if (rule != rules.end()) {
            auto* b = button(*line, "knowledge-folder-unshare-" + path, shell_.t("knowledge.unshare_folder"), [this, path] {
                act("knowledge.unshare_folder", {{"path", path}}, [this](const json&) {
                    shell_.announce(shell_.t("knowledge.unshared"));
                    load_list();
                    load_sharing();
                });
            }, ui::Button::Kind::subtle);
            b->name = shell_.t("knowledge.unshare_folder") + u": " + u16(path);
        } else if (can_share) {
            auto* b = button(*line, "knowledge-folder-share-" + path, shell_.t("knowledge.share_folder"), [this, path] {
                ask_share("knowledge-share", u16(path), shell_.t("knowledge.folder_size"), [this, path](const std::vector<std::string>& g) {
                    act("knowledge.share_folder", {{"path", path}, {"groups", g}}, [this](const json&) {
                        shell_.announce(shell_.t("knowledge.shared"));
                        load_list();
                        load_sharing();
                    });
                });
            }, ui::Button::Kind::subtle);
            b->name = shell_.t("knowledge.share_folder") + u" " + u16(path);
        }
        auto* r = button(*line, "knowledge-folder-remove-" + path, shell_.t("knowledge.remove"), [this, path] {
            ask_confirm("knowledge-remove-folder", shell_.t("knowledge.remove_folder_title"),
                        shell_.t("knowledge.remove_folder_body", {{"name", u16(path)}}), shell_.t("knowledge.remove"), [this, path] {
                            act("knowledge.folder.remove", {{"path", path}}, [this](const json&) { load_list(); });
                        });
        }, ui::Button::Kind::subtle);
        r->name = shell_.t("knowledge.remove_named", {{"name", u16(path)}});
    }
    for (const auto& f : documents) {
        if (!f.is_string()) continue;
        const std::string path = f.get<std::string>();
        auto* line = entry("knowledge-document-row-" + path, path, {});
        auto* b = button(*line, "knowledge-document-remove-" + path, shell_.t("knowledge.remove"), [this, path] {
            act("knowledge.document.remove", {{"path", path}}, [this](const json&) { load_list(); });
        }, ui::Button::Kind::subtle);
        b->name = shell_.t("knowledge.remove_named", {{"name", u16(path)}});
    }
}

// The reader (D5): title, meta line, the actions with More, the sharing state, the indexed text and collapsed Details.
void KnowledgeView::build_item(Node& parent) {
    ui::Ui& u = shell_.ui();
    const long long id = jint(item_, "id");
    const std::string source = jstr(item_, "source");
    const std::u16string title = u16(jstr(item_, "title"));
    auto* box = parent.emplace<Node>("knowledge-item");
    box->role = ui::Role::group;
    box->name = title;
    box->gap = 8;
    box->emplace<ui::Label>(u, title, TextStyle::subtitle, Paint::text, true, "knowledge-item-title")->role = ui::Role::heading;
    const std::u16string size = size_text(jint(item_, "size"));
    box->emplace<ui::Label>(u, shell_.t("knowledge.item_meta", {{"source", shell_.t("knowledge.tab." + source)}, {"date", day(jstr(item_, "date"))}, {"size", size}}),
                            TextStyle::caption, Paint::text_secondary, true, "knowledge-item-meta");
    if (jbool(item_, "pending"))
        box->emplace<ui::Label>(u, shell_.t("knowledge.pending"), TextStyle::caption, Paint::text_secondary, true, "knowledge-item-pending");

    const json* share = jobj(item_, "sharing");
    const std::string state = share ? jstr(*share, "state") : std::string();
    const bool shared = state == "shared" || state == "error" || state == "unsharing";
    // A note saved from mail or a Chat (add-save-as-note D4): where it came from, as a link while that still exists.
    const json origin = item_.value("origin", json(nullptr));
    std::u16string copied;
    if (origin.is_object()) {
        const bool chat = jstr(origin, "kind") == "chat";
        copied = shell_.t(chat ? "knowledge.share_copied_chat" : "knowledge.share_copied_mail");
        const std::u16string from = shell_.t(chat ? "knowledge.origin_chat" : "knowledge.origin_mail", {{"title", u16(jstr(origin, "title"))}});
        if (chat ? jbool(origin, "available") : jint(origin, "item_id") != 0) {
            auto* link = button(*box, "knowledge-item-origin", from, [this, origin] { open_origin(origin); }, ui::Button::Kind::subtle);
            link->role = ui::Role::link;
            link->enabled = true;
        } else {
            box->emplace<ui::Label>(u, from, TextStyle::caption, Paint::text_secondary, true, "knowledge-item-origin");
        }
    }
    auto* actions = row_box(*box, "knowledge-item-actions");
    actions->gap = 6;
    if (jbool(item_, "shareable") && sharing_.is_object() && jbool(sharing_, "enabled") && !shared)
        button(*actions, "knowledge-share", shell_.t("knowledge.share"), [this, id, title, size, copied] {
            ask_share("knowledge-share", title, size, [this, id](const std::vector<std::string>& g) {
                act("knowledge.share", {{"item_id", id}, {"groups", g}}, [this, id](const json&) {
                    shell_.announce(shell_.t("knowledge.shared"));
                    open_item(id, selected_);
                    load_list();
                });
            }, copied);
        });
    // Save as note for a mail item (add-save-as-note D3).
    if (source == "mail" && save_as_note_)
        button(*actions, "knowledge-item-save-note", shell_.t("knowledge.save_as_note"), [this, id] { save_mail_as_note(id); });
    if (shared && state != "unsharing")
        button(*actions, "knowledge-unshare", shell_.t("knowledge.unshare"), [this, id] {
            act("knowledge.unshare", {{"item_id", id}}, [this, id](const json&) {
                shell_.announce(shell_.t("knowledge.unshared"));
                open_item(id, selected_);
                load_list();
            });
        });
    if (item_.contains("note_id")) {
        const long long note = jint(item_, "note_id");
        button(*actions, "knowledge-item-edit-note", shell_.t("knowledge.edit_note"), [this, note] { open_note(note); });
    }
    more_ = actions->emplace<ui::IconButton>(u, "knowledge-item-more", ui::icon::more, shell_.t("knowledge.more"));
    more_->on_click = [this] { show_more_menu(); };
    more_->enabled = !busy_;

    // Sharing in one line.
    std::u16string line;
    Paint ink = Paint::text_secondary;
    if (!jbool(item_, "shareable")) {
        line = shell_.t("knowledge.not_shareable");
    } else if (!share || state == "withdrawn") {
        line = shell_.t("knowledge.private");
    } else if (state == "removed") {
        line = shell_.t("knowledge.share_removed", {{"reason", u16(jstr(*share, "error"))}});
        ink = Paint::warning;
    } else if (state == "unsharing") {
        line = shell_.t("knowledge.share_unsharing");
    } else {
        line = shell_.t("knowledge.shared_with", {{"groups", joined(share->value("groups", json::array()))}});
        ink = Paint::text;
    }
    box->emplace<ui::Label>(u, line, TextStyle::caption, ink, true, "knowledge-item-sharing");
    if (state == "error")
        box->emplace<ui::Label>(u, shell_.t("knowledge.share_error", {{"reason", u16(jstr(*share, "error"))}}), TextStyle::caption, Paint::warning,
                                true, "knowledge-item-share-error");

    // The indexed text.
    std::string body = jstr(item_, "text");
    if (body.size() > 8000) body = utf8_truncate(body, 8000) + "…";
    auto* t = box->emplace<ui::Box>("knowledge-item-body", Paint::none, 0, Paint::none);
    t->padding = ui::Insets::xy(0, 8);
    t->emplace<ui::Label>(u, u16(body), TextStyle::body, Paint::text, true, "knowledge-item-text");

    build_details(*box, id);
}

// Details (D5), collapsed by default: the entities with rename, and the facts with edit and delete.
void KnowledgeView::build_details(Node& parent, long long id) {
    ui::Ui& u = shell_.ui();
    const json ents = item_.value("entities", json::array());
    const json fl = item_.value("facts", json::array());
    auto* card = parent.add(std::make_unique<ui::Card>(u, "knowledge-item-details",
                                                       shell_.t("knowledge.details", {{"entities", shell_.count("knowledge.entity_count", static_cast<long long>(ents.size()))},
                                                                                      {"facts", shell_.count("knowledge.fact_count", static_cast<long long>(fl.size()))}}),
                                                       true, details_open_));
    auto* c = static_cast<ui::Card*>(card);
    c->on_expand = [this](bool on) { details_open_ = on; };
    Node& b = c->body();

    b.emplace<ui::Label>(u, shell_.t("knowledge.entities"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* el = b.emplace<Node>("knowledge-item-entities");
    el->role = ui::Role::list;
    el->name = shell_.t("knowledge.entities");
    el->gap = 2;
    if (ents.empty()) text(*el, shell_.t("knowledge.no_entities"), Paint::text_secondary, "knowledge-no-entities");
    for (const auto& e : ents) {
        const long long eid = jint(e, "id");
        const std::string sid = std::to_string(eid);
        const std::u16string ename = u16(jstr(e, "name"));
        const std::u16string what = ename + u" (" + u16(jstr(e, "type")) + u")";
        auto* line = row_box(*el, "knowledge-entity-" + sid);
        line->role = ui::Role::list_item;
        line->name = what;
        auto* l = line->emplace<ui::Label>(u, what, TextStyle::body, Paint::text, true);
        l->role = ui::Role::none;
        l->padding.top = 6;
        auto* rb = button(*line, "knowledge-entity-rename-" + sid, shell_.t("knowledge.rename"), [this, eid, ename, id] {
            shell_.ask_text_value("knowledge-rename", shell_.t("knowledge.rename_title"), shell_.t("knowledge.name"), ename,
                                  [this, eid, id](const std::string& v) {
                                      act("knowledge.entity.rename", {{"id", eid}, {"name", v}}, [this, id](const json&) { open_item(id, selected_); });
                                  });
        }, ui::Button::Kind::subtle);
        rb->name = shell_.t("knowledge.rename_named", {{"name", ename}});
    }

    b.emplace<ui::Label>(u, shell_.t("knowledge.facts"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* facts = b.emplace<Node>("knowledge-item-facts");
    facts->role = ui::Role::list;
    facts->name = shell_.t("knowledge.facts");
    facts->gap = 2;
    if (fl.empty()) text(*facts, shell_.t("knowledge.no_facts"), Paint::text_secondary, "knowledge-no-facts");
    for (const auto& f : fl) {
        const std::string kind = jstr(f, "kind");
        const long long fid = jint(f, "id");
        const std::string key = kind + "-" + std::to_string(fid);
        const std::u16string ftext = u16(jstr(f, "text"));
        const std::u16string what = kind == "user" ? shell_.t("knowledge.fact_user", {{"text", ftext}}) : ftext;
        auto* line = row_box(*facts, "knowledge-fact-" + key);
        line->role = ui::Role::list_item;
        line->name = what;
        auto* l = line->emplace<ui::Label>(u, what, TextStyle::body, Paint::text, true);
        l->role = ui::Role::none;
        l->padding.top = 6;
        auto* e = button(*line, "knowledge-fact-edit-" + key, shell_.t("knowledge.edit"), [this, kind, fid, ftext, id] {
            shell_.ask_text_value("knowledge-fact", shell_.t("knowledge.edit_fact_title"), shell_.t("knowledge.fact_label"), ftext,
                                  [this, kind, fid, id](const std::string& v) {
                                      act("knowledge.fact.edit", {{"kind", kind}, {"id", fid}, {"text", v}}, [this, id](const json&) { open_item(id, selected_); });
                                  });
        }, ui::Button::Kind::subtle);
        e->name = shell_.t("knowledge.edit_named", {{"name", ftext}});
        auto* d = button(*line, "knowledge-fact-delete-" + key, shell_.t("knowledge.delete"), [this, kind, fid, id] {
            act("knowledge.fact.delete", {{"kind", kind}, {"id", fid}}, [this, id](const json&) { open_item(id, selected_); });
        }, ui::Button::Kind::subtle);
        d->name = shell_.t("knowledge.delete_named", {{"name", ftext}});
    }
}

}  // namespace wb::shell
