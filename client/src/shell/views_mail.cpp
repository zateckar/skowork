// Mail and calendar (client/mail-calendar): the Settings section with the mailbox connection, the last sync, what is
// synced and withheld, and the exclusions — policy's, which cannot be removed, and the user's own folders and
// categories. The host does the work (mail.status, .connect, .disconnect, .sync_now, .exclusions.add, .exclusions.remove).
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

// "2026-10-26T15:04:05.123Z" as "2026-10-26 15:04 UTC".
std::u16string sync_time(const std::string& iso) {
    if (iso.size() < 16) return u16(iso);
    return u16(iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC");
}

std::string joined(const json* list) {
    std::string out;
    if (list && list->is_array())
        for (const auto& x : *list)
            if (x.is_string()) out += (out.empty() ? "" : ", ") + x.get<std::string>();
    return out;
}

}  // namespace

void PagePane::load_mail() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("mail.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            mail_ = *res;
            errors_.erase("mail");
        } else {
            errors_["mail"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::mail_changed() {
    if (shell_.route().name == RouteName::settings) load_mail();
}

void PagePane::build_mail(Node& c) {
    text(c, shell_.t("settings.mail.body"), Paint::text_secondary);
    if (!mail_.is_object()) {
        if (!errors_.contains("mail")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "mail");
        return;
    }
    // OAuth Configuration Missing, and policy this build does not accept.
    if (!jbool(mail_, "available")) {
        const std::string code = jstr(mail_, "unavailable_code");
        text(c, code == "not_configured" ? shell_.t("settings.mail.not_configured")
                                         : shell_.t("settings.mail.policy_error", {{"reason", u16(jstr(mail_, "unavailable_reason"))}}),
             Paint::warning, "settings-mail-unavailable");
        error_line(c, "mail");
        return;
    }
    const std::string state = jstr(mail_, "state", "disconnected");
    std::u16string st = shell_.t("settings.mail.states." + state);
    if (state == "connected" && !jstr(mail_, "account").empty()) st = shell_.t("settings.mail.connected_as", {{"account", u16(jstr(mail_, "account"))}});
    row(c, "settings-mail-state", shell_.t("settings.mail.connection"), st,
        state == "interaction_required" ? Paint::warning : Paint::text);
    if (state == "interaction_required") text(c, shell_.t("settings.mail.interaction_required"), Paint::warning, "settings-mail-reconnect");
    if (const std::string e = jstr(mail_, "error"); !e.empty() && state != "connected")
        text(c, shell_.t("settings.mail.problem", {{"reason", u16(e)}}), Paint::text_secondary, "settings-mail-auth-error");
    if (jbool(mail_, "mock")) text(c, shell_.t("settings.mail.mock"), Paint::text_secondary, "settings-mail-mock");

    auto* actions = c.emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    if (state == "disconnected" || state == "interaction_required") {
        button(*actions, "settings-mail-connect", shell_.t("settings.mail.connect"), [this] {
            act("mail", "mail.connect", json::object(), [this](const json&) { shell_.announce(shell_.t("settings.mail.connect_started")); });
        })->enabled = busy_.empty();
    } else if (state == "connecting") {
        text(*actions, shell_.t("settings.mail.connecting"), Paint::text_secondary, "settings-mail-connecting");
    }
    if (state == "connected") {
        button(*actions, "settings-mail-sync", shell_.t("settings.mail.sync_now"), [this] {
            act("mail", "mail.sync_now", json::object(), [this](const json&) { shell_.announce(shell_.t("settings.mail.sync_started")); });
        })->enabled = busy_.empty();
    }
    if (state == "connected" || state == "interaction_required") {
        button(*actions, "settings-mail-disconnect", shell_.t("settings.mail.disconnect"), [this] {
            act("mail", "mail.disconnect", json::object(), [this](const json&) {
                shell_.announce(shell_.t("settings.mail.disconnected"));
                load_mail();
            });
        }, ui::Button::Kind::subtle)->enabled = busy_.empty();
    }

    // The local copy.
    const std::string last = jstr(mail_, "last_sync");
    row(c, "settings-mail-last-sync", shell_.t("settings.mail.last_sync"), last.empty() ? shell_.t("settings.mail.never") : sync_time(last));
    row(c, "settings-mail-counts", shell_.t("settings.mail.local_copy"),
        shell_.t("settings.mail.counts", {{"messages", u16(std::to_string(jint(mail_, "messages")))},
                                          {"events", u16(std::to_string(jint(mail_, "events")))},
                                          {"withheld", u16(std::to_string(jint(mail_, "withheld")))}}));
    if (const std::string e = jstr(mail_, "sync_error"); !e.empty())
        text(c, shell_.t("settings.mail.sync_error", {{"reason", u16(e)}}), Paint::warning, "settings-mail-sync-error");

    // Exclusions: policy's first (fixed), then the user's (removable).
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.mail.exclusions"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.mail.exclusions_body"), Paint::text_secondary);
    const json empty = json::object();
    const json* pol = jobj(mail_, "policy_exclusions");
    if (!pol) pol = &empty;
    if (const std::string f = joined(jobj(*pol, "folders")); !f.empty())
        row(c, "settings-mail-policy-folders", shell_.t("settings.mail.policy_folders"), u16(f));
    if (const std::string g = joined(jobj(*pol, "categories")); !g.empty())
        row(c, "settings-mail-policy-categories", shell_.t("settings.mail.policy_categories"), u16(g));
    text(c, shell_.t(jbool(*pol, "private_items", true) ? "settings.mail.private_hidden" : "settings.mail.private_shown"), Paint::text_secondary,
         "settings-mail-private");

    const json* user = jobj(mail_, "user_exclusions");
    auto* list = c.emplace<Node>("settings-mail-exclusions");
    list->role = ui::Role::list;
    list->name = shell_.t("settings.mail.your_exclusions");
    list->gap = 4;
    bool any = false;
    for (const char* kind : {"folder", "category"}) {
        const json* values = user ? jobj(*user, std::string(kind) == "folder" ? "folders" : "categories") : nullptr;
        if (!values || !values->is_array()) continue;
        for (const auto& v : *values) {
            if (!v.is_string()) continue;
            any = true;
            const std::string value = v.get<std::string>();
            const std::string key = std::string(kind) + "-" + value;
            auto* item = list->emplace<Node>("mail-exclusion-" + key);
            item->role = ui::Role::list_item;
            item->axis = ui::Axis::row;
            item->gap = 8;
            const std::u16string what = shell_.t(std::string("settings.mail.excluded_") + kind, {{"name", u16(value)}});
            item->name = what;
            item->emplace<ui::Label>(shell_.ui(), what, TextStyle::body, Paint::text, false)->role = ui::Role::none;
            auto* rm = button(*item, "mail-exclusion-remove-" + key, shell_.t("settings.mail.remove"), [this, kind = std::string(kind), value] {
                act("mail", "mail.exclusions.remove", {{"kind", kind}, {"value", value}}, [this](const json&) { load_mail(); });
            }, ui::Button::Kind::subtle);
            rm->name = shell_.t("settings.mail.remove_named", {{"name", what}});
            rm->enabled = busy_.empty();
        }
    }
    if (!any) text(*list, shell_.t("settings.mail.no_exclusions"), Paint::text_secondary, "settings-mail-no-exclusions");

    auto* field = c.emplace<ui::TextField>(shell_.ui(), "settings-mail-exclusion", shell_.t("settings.mail.exclusion_name"));
    field->set_text(mail_exclusion_);
    ui::TextField* fp = field;
    field->on_change = [this, fp] { mail_exclusion_ = fp->text(); };
    auto* adds = c.emplace<Node>();
    adds->axis = ui::Axis::row;
    adds->gap = 8;
    for (const char* kind : {"folder", "category"}) {
        button(*adds, std::string("settings-mail-exclude-") + kind, shell_.t(std::string("settings.mail.exclude_") + kind), [this, kind = std::string(kind)] {
            const std::string value = ui::to_utf8(mail_exclusion_);
            if (value.find_first_not_of(" \t") == std::string::npos) return;
            act("mail", "mail.exclusions.add", {{"kind", kind}, {"value", value}}, [this](const json&) {
                mail_exclusion_.clear();
                load_mail();
            });
        })->enabled = busy_.empty();
    }
    error_line(c, "mail");
}

}  // namespace wb::shell
