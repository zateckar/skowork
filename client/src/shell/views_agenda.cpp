// The Agenda view (client/work-inbox Agenda View) and the Agenda settings section (client/agenda Agenda Is Opt In,
// Agenda Data Deletion). Open Agenda items are grouped as Overdue, Today, Tomorrow, This week and Later; each row offers
// Prepare or Open preparation, Open source, Mark done, Dismiss, Edit date and Rename. The host does the work
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
        button(*this, "agenda-open-settings", shell_.t("agenda.open_settings"), [this] { shell_.navigate("#/settings"); });
        error_line(*this, "agenda");
        return;
    }
    const long long now = jint(agenda_, "now", shell_.now());
    const long long tz = jint(agenda_, "tz_offset_min");
    const json* items = jobj(agenda_, "items");
    if (const long long pending = jint(st, "pending"); pending > 0)
        text(*this, shell_.count("agenda.pending", pending), Paint::text_secondary, "agenda-pending");
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
        auto* h = emplace<ui::Label>(shell_.ui(), shell_.t(std::string("agenda.group.") + group), TextStyle::body_strong, Paint::text, true,
                                     std::string("agenda-group-") + group);
        h->role = ui::Role::heading;
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
            const std::u16string due = u16(timed ? local.substr(0, 10) + " " + local.substr(11, 5) : local.substr(0, 10));
            auto* item = list->emplace<Node>("agenda-item-" + id);
            item->role = ui::Role::list_item;
            item->gap = 4;
            item->name = shell_.t("agenda.item_name", {{"title", title}, {"due", due}});
            item->emplace<ui::Label>(shell_.ui(), title, TextStyle::body_strong, Paint::text, true)->role = ui::Role::none;
            if (const std::string action = jstr(it, "action"); !action.empty()) text(*item, u16(action), Paint::text_secondary, "agenda-action-" + id);
            std::u16string kinds;
            std::string link;
            for (const auto& s : it.value("sources", json::array())) {
                const std::u16string k = shell_.t("agenda.source." + jstr(s, "kind"));
                if (kinds.find(k) == std::u16string::npos) kinds += (kinds.empty() ? u"" : u", ") + k;
                if (link.empty()) link = jstr(s, "link");
            }
            const bool prepared = !jstr(it, "prep_thread_id").empty();
            std::u16string meta = shell_.t(jbool(it, "inferred") ? "agenda.due_inferred" : "agenda.due", {{"due", due}});
            meta += u" · " + kinds;
            if (prepared) meta += u" · " + shell_.t("agenda.prepared");
            text(*item, meta, Paint::text_secondary, "agenda-meta-" + id);
            if (const std::string notice = jstr(it, "notice"); !notice.empty())
                text(*item, shell_.t("agenda.notice." + notice), Paint::warning, "agenda-notice-" + id);
            for (const auto& s : it.value("sources", json::array()))
                if (const std::string q = jstr(s, "quote"); !q.empty()) {
                    text(*item, u"“" + u16(q) + u"”", Paint::text_secondary, "agenda-quote-" + id);
                    break;
                }
            auto* actions = item->emplace<Node>();
            actions->axis = ui::Axis::row;
            actions->gap = 8;
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
                }, ui::Button::Kind::subtle);
                src->name = shell_.t("agenda.open_source_named", {{"title", title}});
            }
            auto* done = button(*actions, "agenda-done-" + id, shell_.t("agenda.done"), [this, id, title] {
                act("agenda", "agenda.done", {{"id", id}}, [this, title](const json&) {
                    shell_.announce(shell_.t("agenda.done_announce", {{"title", title}}));
                    load_agenda();
                });
            }, ui::Button::Kind::subtle);
            done->name = shell_.t("agenda.done_named", {{"title", title}});
            done->enabled = busy_.empty();
            auto* dismiss = button(*actions, "agenda-dismiss-" + id, shell_.t("agenda.dismiss"), [this, id, title] {
                act("agenda", "agenda.dismiss", {{"id", id}}, [this, title](const json&) {
                    shell_.announce(shell_.t("agenda.dismissed", {{"title", title}}));
                    load_agenda();
                });
            }, ui::Button::Kind::subtle);
            dismiss->name = shell_.t("agenda.dismiss_named", {{"title", title}});
            dismiss->enabled = busy_.empty();
            const std::u16string initial = u16(timed ? local.substr(0, 16) : local.substr(0, 10));
            auto* date = button(*actions, "agenda-date-" + id, shell_.t("agenda.edit_date"), [this, id, title, initial] {
                shell_.ask_text_value("agenda-date-dialog", shell_.t("agenda.edit_date_title", {{"title", title}}), shell_.t("agenda.edit_date_label"),
                                      initial, [this, id](const std::string& v) {
                                          act("agenda", "agenda.edit", {{"id", id}, {"patch", {{"due", v}}}}, [this](const json&) { load_agenda(); });
                                      });
            }, ui::Button::Kind::subtle);
            date->name = shell_.t("agenda.edit_date_named", {{"title", title}});
            date->enabled = busy_.empty();
            auto* rename = button(*actions, "agenda-rename-" + id, shell_.t("agenda.rename"), [this, id, title] {
                shell_.ask_text_value("agenda-rename-dialog", shell_.t("agenda.rename_title"), shell_.t("agenda.rename_label"), title,
                                      [this, id](const std::string& v) {
                                          act("agenda", "agenda.edit", {{"id", id}, {"patch", {{"title", v}}}}, [this](const json&) { load_agenda(); });
                                      });
            }, ui::Button::Kind::subtle);
            rename->name = shell_.t("agenda.rename_named", {{"title", title}});
            rename->enabled = busy_.empty();
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
