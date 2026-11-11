// The API connectors of Settings > Connectors (client/api-connectors Connector Status and Personal Token Entry): each
// connector's kind, state, usable tools and the reason none is usable, and for a personal access token the add, replace
// and remove actions. The token field is masked; the typed text is taken when it is sent, so the field is empty after it.
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

// "2026-11-10T15:04:05.123Z" as "2026-11-10 15:04 UTC".
std::u16string set_time(const std::string& iso) {
    if (iso.size() < 16) return u16(iso);
    return u16(iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC");
}

}  // namespace

void PagePane::load_connectors() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("connectors.list", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            connectors_ = *res;
            errors_.erase("connectors");
        } else {
            errors_["connectors"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::build_api_connectors(Node& c) {
    const json* list = connectors_.is_object() ? jobj(connectors_, "connectors") : nullptr;
    if (!list || !list->is_array()) {
        if (!errors_.contains("connectors")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "connectors");
        return;
    }
    std::vector<const json*> apis;
    for (const json& x : *list)
        if (jstr(x, "kind") == "api") apis.push_back(&x);
    if (apis.empty()) {
        error_line(c, "connectors");
        return;
    }

    // The bundle's view of each connector: usable tools and the reason none is usable. Missing until the bundle arrives.
    const json& status = shell_.status();
    const json* bundle = status.is_object() && status.contains("api_connectors") && status["api_connectors"].is_array() ? &status["api_connectors"] : nullptr;

    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.connectors.api.heading"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* items = c.emplace<Node>("settings-api-connectors");
    items->role = ui::Role::list;
    items->name = shell_.t("settings.connectors.api.heading");
    items->gap = 8;
    for (const json* x : apis) {
        const std::string id = jstr(*x, "id");
        const std::string state = jstr(*x, "state");
        const std::string auth = jstr(*x, "auth");
        const std::string cname = jstr(*x, "name", id);
        const json* b = nullptr;
        if (bundle)
            for (const json& e : *bundle)
                if (jstr(e, "id") == id) b = &e;
        const long long usable = b ? jint(*b, "usable_tools") : jint(*x, "offered_tools");
        const long long total = b ? jint(*b, "operations") : jint(*x, "offered_tools");
        std::string reason = b ? jstr(*b, "reason") : std::string{};
        if (reason.empty() && (state == "needs-token" || state == "token-rejected")) reason = state;
        if (reason.empty() && total == 0) reason = "none-selected";

        const std::string sk = "settings.connectors.api.states." + state;
        const std::u16string st = shell_.ui().strings.has(sk) ? shell_.t(sk) : u16(state);
        auto* item = items->emplace<Node>("api-connector-" + id);
        item->role = ui::Role::list_item;
        item->gap = 2;
        item->name = u16(cname) + u", " + shell_.t("settings.connectors.api.kind") + u", " + st;
        auto* head = item->emplace<Node>();
        head->axis = ui::Axis::row;
        head->gap = 8;
        head->emplace<ui::Label>(shell_.ui(), u16(cname), TextStyle::body_strong, Paint::text, false)->role = ui::Role::none;
        head->emplace<ui::Label>(shell_.ui(), shell_.t("settings.connectors.api.kind"), TextStyle::caption, Paint::text_secondary, false)->role =
            ui::Role::none;
        head->emplace<ui::Badge>(shell_.ui(), st,
                                 state == "needs-token" || state == "token-rejected" ? Paint::warning_background : Paint::info_background,
                                 Paint::text)
            ->role = ui::Role::none;
        item->emplace<ui::Label>(shell_.ui(),
                                 shell_.t("settings.connectors.tools", {{"usable", u16(std::to_string(usable))}, {"total", u16(std::to_string(total))}}),
                                 TextStyle::caption, Paint::text_secondary)
            ->role = ui::Role::none;
        if (usable == 0) {
            std::u16string why = shell_.t("settings.connectors.api.no_usable_tools");
            if (!reason.empty()) why += u": " + shell_.t("settings.connectors.api.reason." + reason);
            item->emplace<ui::Label>(shell_.ui(), shell_.t("settings.connectors.warning") + u": " + why, TextStyle::caption)->role = ui::Role::none;
        }
        if (const std::string ht = jstr(*x, "help_text"); !ht.empty())
            item->emplace<ui::Label>(shell_.ui(), u16(ht), TextStyle::caption, Paint::text_secondary, true)->role = ui::Role::none;
        if (const std::string hl = jstr(*x, "help_link"); !hl.empty())
            item->emplace<ui::Label>(shell_.ui(), shell_.t("settings.connectors.api.help_link", {{"link", u16(hl)}}), TextStyle::caption,
                                     Paint::text_secondary, true)
                ->role = ui::Role::none;
        if (auth != "user-pat") continue;

        // The personal access token: whether one is set and when, never its text.
        const json* tok = jobj(*x, "token");
        const bool has = tok && jbool(*tok, "set");
        item->emplace<ui::Label>(shell_.ui(),
                                 has ? shell_.t("settings.connectors.api.token_set", {{"date", set_time(jstr(*tok, "set_at"))}})
                                     : shell_.t("settings.connectors.api.no_token"),
                                 TextStyle::caption, Paint::text_secondary)
            ->role = ui::Role::none;
        const bool editing = connector_editing_ == id;
        auto* actions = item->emplace<Node>();
        actions->axis = ui::Axis::row;
        actions->gap = 8;
        if (!editing) {
            const bool replacing = has && state == "ready";
            button(*actions, "settings-connector-token-open-" + id,
                   shell_.t(replacing ? "settings.connectors.api.replace_token" : "settings.connectors.api.add_token"),
                   [this, id] {
                       connector_editing_ = id;
                       rebuild();
                   })
                ->enabled = busy_.empty();
            if (has)
                button(*actions, "settings-connector-token-remove-" + id, shell_.t("settings.connectors.api.remove_token"),
                       [this, id] {
                           act("connectors", "connectors.token.remove", {{"id", id}}, [this](const json&) {
                               shell_.announce(shell_.t("settings.connectors.api.token_removed"));
                               load_connectors();
                           });
                       },
                       ui::Button::Kind::subtle)
                    ->enabled = busy_.empty();
            continue;
        }
        ui::TextField::Options o;
        o.secret = true;
        o.max_length = 4096;
        auto* field = item->emplace<ui::TextField>(shell_.ui(), "settings-connector-token-" + id, shell_.t("settings.connectors.api.token_label"), o);
        field->set_text(connector_token_.text());
        ui::TextField* fp = field;
        field->on_change = [this, fp] { connector_token_.set(fp->text()); };
        button(*actions, "settings-connector-token-save-" + id, shell_.t("settings.connectors.api.save_token"), [this, id, fp] {
            if (!busy_.empty()) return;
            // The draft is taken before the call: the token is held by the request only, and the field is cleared.
            const std::string token = ui::to_utf8(connector_token_.take());
            fp->set_text(u"");
            if (token.find_first_not_of(" \t\r\n") == std::string::npos) {
                connector_editing_.clear();
                rebuild();
                return;
            }
            act("connectors", "connectors.token.set", {{"id", id}, {"token", token}}, [this](const json&) {
                connector_editing_.clear();
                shell_.announce(shell_.t("settings.connectors.api.token_saved"));
                load_connectors();
            });
        })->enabled = busy_.empty();
        button(*actions, "settings-connector-token-cancel-" + id, shell_.t("settings.connectors.api.cancel_token"), [this] {
            connector_token_.clear();
            connector_editing_.clear();
            rebuild();
        }, ui::Button::Kind::subtle);
    }
    error_line(c, "connectors");
}

}  // namespace wb::shell
