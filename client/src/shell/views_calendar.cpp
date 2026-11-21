// add-local-outlook-and-teams in the window: the Calendar page (client/calendar-view), the Teams chats and Daily report
// settings sections (client/teams-chats, client/daily-report), and the send form of a prepared draft (client/agenda
// Sending A Prepared Draft). The host does the work (calendar.view, .ask, .open_in_outlook; teams.*; daily_report.*;
// agenda.draft, .send_draft, .open_draft_in_outlook).
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

#include <ctime>

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

constexpr long long kDayMs = 86'400'000;

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

std::tm local_tm(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm lt{};
    localtime_s(&lt, &t);
    return lt;
}

// Local midnight of the day `days` after the day of `ms`.
long long local_midnight(long long ms, int days = 0) {
    std::tm lt = local_tm(ms);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_mday += days;
    lt.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&lt)) * 1000;
}

std::u16string hhmm(long long ms) {
    const std::tm lt = local_tm(ms);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return u16(buf);
}

std::u16string day_label(Shell& s, long long ms) {
    const std::tm lt = local_tm(ms);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d. %d.", lt.tm_mday, lt.tm_mon + 1);
    return s.t(std::string("calendar.weekday.") + std::to_string(lt.tm_wday)) + u" " + u16(buf);
}

std::vector<std::string> split_addresses(const std::u16string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : ui::to_utf8(s)) {
        if (c == ',' || c == ';' || c == ' ' || c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::u16string join_addresses(const json& list) {
    std::u16string out;
    if (list.is_array())
        for (const auto& a : list)
            if (a.is_string()) out += (out.empty() ? u"" : u"; ") + u16(a.get<std::string>());
    return out;
}

}  // namespace

// ---- Calendar page ----------------------------------------------------------------------------------------------------

void PagePane::load_calendar() {
    const long long now = shell_.now();
    long long from = 0, to = 0;
    if (calendar_mode_ == "day") {
        from = local_midnight(now, static_cast<int>(calendar_offset_));
        to = local_midnight(now, static_cast<int>(calendar_offset_) + 1);
    } else if (calendar_mode_ == "list") {
        from = local_midnight(now);
        to = local_midnight(now, 14);
    } else {
        const int wday = local_tm(now).tm_wday;  // 0 = Sunday
        const int to_monday = wday == 0 ? -6 : 1 - wday;
        from = local_midnight(now, to_monday + static_cast<int>(calendar_offset_) * 7);
        to = local_midnight(now, to_monday + static_cast<int>(calendar_offset_) * 7 + 5);
    }
    std::weak_ptr<int> alive = alive_;
    shell_.call("calendar.view", {{"from", from}, {"to", to}}, [this, alive, from, to](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            calendar_ = *res;
            calendar_["from"] = from;
            calendar_["to"] = to;
            errors_.erase("calendar");
        } else {
            errors_["calendar"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::build_calendar() {
    auto* bar = emplace<Node>("calendar-toolbar");
    bar->axis = ui::Axis::row;
    bar->gap = 8;
    const auto mode = [this](const char* m) {
        return [this, m] {
            calendar_mode_ = m;
            calendar_offset_ = 0;
            load_calendar();
        };
    };
    for (const char* m : {"day", "week", "list"})
        button(*bar, std::string("calendar-mode-") + m, shell_.t(std::string("calendar.mode.") + m), mode(m),
               calendar_mode_ == m ? ui::Button::Kind::primary : ui::Button::Kind::subtle);
    if (calendar_mode_ != "list") {
        button(*bar, "calendar-previous", shell_.t("calendar.previous"), [this] {
            --calendar_offset_;
            load_calendar();
        }, ui::Button::Kind::subtle);
        button(*bar, "calendar-today", shell_.t("calendar.today"), [this] {
            calendar_offset_ = 0;
            load_calendar();
        }, ui::Button::Kind::subtle);
        button(*bar, "calendar-next", shell_.t("calendar.next"), [this] {
            ++calendar_offset_;
            load_calendar();
        }, ui::Button::Kind::subtle);
    }
    if (!calendar_.is_object()) {
        if (!errors_.contains("calendar")) text(*this, shell_.t("common.loading"), Paint::text_secondary);
        error_line(*this, "calendar");
        return;
    }
    if (!jbool(calendar_, "available")) {
        text(*this, shell_.t("calendar.unavailable"), Paint::text_secondary, "calendar-unavailable");
        button(*this, "calendar-open-settings", shell_.t("calendar.open_settings"), [this] { shell_.navigate("#/settings"); });
        return;
    }
    // Offline: the local copy, marked with the last sync (Calendar Page).
    const std::string last = jstr(calendar_, "last_sync");
    const bool offline = jstr(calendar_, "state") != "connected";
    text(*this, last.empty() ? shell_.t("calendar.never_synced") : shell_.t(offline ? "calendar.offline_since" : "calendar.synced", {{"time", u16(last)}}),
         offline ? Paint::warning : Paint::text_secondary, "calendar-sync");

    const long long from = jint(calendar_, "from"), to = jint(calendar_, "to"), now = shell_.now();
    const json events = calendar_.value("events", json::array());
    const auto event_button = [this](Node& parent, const json& e) {
        const std::string id = jstr(e, "id");
        const bool busy = e.contains("withheld") || jbool(e, "private");
        const std::u16string when = jbool(e, "all_day") ? shell_.t("calendar.all_day") : hhmm(jint(e, "start_ms")) + u"–" + hhmm(jint(e, "end_ms"));
        const std::u16string title = busy ? shell_.t("calendar.busy") : u16(jstr(e, "subject").empty() ? "(no subject)" : jstr(e, "subject"));
        std::u16string label = when + u"  " + title;
        if (!busy && !jstr(e, "location").empty()) label += u" · " + u16(jstr(e, "location"));
        if (jbool(e, "cancelled")) label = shell_.t("calendar.cancelled") + u" " + label;
        auto* b = button(parent, "calendar-event-" + id, label, [this, id] {
            calendar_open_ = calendar_open_ == id ? std::string{} : id;
            rebuild();
        }, calendar_open_ == id ? ui::Button::Kind::primary : ui::Button::Kind::standard);
        b->name = label;
        b->enabled = !busy;
    };
    const auto day_events = [&](long long d0, long long d1) {
        std::vector<json> out;
        for (const auto& e : events)
            if (jint(e, "end_ms") > d0 && jint(e, "start_ms") < d1) out.push_back(e);
        std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) {
            if (jbool(a, "all_day") != jbool(b, "all_day")) return jbool(a, "all_day");
            return jint(a, "start_ms") < jint(b, "start_ms");
        });
        return out;
    };

    if (calendar_mode_ == "list") {
        auto* list = emplace<Node>("calendar-list");
        list->role = ui::Role::list;
        list->gap = 6;
        bool any = false;
        for (long long d = from; d < to; d = local_midnight(d, 1)) {
            const auto evs = day_events(d, local_midnight(d, 1));
            if (evs.empty()) continue;
            any = true;
            list->emplace<ui::Label>(shell_.ui(), day_label(shell_, d), TextStyle::body_strong)->role = ui::Role::heading;
            for (const auto& e : evs) event_button(*list, e);
        }
        if (!any) text(*list, shell_.t("calendar.empty"), Paint::text_secondary, "calendar-empty");
    } else {
        auto* grid = emplace<Node>("calendar-grid");
        grid->axis = ui::Axis::row;
        grid->gap = 8;
        for (long long d = from; d < to; d = local_midnight(d, 1)) {
            const long long d1 = local_midnight(d, 1);
            const bool today = now >= d && now < d1;
            auto* col = grid->emplace<ui::Box>("calendar-day-" + std::to_string(d), today ? Paint::surface_alt : Paint::surface, 6, Paint::border);
            col->role = ui::Role::group;
            col->name = day_label(shell_, d);
            col->padding = ui::Insets::all(8);
            col->gap = 6;
            col->emplace<ui::Label>(shell_.ui(), day_label(shell_, d), TextStyle::body_strong, today ? Paint::accent : Paint::text)->role = ui::Role::heading;
            bool marked = !today;
            for (const auto& e : day_events(d, d1)) {
                if (!marked && !jbool(e, "all_day") && jint(e, "start_ms") > now) {
                    text(*col, shell_.t("calendar.now", {{"time", hhmm(now)}}), Paint::accent, "calendar-now");
                    marked = true;
                }
                event_button(*col, e);
            }
            if (!marked) text(*col, shell_.t("calendar.now", {{"time", hhmm(now)}}), Paint::accent, "calendar-now");
        }
    }

    // The chosen event's details and actions (Calendar Event Actions).
    for (const auto& e : events) {
        if (jstr(e, "id") != calendar_open_) continue;
        auto* d = emplace<ui::Box>("calendar-details", Paint::surface, 8, Paint::border);
        d->role = ui::Role::group;
        d->name = u16(jstr(e, "subject"));
        d->padding = ui::Insets::all(16);
        d->gap = 6;
        d->emplace<ui::Label>(shell_.ui(), u16(jstr(e, "subject")), TextStyle::subtitle)->role = ui::Role::heading;
        row(*d, "calendar-details-when", shell_.t("calendar.when"),
            day_label(shell_, jint(e, "start_ms")) + u" " + hhmm(jint(e, "start_ms")) + u"–" + hhmm(jint(e, "end_ms")));
        if (!jstr(e, "location").empty()) row(*d, "calendar-details-where", shell_.t("calendar.where"), u16(jstr(e, "location")));
        if (const json* o = jobj(e, "organizer"); o && !jstr(*o, "address").empty())
            row(*d, "calendar-details-organizer", shell_.t("calendar.organizer"), u16(jstr(*o, "name").empty() ? jstr(*o, "address") : jstr(*o, "name")));
        std::u16string people;
        for (const auto& a : e.value("attendees", json::array()))
            people += (people.empty() ? u"" : u", ") + u16(jstr(a, "name").empty() ? jstr(a, "address") : jstr(a, "name"));
        if (!people.empty()) row(*d, "calendar-details-attendees", shell_.t("calendar.attendees"), people);
        if (const std::string body = jstr(e, "body"); !body.empty()) text(*d, u16(body.size() > 1500 ? body.substr(0, 1500) + "…" : body), Paint::text_secondary, "calendar-details-body");
        auto* actions = d->emplace<Node>();
        actions->axis = ui::Axis::row;
        actions->gap = 8;
        const std::string id = jstr(e, "id");
        if (const std::string url = jstr(e, "online_meeting_url"); url.starts_with("https://"))
            button(*actions, "calendar-join", shell_.t("calendar.join"), [this, url] {
                if (shell_.ui().open_link) shell_.ui().open_link(u16(url));
            }, ui::Button::Kind::primary);
        button(*actions, "calendar-ask", shell_.t("calendar.ask"), [this, id] {
            act("calendar", "calendar.ask", {{"id", id}}, [this](const json& r) {
                if (const std::string t = jstr(r, "thread_id"); !t.empty()) shell_.navigate(thread_hash(t));
            });
        })->enabled = busy_.empty() && shell_.runs_blocked().empty();
        if (jstr(calendar_, "backend") == "outlook")
            button(*actions, "calendar-open-outlook", shell_.t("calendar.open_in_outlook"), [this, id] {
                act("calendar", "calendar.open_in_outlook", {{"id", id}});
            }, ui::Button::Kind::subtle)->enabled = busy_.empty();
    }
    error_line(*this, "calendar");
}

// ---- Teams chats section --------------------------------------------------------------------------------------------

void PagePane::load_teams() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("teams.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            teams_ = *res;
            errors_.erase("teams");
        } else {
            errors_["teams"] = u16(res.error().message);
        }
        rebuild();
    });
    shell_.call("teams.conversations", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        teams_conversations_ = res ? *res : json(nullptr);
        rebuild();
    });
}

void PagePane::teams_changed() {
    if (shell_.route().name == RouteName::settings) load_teams();
}

void PagePane::build_teams(Node& c) {
    text(c, shell_.t("settings.teams.body"), Paint::text_secondary);
    if (!teams_.is_object()) {
        if (!errors_.contains("teams")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "teams");
        return;
    }
    if (!jbool(teams_, "available")) {
        text(c, shell_.t("settings.teams.off_by_policy"), Paint::warning, "settings-teams-policy");
        return;
    }
    const bool on = jbool(teams_, "enabled");
    setting_switch(c, "settings-teams-enabled", shell_.t("settings.teams.enabled"), on, false, [this](bool v) {
        act("teams", "teams.configure", {{"enabled", v}}, [this](const json&) {
            shell_.announce(shell_.t("settings.teams.saved"));
            load_teams();
        });
    });
    const json profiles = teams_.value("profiles", json::array());
    if (profiles.empty()) text(c, shell_.t("settings.teams.no_profile"), Paint::text_secondary, "settings-teams-no-profile");
    if (profiles.size() > 1) {
        std::vector<std::pair<std::string, std::u16string>> opts;
        for (const auto& p : profiles) {
            const std::string path = p.get<std::string>();
            const size_t ebw = path.find("WV2Profile_");
            opts.emplace_back(path, u16(ebw == std::string::npos ? path : path.substr(ebw, path.find('\\', ebw) - ebw)));
        }
        std::u16string current = opts.front().second;
        for (const auto& [k, v] : opts)
            if (k == jstr(teams_, "profile")) current = v;
        choice(c, "settings-teams-profile", shell_.t("settings.teams.profile"), current, opts, [this](const std::string& v) {
            act("teams", "teams.configure", {{"profile", v}}, [this](const json&) { load_teams(); });
        }, busy_.empty());
    }
    if (!on) {
        error_line(c, "teams");
        return;
    }
    const std::string last = jstr(teams_, "last_read");
    row(c, "settings-teams-last-read", shell_.t("settings.teams.last_read"), last.empty() ? shell_.t("settings.mail.never") : u16(last));
    row(c, "settings-teams-counts", shell_.t("settings.teams.local_copy"),
        shell_.t("settings.teams.counts", {{"conversations", u16(std::to_string(jint(teams_, "conversations")))},
                                           {"messages", u16(std::to_string(jint(teams_, "messages")))}}));
    if (const std::string e = jstr(teams_, "error"); !e.empty()) text(c, shell_.t("settings.teams.problem", {{"reason", u16(e)}}), Paint::warning, "settings-teams-error");
    if (const json* n = jobj(teams_, "counts"); n && (jint(*n, "skipped") > 0 || jint(*n, "decode_errors") > 0))
        text(c, shell_.t("settings.teams.skipped", {{"skipped", u16(std::to_string(jint(*n, "skipped") + jint(*n, "decode_errors")))}}),
             Paint::text_secondary, "settings-teams-skipped");
    auto* actions = c.emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    button(*actions, "settings-teams-sync", shell_.t("settings.teams.read_now"), [this] {
        act("teams", "teams.sync", json::object(), [this](const json&) { shell_.announce(shell_.t("settings.teams.read_started")); });
    })->enabled = busy_.empty();
    button(*actions, "settings-teams-forget", shell_.t("settings.teams.forget"), [this] {
        ask_forget("teams", shell_.t("settings.teams.forget_what"), "teams.forget", json::object(), [this] { load_teams(); });
    }, ui::Button::Kind::subtle)->enabled = busy_.empty();

    // Teams Exclusions: the user's excluded conversations.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.teams.exclusions"), TextStyle::body_strong)->role = ui::Role::heading;
    const json excluded = teams_.value("excluded", json::array());
    auto* list = c.emplace<Node>("settings-teams-conversations");
    list->role = ui::Role::list;
    list->gap = 4;
    size_t shown = 0;
    if (teams_conversations_.is_object())
        for (const auto& conv : teams_conversations_.value("conversations", json::array())) {
            if (++shown > 40) break;
            const std::string id = jstr(conv, "id");
            const bool ex = std::find(excluded.begin(), excluded.end(), json(id)) != excluded.end();
            setting_switch(*list, "settings-teams-conv-" + std::to_string(shown),
                           u16(jstr(conv, "name")) + u" (" + shell_.t("settings.teams.type." + jstr(conv, "type", "group")) + u")", !ex, false,
                           [this, id](bool include) {
                               act("teams", "teams.exclude", {{"conversation_id", id}, {"excluded", !include}}, [this](const json&) { load_teams(); });
                           });
        }
    for (const auto& id : excluded)
        if (id.is_string())
            button(*list, "settings-teams-include-" + id.get<std::string>().substr(0, 24), shell_.t("settings.teams.include_again"),
                   [this, cid = id.get<std::string>()] {
                       act("teams", "teams.exclude", {{"conversation_id", cid}, {"excluded", false}}, [this](const json&) { load_teams(); });
                   }, ui::Button::Kind::subtle);
    error_line(c, "teams");
}

// ---- Daily report section -------------------------------------------------------------------------------------------

void PagePane::load_daily_report() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("daily_report.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            report_ = *res;
            errors_.erase("daily_report");
        } else {
            errors_["daily_report"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::daily_report_changed() {
    if (shell_.route().name == RouteName::settings) load_daily_report();
}

void PagePane::build_daily_report(Node& c) {
    text(c, shell_.t("settings.daily_report.body"), Paint::text_secondary);
    if (!report_.is_object()) {
        if (!errors_.contains("daily_report")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "daily_report");
        return;
    }
    if (!jbool(report_, "available")) {
        text(c, shell_.t("settings.daily_report.off_by_policy"), Paint::warning, "settings-report-policy");
        return;
    }
    const auto configure = [this](const json& patch) {
        act("daily_report", "daily_report.configure", patch, [this](const json&) {
            shell_.announce(shell_.t("settings.daily_report.saved"));
            load_daily_report();
        });
    };
    const bool on = jbool(report_, "enabled");
    setting_switch(c, "settings-report-enabled", shell_.t("settings.daily_report.enabled"), on, false,
                   [configure](bool v) { configure({{"enabled", v}}); });
    if (!on) {
        error_line(c, "daily_report");
        return;
    }
    if (report_time_.empty()) report_time_ = u16(jstr(report_, "time", "08:00"));
    auto* time = c.emplace<ui::TextField>(shell_.ui(), "settings-report-time", shell_.t("settings.daily_report.time"));
    time->set_text(report_time_);
    ui::TextField* tp = time;
    time->on_change = [this, tp] { report_time_ = tp->text(); };
    button(c, "settings-report-time-save", shell_.t("settings.daily_report.save_time"), [this, configure] {
        configure({{"time", ui::to_utf8(report_time_)}});
    })->enabled = busy_.empty();
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.daily_report.days"), TextStyle::body_strong)->role = ui::Role::heading;
    const json days = report_.value("days", json::array());
    for (int d : {1, 2, 3, 4, 5, 6, 0}) {
        const bool has = std::find(days.begin(), days.end(), json(d)) != days.end();
        setting_switch(c, "settings-report-day-" + std::to_string(d), shell_.t("calendar.weekday." + std::to_string(d)), has, false,
                       [configure, days, d](bool v) {
                           json next = json::array();
                           for (const auto& x : days)
                               if (x != json(d)) next.push_back(x);
                           if (v) next.push_back(d);
                           configure({{"days", next}});
                       });
    }
    if (const std::string last = jstr(report_, "last_day"); !last.empty())
        row(c, "settings-report-last", shell_.t("settings.daily_report.last"), u16(last));
    auto* actions = c.emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    button(*actions, "settings-report-now", shell_.t("settings.daily_report.write_now"), [this] {
        act("daily_report", "daily_report.write_now", json::object(), [this](const json& r) {
            if (const std::string t = jstr(r, "thread_id"); !t.empty()) shell_.navigate(thread_hash(t));
        });
    })->enabled = busy_.empty() && shell_.runs_blocked().empty();
    if (const std::string t = jstr(report_, "last_thread"); !t.empty())
        button(*actions, "settings-report-open", shell_.t("settings.daily_report.open_last"), [this, t] { shell_.navigate(thread_hash(t)); },
               ui::Button::Kind::subtle);
    error_line(c, "daily_report");
}

// ---- The send form of a prepared draft ------------------------------------------------------------------------------

void PagePane::open_draft(const std::string& item_id) {
    if (draft_item_ == item_id) {
        draft_item_.clear();
        draft_ = nullptr;
        rebuild();
        return;
    }
    std::weak_ptr<int> alive = alive_;
    act("agenda", "agenda.draft", {{"id", item_id}}, [this, alive, item_id](const json& r) {
        if (!alive.lock()) return;
        draft_item_ = item_id;
        draft_ = r;
        draft_to_ = join_addresses(r.value("to", json::array()));
        draft_cc_ = join_addresses(r.value("cc", json::array()));
        draft_subject_ = u16(jstr(r, "subject"));
        draft_body_ = u16(jstr(r, "body"));
        rebuild();
    });
}

void PagePane::build_draft(Node& parent) {
    if (!draft_.is_object()) return;
    auto* f = parent.emplace<ui::Box>("agenda-draft-form", Paint::surface_alt, 6, Paint::border);
    f->role = ui::Role::group;
    f->name = shell_.t("agenda.draft.title");
    f->padding = ui::Insets::all(12);
    f->gap = 6;
    f->emplace<ui::Label>(shell_.ui(), shell_.t("agenda.draft.title"), TextStyle::body_strong)->role = ui::Role::heading;
    if (draft_body_.empty()) text(*f, shell_.t("agenda.draft.no_draft"), Paint::text_secondary, "agenda-draft-empty");
    if (jstr(draft_, "source") == "teams") text(*f, shell_.t("agenda.draft.teams_note"), Paint::text_secondary, "agenda-draft-teams");
    const auto field = [this, f](const std::string& id, const std::u16string& label, std::u16string* value, bool multi) {
        ui::TextField::Options o;
        o.multiline = multi;
        o.min_lines = multi ? 6 : 1;
        o.max_lines = multi ? 20 : 1;
        auto* t = f->emplace<ui::TextField>(shell_.ui(), id, label, o);
        t->set_text(*value);
        ui::TextField* tp = t;
        t->on_change = [tp, value] { *value = tp->text(); };
    };
    field("agenda-draft-to", shell_.t("agenda.draft.to"), &draft_to_, false);
    field("agenda-draft-cc", shell_.t("agenda.draft.cc"), &draft_cc_, false);
    field("agenda-draft-subject", shell_.t("agenda.draft.subject"), &draft_subject_, false);
    field("agenda-draft-body", shell_.t("agenda.draft.body"), &draft_body_, true);
    const auto params = [this] {
        return json{{"to", split_addresses(draft_to_)}, {"cc", split_addresses(draft_cc_)}, {"subject", ui::to_utf8(draft_subject_)},
                    {"body", ui::to_utf8(draft_body_)}, {"reply_to", jstr(draft_, "reply_to")}};
    };
    auto* actions = f->emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    // Send: the whole message is on screen, so this press is the user's approval of exactly this message.
    auto* send = button(*actions, "agenda-draft-send", shell_.t("agenda.draft.send"), [this, params] {
        act("agenda", "agenda.send_draft", params(), [this](const json& r) {
            // An unconfirmed send closes the form too, with the host's note, so it is not pressed again.
            shell_.announce(jbool(r, "unknown") ? ui::to_utf16(jstr(r, "message")) : shell_.t("agenda.draft.sent"));
            draft_ = nullptr;
            draft_item_.clear();
            load_agenda();
        });
    }, ui::Button::Kind::primary);
    // The fields change without a rebuild, so an empty To or body is refused by the host (agenda.send_draft) and shown
    // as the error line, not decided here once at build time.
    send->enabled = busy_.empty() && jbool(draft_, "can_send");
    if (jbool(draft_, "outlook"))
        button(*actions, "agenda-draft-outlook", shell_.t("agenda.draft.open_in_outlook"), [this, params] {
            act("agenda", "agenda.open_draft_in_outlook", params(), [this](const json&) { shell_.announce(shell_.t("agenda.draft.opened")); });
        })->enabled = busy_.empty();
    button(*actions, "agenda-draft-cancel", shell_.t("agenda.draft.cancel"), [this] {
        draft_ = nullptr;
        draft_item_.clear();
        rebuild();
    }, ui::Button::Kind::subtle);
}

}  // namespace wb::shell
