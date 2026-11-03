// The Chats in the reading pane (client/user-messaging Chats View, Own Presence, Offline Sending; add-user-messaging
// design D4, add-chats-in-inbox D3, D4). The Chats are Inbox entries in the Thread list. A Chat entry opens here: its
// messages rendered as Markdown with sender and time, queued ones marked pending; the composer; rename, add members and
// leave (after asking); Save as note with a period (add-save-as-note D3), which opens the draft in the Knowledge view's
// editor. New Chat and Add members pick people by searching (improve-desktop-look D7, fold-chats-into-inbox D2). The
// host keeps the cache and talks to the control plane (messaging.*).
#include "base/strings.h"
#include "base/time.h"
#include "shell/shell.h"
#include "ui/richtext.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

bool blank(const std::u16string& s) { return s.find_first_not_of(u" \t\r\n") == std::u16string::npos; }

Node* row_box(Node& parent, const std::string& id = {}) {
    auto* r = id.empty() ? parent.emplace<Node>() : parent.emplace<Node>(id);
    r->axis = ui::Axis::row;
    r->gap = 8;
    return r;
}

Node* find_id(Node& n, const std::string& id) {
    if (n.automation_id() == id) return &n;
    for (const auto& c : n.children())
        if (Node* x = find_id(*c, id)) return x;
    return nullptr;
}

// The people picker (client/user-messaging Chats View, improve-desktop-look D7): a search field that asks the directory
// as the user types (two characters or more; one request at a time, and the latest text wins), the matches with their
// Presence and a checkbox each, and the picked people as chips with a remove button. Members of the Chat are marked and
// cannot be picked.
class PeoplePicker : public Node {
public:
    PeoplePicker(Shell& s, std::string id, std::set<std::string> members, std::function<std::u16string(const std::string&)> presence)
        : Node(id), shell_(s), id_(std::move(id)), members_(std::move(members)), presence_(std::move(presence)) {
        gap = 8;
        field_ = emplace<ui::TextField>(s.ui(), id_ + "-search", s.t("chats.picker_search"));
        field_->set_placeholder(s.t("chats.picker_placeholder"));
        field_->on_change = [this] { query(); };
        chips_ = emplace<Node>(id_ + "-chips");
        chips_->axis = ui::Axis::row;
        chips_->wraps = true;
        chips_->gap = 6;
        chips_->role = ui::Role::list;
        chips_->name = s.t("chats.picker_picked");
        results_ = emplace<Node>(id_ + "-results");
        results_->role = ui::Role::list;
        results_->name = s.t("chats.directory_results");
        results_->gap = 2;
        build_chips();
        build_results();
    }
    std::vector<std::string> picked() const {
        std::vector<std::string> out;
        for (const auto& [user, shown] : picked_) out.push_back(user);
        return out;
    }
    void pick(const std::string& user, const std::u16string& shown) {
        if (is_picked(user) || is_member(user)) return;
        picked_.emplace_back(user, shown);
        build_chips();
    }
    std::function<void()> on_change;

private:
    bool is_member(const std::string& user) const { return members_.contains(to_lower(user)); }
    bool is_picked(const std::string& user) const {
        for (const auto& p : picked_)
            if (iequals(p.first, user)) return true;
        return false;
    }
    std::u16string text() const {
        const std::u16string t = field_->text();
        const size_t a = t.find_first_not_of(u" \t\r\n"), b = t.find_last_not_of(u" \t\r\n");
        return a == std::u16string::npos ? std::u16string() : t.substr(a, b - a + 1);
    }
    void query() {
        const std::u16string q = text();
        if (q.size() < 2) {
            users_ = json::array();
            build_results();
            return;
        }
        if (in_flight_) return;  // the answer asks again when the text changed meanwhile
        in_flight_ = true;
        std::weak_ptr<int> alive = alive_;
        shell_.call("messaging.directory", {{"q", ui::to_utf8(q)}}, [this, alive, q](Result<json> r) {
            if (!alive.lock()) return;
            in_flight_ = false;
            if (text() != q) {
                query();
                return;
            }
            error_ = r ? std::u16string() : ui::to_utf16(r.error().message);
            users_ = r ? r->value("users", json::array()) : json::array();
            build_results();
        });
    }
    // After the click that asked for it returns: rebuilding removes the control that was clicked.
    void later(std::function<void()> f) {
        std::weak_ptr<int> alive = alive_;
        shell_.window().set_timer(1, [alive, f = std::move(f)] {
            if (alive.lock()) f();
        });
    }
    void changed() {
        relayout();
        invalidate();
        if (on_change) on_change();
    }
    void build_chips() {
        chips_->clear_children();
        chips_->visible = !picked_.empty();
        for (const auto& [user, shown] : picked_) {
            auto* chip = chips_->emplace<ui::Box>(id_ + "-chip-" + user, Paint::accent_surface, 14, Paint::control_border);
            chip->axis = ui::Axis::row;
            chip->width = ui::Sizing::fit;
            chip->gap = 2;
            chip->padding = ui::Insets{10, 0, 2, 0};
            chip->role = ui::Role::list_item;
            chip->name = shown;
            auto* l = chip->emplace<ui::Label>(shell_.ui(), shown, TextStyle::body, Paint::text, false);
            l->role = ui::Role::none;
            l->padding.top = 5;
            auto* x = chip->emplace<ui::IconButton>(shell_.ui(), id_ + "-remove-" + user, ui::icon::cancel,
                                                    shell_.t("chats.picker_remove", {{"name", shown}}));
            x->on_click = [this, u = user] {
                later([this, u] {
                    std::erase_if(picked_, [&](const auto& p) { return p.first == u; });
                    build_chips();
                    build_results();
                    shell_.announce(shell_.t("chats.picker_removed"));
                });
            };
        }
        changed();
    }
    void build_results() {
        results_->clear_children();
        if (!error_.empty()) {
            results_->emplace<ui::Label>(shell_.ui(), shell_.t("common.error_prefix") + u" " + error_, TextStyle::body, Paint::error, true, id_ + "-error");
        } else if (text().size() < 2) {
            results_->emplace<ui::Label>(shell_.ui(), shell_.t("chats.picker_hint"), TextStyle::caption, Paint::text_secondary, true, id_ + "-hint");
        } else if (users_.empty() && !in_flight_) {
            results_->emplace<ui::Label>(shell_.ui(), shell_.t("chats.no_users"), TextStyle::body, Paint::text_secondary, true, id_ + "-none");
        }
        for (const auto& u : users_) {
            const std::string user = jstr(u, "user_name");
            const std::u16string shown = ui::to_utf16(jstr(u, "display_name", user));
            const bool member = is_member(user);
            const std::u16string what = shell_.t(member ? "chats.picker_member_row" : "chats.picker_row",
                                                 {{"name", shown}, {"user", ui::to_utf16(user)}, {"presence", presence_(jstr(u, "presence"))}});
            auto* line = results_->emplace<Node>(id_ + "-user-" + user);
            line->axis = ui::Axis::row;
            line->gap = 8;
            line->role = ui::Role::list_item;
            line->name = what;
            line->emplace<ui::Avatar>(shell_.ui(), shown);
            auto* cb = line->emplace<ui::Checkbox>(shell_.ui(), id_ + "-pick-" + user, what, member || is_picked(user));
            cb->padding.top = 4;
            cb->enabled = !member;
            cb->on_change = [this, user, shown](bool on) {
                later([this, user, shown, on] {
                    if (on) pick(user, shown);
                    else {
                        std::erase_if(picked_, [&](const auto& p) { return p.first == user; });
                        build_chips();
                    }
                });
            };
        }
        changed();
    }

    Shell& shell_;
    std::string id_;
    std::set<std::string> members_;  // lower case
    std::function<std::u16string(const std::string&)> presence_;
    ui::TextField* field_ = nullptr;
    Node* chips_ = nullptr;
    Node* results_ = nullptr;
    std::vector<std::pair<std::string, std::u16string>> picked_;
    json users_ = json::array();
    std::u16string error_;
    bool in_flight_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace

ChatsView::ChatsView(Shell& shell, std::string chat) : Node("chats-view"), shell_(shell), open_(std::move(chat)) {
    gap = 12;
    rebuild();
    load_list();
    if (!open_.empty()) open(open_);
    std::weak_ptr<int> alive = alive_;
    shell_.call("knowledge.save_as_note", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        save_as_note_ = r && jbool(*r, "enabled");
        rebuild();
    });
}

ui::Button* ChatsView::button(Node& parent, const std::string& id, const std::u16string& label, std::function<void()> f,
                              ui::Button::Kind kind, char16_t glyph) {
    auto* b = parent.emplace<ui::Button>(shell_.ui(), id, label, kind, glyph);
    b->width = ui::Sizing::fit;
    b->on_click = std::move(f);
    b->enabled = !busy_;
    return b;
}

std::u16string ChatsView::presence_text(const std::string& presence) {
    return shell_.t("chats.presence." + (presence.empty() ? std::string("offline") : presence));
}

std::u16string ChatsView::title_of(const json& c) const {
    if (jstr(c, "kind") == "team") return u16(jstr(c, "name"));
    if (const json* p = jobj(c, "peer")) return u16(jstr(*p, "display_name", jstr(*p, "user_name")));
    return u16(jstr(c, "name"));
}

// The local time of a message; older than a day with its date.
std::u16string ChatsView::when(const std::string& iso) {
    const auto ms = parse_iso8601_ms(iso);
    if (!ms) return u16(iso);
    std::u16string t = shell_.clock(*ms / 1000);
    if (shell_.now() - *ms > 20LL * 3600 * 1000) t = u16(iso.substr(0, 10)) + u" " + t;
    return t;
}

// ---- data ------------------------------------------------------------------------------------------------------------

void ChatsView::act(const std::string& method, const json& params, std::function<void(const json&)> done) {
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

void ChatsView::load_list() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("messaging.chats", json::object(), [this, alive](Result<json> r) {
        if (!alive.lock()) return;
        if (r) status_ = *r;
        else error_ = u16(r.error().message);
        rebuild();
    });
}

void ChatsView::open(const std::string& id) {
    if (id != open_) messages_ = nullptr;
    open_ = id;
    std::weak_ptr<int> alive = alive_;
    shell_.call("messaging.messages", {{"chat", id}}, [this, alive, id](Result<json> r) {
        if (!alive.lock() || open_ != id) return;
        if (!r) {
            error_ = u16(r.error().message);
            rebuild();
            return;
        }
        // A message from someone else that was not shown yet is read out.
        const json before = messages_.is_object() ? messages_.value("messages", json::array()) : json::array();
        const json after = r->value("messages", json::array());
        if (!before.empty() && !after.empty() && jstr(after.back(), "id") != jstr(before.back(), "id") &&
            !iequals(jstr(after.back(), "sender"), jstr(status_, "me")))
            shell_.announce(shell_.t("chats.new_message", {{"name", u16(jstr(after.back(), "sender_name", jstr(after.back(), "sender")))}}));
        messages_ = *r;
        load_list();  // its unread count is gone
        rebuild();
    });
    rebuild();
}

void ChatsView::changed(const json& data) {
    load_list();
    const std::string chat = jstr(data, "chat");
    if (!open_.empty() && (chat.empty() || chat == open_)) open(open_);
}

void ChatsView::send() {
    const std::u16string text = drafts_[open_];
    if (open_.empty() || blank(text)) return;
    const std::string id = open_;
    act("messaging.send", {{"chat", id}, {"text", ui::to_utf8(text)}}, [this, id](const json& r) {
        drafts_.erase(id);
        if (open_ == id) messages_ = {{"chat", r["chat"]}, {"messages", r["messages"]}};
        const json* sent = jobj(r, "sent");
        shell_.announce(shell_.t(sent && jbool(*sent, "pending") ? "chats.queued" : "chats.sent"));
        load_list();
    });
}

// ---- dialogs -----------------------------------------------------------------------------------------------------------

// New Chat (fold-chats-into-inbox D2): the people picker, then a name. One colleague and no name make a direct Chat (the
// existing one when there is one); two or more make a team Chat, which needs the name. The Chat opens in the reading pane.
void Shell::ask_new_chat() {
    const std::string id = "new-chat-dialog";
    auto d = std::make_unique<ui::Dialog>(ui(), id, t("chats.new_title"));
    auto* picker = d->body().emplace<PeoplePicker>(*this, id + "-people", std::set<std::string>{}, [this](const std::string& p) {
        return t("chats.presence." + (p.empty() ? std::string("offline") : p));
    });
    auto* name_field = d->body().emplace<ui::TextField>(ui(), id + "-name", t("chats.new_name"));
    name_field->set_placeholder(t("chats.new_name_hint"));
    ui::Dialog* dp = d.get();
    d->add_button(id + "-cancel", t("chats.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    auto* start = d->add_button(id + "-start", t("chats.new_start"), ui::Button::Kind::primary, [this, dp, name_field, picker] {
        const std::string n = trim(ui::to_utf8(name_field->text()));
        const auto users = picker->picked();
        if (users.empty()) return;
        json params;
        if (users.size() == 1 && n.empty()) {
            params = {{"kind", "direct"}, {"user", users.front()}};
        } else if (n.empty()) {
            announce(t("chats.team_incomplete"));
            window().tree().set_focus(name_field);
            return;
        } else {
            params = {{"kind", "team"}, {"name", n}, {"members", users}};
        }
        dp->close();
        call("messaging.create", params, [this](Result<json> r) {
            if (!r) {
                announce(t("common.error_prefix") + u" " + ui::to_utf16(r.error().message));
                return;
            }
            navigate("#/chats/" + jstr(*r, "id"));  // the new Chat in the reading pane
        });
    });
    start->enabled = false;
    picker->on_change = [start, picker] {
        start->enabled = !picker->picked().empty();
        start->invalidate();
    };
    window().modal().show(std::move(d));
    window().render();
}

void Shell::set_busy(bool busy) {
    call("messaging.busy", {{"busy", busy}}, [this, busy](Result<json> r) {
        if (!r) {
            announce(t("common.error_prefix") + u" " + ui::to_utf16(r.error().message));
            return;
        }
        if (status_.is_object() && status_.contains("messaging") && status_["messaging"].is_object())
            status_["messaging"]["busy"] = jbool(*r, "busy", busy);
        announce(t(busy ? "status_bar.busy_on" : "status_bar.busy_off"));
        refresh_status();
    });
}

// Add members (improve-desktop-look D7): pick colleagues by searching; the Chat's members are marked.
void ChatsView::ask_add(const std::string& chat, const json& members) {
    const std::string id = "chat-add-dialog";
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), id, shell_.t("chats.add"));
    std::set<std::string> current;
    for (const auto& m : members) current.insert(to_lower(jstr(m, "user_name")));
    auto* picker = d->body().emplace<PeoplePicker>(shell_, id + "-picker", std::move(current),
                                                    [this](const std::string& p) { return presence_text(p); });
    ui::Dialog* dp = d.get();
    d->add_button(id + "-cancel", shell_.t("chats.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    auto* add = d->add_button(id + "-add", shell_.t("chats.add_confirm"), ui::Button::Kind::primary, [this, dp, chat, picker] {
        const auto users = picker->picked();
        if (users.empty()) return;
        dp->close();
        act("messaging.add_members", {{"chat", chat}, {"users", users}}, [this, chat](const json&) {
            load_list();
            open(chat);
        });
    });
    add->enabled = false;
    picker->on_change = [add, picker] {
        add->enabled = !picker->picked().empty();
        add->invalidate();
    };
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

// Save as note (add-save-as-note D3, improve-desktop-look D4): the period as a list of choices inside the dialog, then
// the draft opens in the Knowledge view; nothing is stored yet.
void ChatsView::ask_save_note(const std::string& chat) {
    const std::string id = "chat-save-note-dialog";
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), id, shell_.t("chats.save_note_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("chats.save_note_body"), TextStyle::body, Paint::text, true, id + "-body");
    auto* choices = d->body().emplace<Node>(id + "-periods");
    choices->role = ui::Role::group;
    choices->name = shell_.t("chats.save_note_periods");
    choices->gap = 6;
    ui::Dialog* dp = d.get();
    const std::pair<const char*, char16_t> periods[] = {{"day", ui::icon::calendar_day}, {"week", ui::icon::calendar_week}, {"all", ui::icon::history}};
    for (const auto& [period, glyph] : periods) {
        const std::string pr = period;
        auto* b = choices->emplace<ui::Button>(shell_.ui(), id + "-" + pr, shell_.t("chats.period_" + pr),
                                               pr == "week" ? ui::Button::Kind::primary : ui::Button::Kind::standard, glyph);
        b->width = ui::Sizing::fill;  // a list of full-width choices
        b->on_click = [this, dp, chat, pr] {
            dp->close();
            act("knowledge.note.draft_from_chat", {{"chat", chat}, {"period", pr}}, [this](const json& r) {
                shell_.knowledge_draft = r;
                shell_.navigate("#/knowledge");
            });
        };
    }
    d->add_button(id + "-cancel", shell_.t("chats.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

void ChatsView::ask_leave(const std::string& chat, const std::u16string& title) {
    const std::string id = "chat-leave-dialog";
    auto d = std::make_unique<ui::Dialog>(shell_.ui(), id, shell_.t("chats.leave_title"));
    d->body().emplace<ui::Label>(shell_.ui(), shell_.t("chats.leave_body", {{"name", title}}), TextStyle::body, Paint::text, true, id + "-body");
    ui::Dialog* dp = d.get();
    d->add_button(id + "-cancel", shell_.t("chats.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
    d->add_button(id + "-confirm", shell_.t("chats.leave"), ui::Button::Kind::primary, [this, dp, chat, title] {
        dp->close();
        act("messaging.leave", {{"chat", chat}}, [this, chat, title](const json& r) {
            status_ = r;
            shell_.announce(shell_.t("chats.left", {{"name", title}}));
            // Its entry is gone: back to the list the Chat was opened from.
            if (open_ == chat) shell_.navigate(view_hash(shell_.view()));
        });
    });
    shell_.window().modal().show(std::move(d));
    shell_.window().render();
}

// ---- the view ----------------------------------------------------------------------------------------------------------

void ChatsView::rebuild() {
    // Keep the keyboard where it was (the composer, a list entry) across the rebuild.
    std::string focus;
    if (ui::Tree* t = Node::tree(); t && t->focused())
        for (const Node* n = t->focused(); n; n = n->parent())
            if (n == this) {
                focus = t->focused()->automation_id();
                break;
            }
    clear_children();
    composer_ = nullptr;
    // On a narrow window the reading pane replaces the list: Back returns to it.
    if (shell_.single_pane())
        button(*row_box(*this, "chat-back-row"), "chat-back", shell_.t("chats.back"), [this] { shell_.navigate(view_hash(shell_.view())); },
               ui::Button::Kind::subtle);
    if (!status_.is_object()) {
        emplace<ui::Label>(shell_.ui(), error_.empty() ? shell_.t("common.loading") : shell_.t("chats.unavailable"), TextStyle::body,
                           Paint::text_secondary, true, "chats-loading");
    } else if (!jbool(status_, "enabled")) {
        // policy.messaging.mode off: nothing is called, and the view says why.
        emplace<ui::Label>(shell_.ui(), shell_.t("chats.off"), TextStyle::body, Paint::warning, true, "chats-off");
    } else {
        if (!jbool(status_, "connected"))
            emplace<ui::Label>(shell_.ui(), shell_.t("chats.offline"), TextStyle::body, Paint::warning, true, "chats-offline");
        if (!error_.empty())
            emplace<ui::Label>(shell_.ui(), shell_.t("common.error_prefix") + u" " + error_, TextStyle::body, Paint::error, true, "chats-error");
        build_conversation(*this);
    }
    if (Node::tree()) {
        relayout();
        invalidate();
        if (!focus.empty())
            if (Node* n = find_id(*this, focus); n && n->focusable) Node::tree()->set_focus(n);
    }
}

void ChatsView::build_conversation(Node& parent) {
    json c = messages_.is_object() ? messages_.value("chat", json()) : json();
    if (!c.is_object())
        for (const auto& x : status_.value("chats", json::array()))
            if (jstr(x, "id") == open_) c = x;
    if (!c.is_object()) {
        // Left, or never one of the user's Chats (an old link); while the messages load, nothing yet.
        if (messages_.is_object() || !error_.empty())
            parent.emplace<ui::Label>(shell_.ui(), shell_.t("chats.gone"), TextStyle::body, Paint::text_secondary, true, "chat-gone");
        return;
    }
    const std::string id = open_;
    const std::u16string title = title_of(c);
    auto* box = parent.emplace<ui::Box>("chat-pane", Paint::surface, 8, Paint::border);
    box->role = ui::Role::group;
    box->name = title;
    box->padding = ui::Insets::all(12);
    box->gap = 8;
    box->emplace<ui::Label>(shell_.ui(), title, TextStyle::subtitle, Paint::text, true, "chat-title")->role = ui::Role::heading;
    const bool team = jstr(c, "kind") == "team";
    if (team) {
        std::u16string names;
        for (const auto& m : c.value("members", json::array()))
            names += (names.empty() ? u"" : u", ") + u16(jstr(m, "display_name", jstr(m, "user_name"))) + u" (" + presence_text(jstr(m, "presence")) + u")";
        box->emplace<ui::Label>(shell_.ui(), shell_.t("chats.members", {{"names", names}}), TextStyle::caption, Paint::text_secondary, true, "chat-members");
        auto* actions = row_box(*box, "chat-actions");
        actions->wraps = true;
        button(*actions, "chat-rename", shell_.t("chats.rename"), [this, id, title] {
            shell_.ask_text_value("chat-rename", shell_.t("chats.rename_title"), shell_.t("chats.team_name"), title, [this, id](const std::string& v) {
                act("messaging.rename", {{"chat", id}, {"name", v}}, [this, id](const json&) {
                    load_list();
                    open(id);
                });
            });
        }, ui::Button::Kind::standard, ui::icon::rename);
        button(*actions, "chat-add", shell_.t("chats.add"), [this, id, members = c.value("members", json::array())] { ask_add(id, members); },
               ui::Button::Kind::standard, ui::icon::add_people);
        if (save_as_note_)
            button(*actions, "chat-save-note", shell_.t("chats.save_as_note"), [this, id] { ask_save_note(id); }, ui::Button::Kind::standard,
                   ui::icon::note);
        button(*actions, "chat-leave", shell_.t("chats.leave"), [this, id, title] { ask_leave(id, title); }, ui::Button::Kind::subtle,
               ui::icon::leave);
    } else {
        if (const json* p = jobj(c, "peer"))
            box->emplace<ui::Label>(shell_.ui(), presence_text(jstr(*p, "presence")), TextStyle::caption, Paint::text_secondary, true, "chat-presence");
        if (save_as_note_)
            button(*row_box(*box, "chat-actions"), "chat-save-note", shell_.t("chats.save_as_note"), [this, id] { ask_save_note(id); },
                   ui::Button::Kind::standard, ui::icon::note);
    }

    // The messages, oldest first; queued ones last and marked pending.
    auto* list = box->emplace<Node>("chat-messages");
    list->role = ui::Role::list;
    list->name = shell_.t("chats.messages");
    list->gap = 8;
    if (!messages_.is_object()) {
        list->emplace<ui::Label>(shell_.ui(), shell_.t("common.loading"), TextStyle::body, Paint::text_secondary, true);
    } else {
        const json msgs = messages_.value("messages", json::array());
        if (msgs.empty())
            list->emplace<ui::Label>(shell_.ui(), shell_.t("chats.no_messages"), TextStyle::body, Paint::text_secondary, true, "chat-no-messages");
        const std::string mine = jstr(status_, "me");
        for (const auto& m : msgs) {
            const std::string mid = jstr(m, "id");
            const bool own = iequals(jstr(m, "sender"), mine);
            const std::u16string sender = own ? shell_.t("chats.you") : u16(jstr(m, "sender_name", jstr(m, "sender")));
            const std::u16string meta = shell_.t("chats.meta", {{"sender", sender}, {"time", when(jstr(m, "at"))}});
            // Own messages on the accent-tinted surface, the others on the alternate surface, each with the sender's
            // avatar (improve-desktop-look D6).
            auto* item = list->emplace<ui::Box>("chat-message-" + mid, own ? Paint::accent_surface : Paint::surface_alt, 8, Paint::border);
            item->role = ui::Role::list_item;
            item->name = meta;
            item->padding = ui::Insets::xy(12, 8);
            item->gap = 4;
            auto* head = row_box(*item);
            const std::u16string sender_name = u16(jstr(m, "sender_name", jstr(m, "sender")));
            head->emplace<ui::Avatar>(shell_.ui(), sender_name, own ? Paint::accent : Paint::surface, own ? Paint::accent_text : Paint::text);
            auto* who = head->emplace<ui::Label>(shell_.ui(), sender, TextStyle::body_strong, Paint::text, false);
            who->role = ui::Role::none;
            who->padding.top = 4;
            auto* at = head->emplace<ui::Label>(shell_.ui(), when(jstr(m, "at")), TextStyle::caption, Paint::text_secondary, false,
                                                "chat-message-meta-" + mid);
            at->role = ui::Role::none;
            at->padding.top = 6;
            head->emplace<Node>()->width = ui::Sizing::fill;
            const std::string text = jstr(m, "text");
            auto* copy = head->emplace<ui::IconButton>(shell_.ui(), "chat-message-copy-" + mid, ui::icon::copy, shell_.t("feed.copy_message"));
            copy->on_click = [this, text] { shell_.announce(shell_.t(shell_.copy_text(text) ? "feed.copied" : "question.copy_failed")); };
            auto* md = item->emplace<ui::MarkdownView>(shell_.ui(), meta, "chat-message-text-" + mid);
            md->set_markdown(text);
            if (m.contains("error"))
                item->emplace<ui::Label>(shell_.ui(), shell_.t("chats.failed", {{"reason", u16(jstr(m, "error"))}}), TextStyle::caption, Paint::error, true,
                                         "chat-message-failed-" + mid);
            else if (jbool(m, "pending"))
                item->emplace<ui::Label>(shell_.ui(), shell_.t("chats.pending"), TextStyle::caption, Paint::warning, true, "chat-message-pending-" + mid);
        }
    }

    // The composer: Enter sends, Shift+Enter starts a new line.
    auto* compose = row_box(*box, "chat-compose");
    ui::TextField::Options o;
    o.multiline = true;
    o.min_lines = 2;
    o.max_lines = 8;
    auto* field = compose->emplace<ui::TextField>(shell_.ui(), "chat-composer", shell_.t("chats.composer"), o);
    field->set_text(drafts_[id]);
    ui::TextField* fp = field;
    field->on_change = [this, fp, id] { drafts_[id] = fp->text(); };
    composer_ = field;
    field->on_key_down = [this, id](const ui::KeyEvent& e) {
        // Escape discards this Chat's recording (add-dictation D3).
        if (e.key == ui::kKeyEscape && !e.ctrl && !e.alt && !e.shift) return shell_.cancel_dictation_of("chat:" + id);
        if (e.key != ui::kKeyEnter || e.shift) return false;
        send();
        return true;
    };
    compose->emplace<DictateControl>(shell_, "chat-dictate", "chat:" + id, alive_, [this] { return composer_; });
    button(*compose, "chat-send", shell_.t("chats.send"), [this] { send(); }, ui::Button::Kind::primary);
}


}  // namespace wb::shell
