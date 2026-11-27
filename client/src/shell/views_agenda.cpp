// The Agenda view (client/work-inbox Agenda View) and the Agenda settings section (client/agenda Agenda Is Opt In,
// Agenda Data Deletion). Open Agenda items are grouped as Overdue, Today, Tomorrow, This week and Later; each row offers
// Prepare or Open preparation, Open source and Mark done, with Dismiss, Edit date and Rename in the card's menu
// (Agenda View Layout). The host does the work
// (agenda.list, .done, .dismiss, .edit, .prepare, .status, .set, .delete_all).
#include "shell/feed.h"
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

constexpr long long kDayMs = 86'400'000;

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

long long local_day(long long ms, long long tz_min) {
    const long long local = ms + tz_min * 60'000;
    return local >= 0 ? local / kDayMs : (local - kDayMs + 1) / kDayMs;
}

int weekday(long long day) { return static_cast<int>(((day + 3) % 7 + 7) % 7); }  // 0 = Monday

// The group of an open item: overdue, today, tomorrow, week (the rest of this week) or later.
const char* group_of(long long due, bool has_time, long long now, long long tz) {
    const long long d = local_day(due, tz), today = local_day(now, tz);
    if (d < today || (has_time && due < now)) return "overdue";
    if (d == today) return "today";
    if (d == today + 1) return "tomorrow";
    if (d - today <= 6 - weekday(today)) return "week";
    return "later";
}

}  // namespace

void PagePane::load_agenda() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("agenda.list", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            agenda_ = *res;
            errors_.erase("agenda");
        } else {
            errors_["agenda"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::load_agenda_status() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("agenda.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            agenda_status_ = *res;
            errors_.erase("agenda-settings");
        } else {
            errors_["agenda-settings"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::agenda_changed() {
    if (shell_.route().name == RouteName::agenda) load_agenda();
    else if (shell_.route().name == RouteName::settings) load_agenda_status();
}

void PagePane::build_agenda() {
    text(*this, shell_.t("agenda.body"), Paint::text_secondary, "agenda-body");
    if (!agenda_.is_object()) {
        if (!errors_.contains("agenda")) text(*this, shell_.t("common.loading"), Paint::text_secondary);
        error_line(*this, "agenda");
        return;
    }
    const json st = agenda_.value("status", json::object());
    if (!jbool(st, "enabled")) {
        text(*this, shell_.t("agenda.disabled"), Paint::text_secondary, "agenda-disabled");
        button(*this, "agenda-open-settings", shell_.t("agenda.open_settings"), [this] { shell_.navigate("#/settings/agenda"); });
        error_line(*this, "agenda");
        return;
    }
    const long long now = jint(agenda_, "now", shell_.now());
    const long long tz = jint(agenda_, "tz_offset_min");
    const json* items = jobj(agenda_, "items");
    if (const long long pending = jint(st, "pending"); pending > 0)
        emplace<ui::Label>(shell_.ui(), shell_.count("agenda.pending", pending), TextStyle::caption, Paint::text_secondary, true, "agenda-pending");
    if (!items || items->empty()) {
        text(*this, shell_.t("agenda.empty"), Paint::text_secondary, "agenda-empty");
        error_line(*this, "agenda");
        return;
    }
    for (const char* group : {"overdue", "today", "tomorrow", "week", "later"}) {
        std::vector<const json*> rows;
        for (const auto& it : *items)
            if (std::string(group) == group_of(jint(it, "due_at"), jbool(it, "has_time"), now, tz)) rows.push_back(&it);
        if (rows.empty()) continue;
        const bool overdue = std::string(group) == "overdue";
        // Agenda View Layout (improve-threads-calendar-agenda D5): the group heading with its count, then one card per item.
        auto* head = emplace<Node>(std::string("agenda-group-head-") + group);
        head->axis = ui::Axis::row;
        head->gap = 8;
        head->padding.top = 12;
        auto* h = head->emplace<ui::Label>(shell_.ui(), shell_.t(std::string("agenda.group.") + group), TextStyle::subtitle,
                                           overdue ? Paint::warning : Paint::text, false, std::string("agenda-group-") + group);
        h->role = ui::Role::heading;
        auto* count = head->emplace<ui::Badge>(shell_.ui(), u16(std::to_string(rows.size())), Paint::surface_alt, Paint::text_secondary);
        count->padding.top = 3;
        auto* list = emplace<Node>(std::string("agenda-list-") + group);
        list->role = ui::Role::list;
        list->name = shell_.t(std::string("agenda.group.") + group);
        list->gap = 8;
        for (const json* row : rows) {
            const json& it = *row;
            const std::string id = jstr(it, "id");
            const std::u16string title = u16(jstr(it, "title"));
            const bool timed = jbool(it, "has_time");
            const std::string local = iso_from_millis(jint(it, "due_at") + tz * 60'000);
            // A readable date (Agenda View Layout): the weekday and day, the year only when it is not this year.
            const long long day = local_day(jint(it, "due_at"), tz);
            const int y = std::stoi(local.substr(0, 4)), mo = std::stoi(local.substr(5, 2)), dd = std::stoi(local.substr(8, 2));
            std::string date = std::to_string(dd) + ". " + std::to_string(mo) + ".";
            if (local.substr(0, 4) != iso_from_millis(now + tz * 60'000).substr(0, 4)) date += " " + std::to_string(y);
            const std::u16string due = shell_.t("calendar.weekday." + std::to_string((weekday(day) + 1) % 7)) + u" " + u16(date) +
                                       (timed ? u" " + u16(local.substr(11, 5)) : std::u16string());
            auto* item = list->emplace<ui::Box>("agenda-item-" + id, Paint::surface, 8, overdue ? Paint::warning : Paint::border);
            item->role = ui::Role::list_item;
            item->padding = ui::Insets::all(12);
            item->gap = 6;
            item->name = shell_.t("agenda.item_name", {{"title", title}, {"due", due}});

            // Title and due date.
            auto* top = item->emplace<Node>();
            top->axis = ui::Axis::row;
            top->gap = 12;
            auto* t = top->emplace<ui::Label>(shell_.ui(), title, TextStyle::body_strong, Paint::text, true, "agenda-title-" + id);
            t->role = ui::Role::none;
            std::u16string due_text = shell_.t(jbool(it, "inferred") ? "agenda.due_inferred" : "agenda.due", {{"due", due}});
            auto* chip = top->emplace<ui::Badge>(shell_.ui(), due_text, overdue ? Paint::warning_background : Paint::surface_alt,
                                                 overdue ? Paint::warning : Paint::text_secondary, "agenda-meta-" + id);
            chip->name = due_text;
            chip->role = ui::Role::text;

            // What to do.
            if (const std::string action = jstr(it, "action"); !action.empty())
                text(*item, u16(action), Paint::text_secondary, "agenda-action-" + id);

            // Sources and preparation as badges, the notice and the quote.
            std::string link;
            auto* tags = item->emplace<Node>("agenda-tags-" + id);
            tags->axis = ui::Axis::row;
            tags->gap = 6;
            tags->wraps = true;
            std::vector<std::u16string> kinds;
            for (const auto& s : it.value("sources", json::array())) {
                const std::u16string k = shell_.t("agenda.source." + jstr(s, "kind"));
                if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) kinds.push_back(k);
                if (link.empty()) link = jstr(s, "link");
            }
            for (const auto& k : kinds) tags->emplace<ui::Badge>(shell_.ui(), k, Paint::surface_alt, Paint::text_secondary);
            const bool prepared = !jstr(it, "prep_thread_id").empty();
            if (prepared) tags->emplace<ui::Badge>(shell_.ui(), shell_.t("agenda.prepared"), Paint::accent_surface, Paint::text);
            if (const std::string notice = jstr(it, "notice"); !notice.empty())
                text(*item, shell_.t("agenda.notice." + notice), Paint::warning, "agenda-notice-" + id);
            for (const auto& s : it.value("sources", json::array()))
                if (const std::string q = jstr(s, "quote"); !q.empty()) {
                    auto* quote = item->emplace<ui::Label>(shell_.ui(), u"“" + u16(q) + u"”", TextStyle::caption, Paint::text_secondary, false,
                                                           "agenda-quote-" + id);
                    quote->help = u16(q);  // the whole quote in the tooltip; the line is cut
                    break;
                }

            // Actions: the main one, Open source and Mark done; the rest in the card's menu.
            auto* actions = item->emplace<Node>();
            actions->axis = ui::Axis::row;
            actions->gap = 8;
            actions->wraps = true;
            if (prepared) {
                const std::string thread = jstr(it, "prep_thread_id");
                auto* open = button(*actions, "agenda-open-prep-" + id, shell_.t("agenda.open_preparation"),
                                    [this, thread] { shell_.navigate(thread_hash(thread)); }, ui::Button::Kind::primary);
                open->name = shell_.t("agenda.open_preparation_named", {{"title", title}});
                // Sending A Prepared Draft (add-local-outlook-and-teams design D9): a reply drafted from a mail or Teams source.
                bool replyable = jstr(it, "draft_kind") == "reply";
                if (replyable) {
                    replyable = false;
                    for (const auto& s : it.value("sources", json::array()))
                        if (jstr(s, "kind") == "mail" || jstr(s, "kind") == "teams") replyable = true;
                }
                if (replyable) {
                    auto* d = button(*actions, "agenda-draft-" + id, shell_.t(draft_item_ == id ? "agenda.draft.close" : "agenda.draft.open"),
                                     [this, id] { open_draft(id); });
                    d->name = shell_.t("agenda.draft.open_named", {{"title", title}});
                    d->enabled = busy_.empty();
                }
            } else {
                auto* prep = button(*actions, "agenda-prepare-" + id, shell_.t("agenda.prepare"), [this, id] {
                    act("agenda", "agenda.prepare", {{"id", id}}, [this](const json& r) {
                        if (const std::string thread = jstr(r, "prep_thread_id"); !thread.empty()) shell_.navigate(thread_hash(thread));
                    });
                }, ui::Button::Kind::primary);
                prep->name = shell_.t("agenda.prepare_named", {{"title", title}});
                prep->enabled = busy_.empty() && shell_.runs_blocked().empty();
            }
            if (!link.empty()) {
                auto* src = button(*actions, "agenda-source-" + id, shell_.t("agenda.open_source"), [this, link] {
                    if (link.starts_with("#/")) shell_.navigate(link);
                    else if ((link.starts_with("https://") || link.starts_with("http://")) && shell_.ui().open_link) shell_.ui().open_link(u16(link));
                });
                src->name = shell_.t("agenda.open_source_named", {{"title", title}});
            }
            auto* done = button(*actions, "agenda-done-" + id, shell_.t("agenda.done"), [this, id, title] {
                act("agenda", "agenda.done", {{"id", id}}, [this, title](const json&) {
                    shell_.announce(shell_.t("agenda.done_announce", {{"title", title}}));
                    load_agenda();
                });
            });
            done->name = shell_.t("agenda.done_named", {{"title", title}});
            done->enabled = busy_.empty();
            const std::u16string initial = u16(timed ? local.substr(0, 16) : local.substr(0, 10));
            auto* more = actions->emplace<ui::IconButton>(shell_.ui(), "agenda-more-" + id, ui::icon::more,
                                                          shell_.t("agenda.more_named", {{"title", title}}));
            more->enabled = busy_.empty();
            ui::IconButton* mp = more;
            more->on_click = [this, id, title, initial, mp] {
                auto menu = std::make_unique<ui::Menu>("agenda-menu-" + id, shell_.t("agenda.more_named", {{"title", title}}));
                menu->add_item(shell_.ui(), "agenda-dismiss-" + id, shell_.t("agenda.dismiss"), [this, id, title] {
                    act("agenda", "agenda.dismiss", {{"id", id}}, [this, title](const json&) {
                        shell_.announce(shell_.t("agenda.dismissed", {{"title", title}}));
                        load_agenda();
                    });
                });
                menu->add_item(shell_.ui(), "agenda-date-" + id, shell_.t("agenda.edit_date"), [this, id, title, initial] {
                    shell_.ask_text_value("agenda-date-dialog", shell_.t("agenda.edit_date_title", {{"title", title}}), shell_.t("agenda.edit_date_label"),
                                          initial, [this, id](const std::string& v) {
                                              act("agenda", "agenda.edit", {{"id", id}, {"patch", {{"due", v}}}}, [this](const json&) { load_agenda(); });
                                          });
                });
                menu->add_item(shell_.ui(), "agenda-rename-" + id, shell_.t("agenda.rename"), [this, id, title] {
                    shell_.ask_text_value("agenda-rename-dialog", shell_.t("agenda.rename_title"), shell_.t("agenda.rename_label"), title,
                                          [this, id](const std::string& v) {
                                              act("agenda", "agenda.edit", {{"id", id}, {"patch", {{"title", v}}}}, [this](const json&) { load_agenda(); });
                                          });
                });
                shell_.window().overlay().show(std::move(menu), mp->frame(), true);
                shell_.window().render();
            };
            if (draft_item_ == id) build_draft(*item);
        }
    }
    error_line(*this, "agenda");
}

void PagePane::build_agenda_settings(Node& c) {
    text(c, shell_.t("settings.agenda.body"), Paint::text_secondary);
    if (!agenda_status_.is_object()) {
        if (!errors_.contains("agenda-settings")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "agenda-settings");
        return;
    }
    const json& s = agenda_status_;
    if (!jbool(s, "available")) {
        text(c, shell_.t("settings.agenda.off_by_policy"), Paint::text_secondary, "agenda-settings-off");
        error_line(c, "agenda-settings");
        return;
    }
    const auto set = [this](const char* key) {
        return [this, key](bool on) {
            act("agenda-settings", "agenda.set", {{key, on}}, [this](const json& r) {
                agenda_status_ = r;
                shell_.announce(shell_.t("settings.agenda.saved"));
                shell_.refresh_app_status();
            });
        };
    };
    const bool on = jbool(s, "enabled");
    setting_switch(c, "agenda-enabled", shell_.t("settings.agenda.enabled"), on, false, set("enabled"));
    if (on) {
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.agenda.sources"), TextStyle::body_strong)->role = ui::Role::heading;
        for (const char* k : {"mail", "calendar", "meetings", "chats", "teams"})
            setting_switch(c, std::string("agenda-source-") + k, shell_.t(std::string("settings.agenda.source.") + k), jbool(s, k, true), false, set(k));
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.agenda.reminders"), TextStyle::body_strong)->role = ui::Role::heading;
        setting_switch(c, "agenda-remind-day", shell_.t("settings.agenda.remind_day_before"), jbool(s, "remind_day_before", true), false,
                       set("remind_day_before"));
        setting_switch(c, "agenda-remind-hour", shell_.t("settings.agenda.remind_hour_before"), jbool(s, "remind_hour_before", true), false,
                       set("remind_hour_before"));
        // Automatic preparation only where the policy allows it (policy.agenda.auto_prepare).
        setting_switch(c, "agenda-auto-prepare", shell_.t("settings.agenda.auto_prepare"), jbool(s, "auto_prepare"),
                       !jbool(s, "auto_prepare_allowed"), set("auto_prepare"));
        text(c, shell_.t("settings.agenda.diagnostics", {{"pending", u16(std::to_string(jint(s, "pending")))},
                                                         {"dropped", u16(std::to_string(jint(s, "dropped_total")))}}),
             Paint::text_secondary, "agenda-diagnostics");
        if (jbool(s, "daily_limit_reached"))
            text(c, shell_.t("settings.agenda.daily_limit"), Paint::text_secondary, "agenda-daily-limit");
        if (const std::string err = jstr(s, "last_error"); !err.empty())
            text(c, shell_.t("settings.agenda.last_error", {{"reason", u16(err)}}), Paint::warning, "agenda-last-error");
    }
    auto* del = button(c, "agenda-delete-all", shell_.t("settings.agenda.delete_all"), [this] {
        auto d = std::make_unique<ui::Dialog>(shell_.ui(), "agenda-delete-dialog", shell_.t("settings.agenda.delete_title"));
        d->body().emplace<ui::Label>(shell_.ui(), shell_.t("settings.agenda.delete_body"));
        ui::Dialog* dp = d.get();
        d->add_button("agenda-delete-cancel", shell_.t("settings.cancel"), ui::Button::Kind::standard, [dp] { dp->close(); });
        d->add_button("agenda-delete-confirm", shell_.t("settings.agenda.delete_all"), ui::Button::Kind::primary, [this, dp] {
            dp->close();
            act("agenda-settings", "agenda.delete_all", json::object(), [this](const json& r) {
                agenda_status_ = r;
                shell_.announce(shell_.t("settings.agenda.deleted"));
            });
        });
        shell_.window().modal().show(std::move(d));
        shell_.window().render();
    }, ui::Button::Kind::subtle);
    del->enabled = busy_.empty();
    error_line(c, "agenda-settings");
}

}  // namespace wb::shell
