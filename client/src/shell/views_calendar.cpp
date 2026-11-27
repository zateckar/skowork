// add-local-outlook-and-teams in the window: the Calendar page (client/calendar-view), the Teams chats and Daily report
// settings sections (client/teams-chats, client/daily-report), and the send form of a prepared draft (client/agenda
// Sending A Prepared Draft). The host does the work (calendar.view, .ask, .open_in_outlook; teams.*; daily_report.*;
// agenda.draft, .send_draft, .open_draft_in_outlook).
#include "shell/calendar_layout.h"
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/canvas.h"
#include "ui/text16.h"

#include <cmath>
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
    std::snprintf(buf, sizeof buf, "%d.%d.", lt.tm_mday, lt.tm_mon + 1);
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

// ---- The time grid (client/calendar-view Calendar Time Grid; improve-threads-calendar-agenda D4) -------------------

constexpr float kGutter = 52;   // the hour scale
constexpr float kHeadH = 30;    // the day names
constexpr float kHourH = 48;    // one hour
constexpr float kAllDayH = 26;  // one all-day row
constexpr float kMinBlock = 20;

// One event: a focusable block that opens its details. Drawn by itself; placed by the grid.
class EventBlock : public Node {
public:
    EventBlock(ui::Ui& ui, std::string id, std::u16string time, std::u16string title, std::u16string place, bool chosen, bool busy,
               bool cancelled, bool all_day)
        : Node(std::move(id)), ui_(ui), time_(std::move(time)), title_(std::move(title)), place_(std::move(place)), chosen_(chosen),
          busy_(busy), cancelled_(cancelled), all_day_(all_day) {
        role = ui::Role::button;
        focusable = true;
        width = ui::Sizing::fixed;
        height = ui::Sizing::fixed;
    }
    std::function<void()> on_click;
    bool can_invoke() const override { return enabled; }
    void invoke() override {
        if (on_click) on_click();
    }
    void on_hover(bool h) override {
        hovered_ = h;
        invalidate();
    }
    void draw(ui::Canvas& c) override {
        const ui::Rect f = {frame().x + 1, frame().y + 1, frame().w - 2, frame().h - 2};
        if (f.empty()) return;
        const Paint fill = chosen_ ? Paint::accent : busy_ ? Paint::surface_alt : Paint::accent_surface;
        c.round(f, 4, fill, ui_.theme.high_contrast ? Paint::text : Paint::none, cancelled_ ? 0.45f : 1.f);
        if (hovered_ && !chosen_) c.round(f, 4, Paint::accent, Paint::none, 0.15f);
        if (!all_day_ && !chosen_) c.fill({f.x, f.y, 3, f.h}, busy_ ? Paint::border : Paint::accent);
        const Paint ink = chosen_ ? Paint::accent_text : cancelled_ ? Paint::text_secondary : Paint::text;
        const Paint ink2 = chosen_ ? Paint::accent_text : Paint::text_secondary;
        c.push_clip(f);
        const float x = f.x + (all_day_ ? 6 : 8), w = std::max(1.f, f.right() - x - 4);
        float y = f.y + 2;
        // Short blocks: time and title on one line; taller ones put the title under the time, then the place.
        if (f.h < 36) {
            IDWriteTextLayout* t = ui_.layout((time_.empty() ? std::u16string() : time_ + u"  ") + title_, TextStyle::caption, w, false);
            DWRITE_TEXT_METRICS m{};
            if (t) t->GetMetrics(&m);
            c.text(t, x, f.y + (f.h - m.height) / 2, ink);
        } else {
            if (!time_.empty()) {
                IDWriteTextLayout* t = ui_.layout(time_, TextStyle::caption, w, false);
                DWRITE_TEXT_METRICS m{};
                if (t) t->GetMetrics(&m);
                c.text(t, x, y, ink2);
                y += m.height;
            }
            IDWriteTextLayout* t = ui_.layout(title_, TextStyle::caption, w, true);
            DWRITE_TEXT_METRICS m{};
            if (t) t->GetMetrics(&m);
            c.text(t, x, y, ink);
            y += m.height;
            if (!place_.empty() && y + 14 < f.bottom()) c.text(ui_.layout(place_, TextStyle::caption, w, false), x, y, ink2);
        }
        c.pop_clip();
    }

private:
    ui::Ui& ui_;
    std::u16string time_, title_, place_;
    bool chosen_, busy_, cancelled_, all_day_;
    bool hovered_ = false;
};

// The day columns with their names, the all-day band, the hour scale and lines, today's shading and the now line.
// Each child EventBlock has a slot: its day, and either an all-day row or minutes and a lane.
class CalendarGrid : public Node {
public:
    struct Slot {
        size_t day = 0;
        bool all_day = false;
        int row = 0;             // all-day row
        int start = 0, end = 0;  // minutes from the day's midnight
        LanePlacement lane;
    };
    CalendarGrid(ui::Ui& ui, std::vector<std::u16string> days, int today, int now_min, int first_hour, int last_hour, int all_day_rows)
        : Node("calendar-grid"), ui_(ui), days_(std::move(days)), today_(today), now_min_(now_min), first_(first_hour), last_(last_hour),
          all_day_rows_(all_day_rows) {
        role = ui::Role::group;
        height = ui::Sizing::fixed;
        fixed_h = grid_top() + static_cast<float>(last_ - first_) * kHourH + 1;
    }
    EventBlock* add_block(std::unique_ptr<EventBlock> b, Slot slot) {
        slots_.push_back(slot);
        return static_cast<EventBlock*>(add(std::move(b)));
    }
    bool arrange_children(ui::Tree& tree, const ui::Rect& in) override {
        const float col = column_w(in);
        for (size_t i = 0; i < children().size() && i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            const float x0 = in.x + kGutter + col * static_cast<float>(s.day);
            ui::Rect r;
            if (s.all_day) {
                r = {x0 + 2, in.y + kHeadH + kAllDayH * static_cast<float>(s.row), col - 4, kAllDayH - 2};
            } else {
                const float lane_w = (col - 8) / static_cast<float>(std::max(1, s.lane.lanes));
                const float top = minute_y(in, s.start), bottom = std::max(top + kMinBlock, minute_y(in, s.end));
                r = {x0 + 2 + lane_w * static_cast<float>(s.lane.lane), top, lane_w, bottom - top};
            }
            tree.place(*children()[i], r);
        }
        return true;
    }
    void draw(ui::Canvas& c) override {
        const ui::Rect& in = frame();
        const float col = column_w(in);
        const float top = in.y + grid_top(), bottom = in.bottom();
        if (today_ >= 0) c.fill({in.x + kGutter + col * static_cast<float>(today_), in.y, col, in.h}, Paint::accent, 0.06f);
        for (size_t d = 0; d < days_.size(); ++d) {
            const float x = in.x + kGutter + col * static_cast<float>(d);
            const bool today = static_cast<int>(d) == today_;
            c.text(ui_.layout(days_[d], TextStyle::body_strong, std::max(1.f, col - 12), false), x + 8, in.y + 5, today ? Paint::accent : Paint::text);
            c.line(x, in.y, x, bottom, Paint::border);
        }
        c.line(in.right() - 1, in.y, in.right() - 1, bottom, Paint::border);
        c.line(in.x + kGutter, top, in.right(), top, Paint::border);
        for (int h = first_; h <= last_; ++h) {
            const float y = top + static_cast<float>(h - first_) * kHourH;
            if (h > first_) c.line(in.x + kGutter, y, in.right(), y, Paint::border, 0.5f);
            if (h < last_) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02d:00", h);
                c.text(ui_.layout(u16(buf), TextStyle::caption, kGutter - 8, false), in.x + 4, y + 2, Paint::text_secondary);
            }
        }
        // The current time in today's column.
        if (today_ >= 0 && now_min_ >= first_ * 60 && now_min_ <= last_ * 60) {
            const float y = minute_y(in, now_min_);
            const float x = in.x + kGutter + col * static_cast<float>(today_);
            c.line(x, y, x + col, y, Paint::error, 2.f);
            c.round({x - 4, y - 4, 8, 8}, 4, Paint::error);
        }
    }

private:
    float grid_top() const { return kHeadH + kAllDayH * static_cast<float>(all_day_rows_) + (all_day_rows_ > 0 ? 4 : 0); }
    float column_w(const ui::Rect& in) const {
        return std::max(40.f, (in.w - kGutter) / static_cast<float>(std::max<size_t>(1, days_.size())));
    }
    float minute_y(const ui::Rect& in, int minute) const {
        return in.y + grid_top() + static_cast<float>(minute - first_ * 60) * kHourH / 60.f;
    }
    ui::Ui& ui_;
    std::vector<std::u16string> days_;
    int today_;
    int now_min_;
    int first_, last_, all_day_rows_;
    std::vector<Slot> slots_;
};

}  // namespace

// ---- Calendar page ----------------------------------------------------------------------------------------------------

void PagePane::show_calendar_event(const std::string& event_id, long long at_ms) {
    const auto monday = [](long long ms) {
        const int wday = local_tm(ms).tm_wday;  // 0 = Sunday
        return local_midnight(ms, wday == 0 ? -6 : 1 - wday);
    };
    // Whole weeks between the two Mondays; rounded, because a week across a DST change is an hour shorter or longer.
    const double weeks = static_cast<double>(monday(at_ms) - monday(shell_.now())) / static_cast<double>(7 * kDayMs);
    calendar_mode_ = "week";
    calendar_offset_ = static_cast<long long>(std::llround(weeks));
    calendar_open_ = event_id;
}

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
    // The shown range, so Previous and Next say where they went.
    if (calendar_.is_object() && calendar_mode_ != "list") {
        const long long a = jint(calendar_, "from"), b = jint(calendar_, "to") - 1;
        const std::tm ta = local_tm(a), tb = local_tm(b);
        char buf[48];
        if (calendar_mode_ == "day") std::snprintf(buf, sizeof buf, "%d.%d.%d", ta.tm_mday, ta.tm_mon + 1, ta.tm_year + 1900);
        else std::snprintf(buf, sizeof buf, "%d.%d. – %d.%d.%d", ta.tm_mday, ta.tm_mon + 1, tb.tm_mday, tb.tm_mon + 1, tb.tm_year + 1900);
        auto* range = bar->emplace<ui::Label>(shell_.ui(), u16(buf), TextStyle::body_strong, Paint::text, false, "calendar-range");
        range->padding.top = 6;
        range->role = ui::Role::text;
    }
    if (!calendar_.is_object()) {
        if (!errors_.contains("calendar")) text(*this, shell_.t("common.loading"), Paint::text_secondary);
        error_line(*this, "calendar");
        return;
    }
    if (!jbool(calendar_, "available")) {
        text(*this, shell_.t("calendar.unavailable"), Paint::text_secondary, "calendar-unavailable");
        button(*this, "calendar-open-settings", shell_.t("calendar.open_settings"), [this] { shell_.navigate("#/settings/mail"); });
        return;
    }
    // Offline: the local copy, marked with the last sync (Calendar Page).
    const std::string last = jstr(calendar_, "last_sync");
    const bool offline = jstr(calendar_, "state") != "connected";
    const auto synced_ms = iso_millis(last);
    const std::u16string synced = synced_ms ? day_label(shell_, *synced_ms) + u" " + hhmm(*synced_ms) : u16(last);
    text(*this, last.empty() ? shell_.t("calendar.never_synced") : shell_.t(offline ? "calendar.offline_since" : "calendar.synced", {{"time", synced}}),
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
        build_calendar_details(events);
    } else {
        build_calendar_details(events);
        build_calendar_grid(events, from, to, now);
    }
    error_line(*this, "calendar");
}

// The chosen event's details and actions (Calendar Event Actions), above the grid so they are seen without scrolling.
void PagePane::build_calendar_details(const json& events) {
    for (const auto& e : events) {
        if (jstr(e, "id") != calendar_open_) continue;
        auto* d = emplace<ui::Box>("calendar-details", Paint::surface, 8, Paint::border);
        d->role = ui::Role::group;
        d->name = u16(jstr(e, "subject"));
        d->padding = ui::Insets::all(16);
        d->gap = 6;
        auto* head = d->emplace<Node>();
        head->axis = ui::Axis::row;
        head->gap = 8;
        head->emplace<ui::Label>(shell_.ui(), u16(jstr(e, "subject")), TextStyle::subtitle)->role = ui::Role::heading;
        auto* close = head->emplace<ui::IconButton>(shell_.ui(), "calendar-details-close", ui::icon::cancel, shell_.t("calendar.close_details"));
        close->on_click = [this] {
            calendar_open_.clear();
            rebuild();
        };
        row(*d, "calendar-details-when", shell_.t("calendar.when"),
            jbool(e, "all_day") ? day_label(shell_, jint(e, "start_ms")) + u" " + shell_.t("calendar.all_day")
                                : day_label(shell_, jint(e, "start_ms")) + u" " + hhmm(jint(e, "start_ms")) + u"–" + hhmm(jint(e, "end_ms")));
        if (!jstr(e, "location").empty()) row(*d, "calendar-details-where", shell_.t("calendar.where"), u16(jstr(e, "location")));
        if (const json* o = jobj(e, "organizer"); o && !jstr(*o, "address").empty())
            row(*d, "calendar-details-organizer", shell_.t("calendar.organizer"), u16(jstr(*o, "name").empty() ? jstr(*o, "address") : jstr(*o, "name")));
        std::u16string people;
        for (const auto& a : e.value("attendees", json::array()))
            people += (people.empty() ? u"" : u", ") + u16(jstr(a, "name").empty() ? jstr(a, "address") : jstr(a, "name"));
        if (!people.empty()) row(*d, "calendar-details-attendees", shell_.t("calendar.attendees"), people);
        if (const std::string body = jstr(e, "body"); !body.empty())
            text(*d, u16(body.size() > 1500 ? body.substr(0, 1500) + "…" : body), Paint::text_secondary, "calendar-details-body");
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
}

// Calendar Time Grid (improve-threads-calendar-agenda D4): one column per day, events placed by time and length.
void PagePane::build_calendar_grid(const json& events, long long from, long long to, long long now) {
    std::vector<long long> days;
    for (long long d = from; d < to; d = local_midnight(d, 1)) days.push_back(d);
    struct Item {
        const json* e;
        CalendarGrid::Slot slot;
    };
    std::vector<Item> items;
    std::vector<int> all_day_rows(days.size(), 0);
    std::vector<DaySpan> spans_all;
    for (size_t di = 0; di < days.size(); ++di) {
        const long long d0 = days[di], d1 = local_midnight(d0, 1);
        std::vector<const json*> timed;
        for (const auto& e : events) {
            const long long s = jint(e, "start_ms"), en = jint(e, "end_ms");
            if (!(en > d0 && s < d1)) continue;
            // All-day, or covering the whole day (a multi-day event): the band.
            if (jbool(e, "all_day") || (s <= d0 && en >= d1)) {
                CalendarGrid::Slot slot;
                slot.day = di;
                slot.all_day = true;
                slot.row = all_day_rows[di]++;
                items.push_back({&e, slot});
            } else {
                timed.push_back(&e);
            }
        }
        std::stable_sort(timed.begin(), timed.end(), [](const json* a, const json* b) { return jint(*a, "start_ms") < jint(*b, "start_ms"); });
        std::vector<DaySpan> spans;
        for (const json* e : timed) {
            const int s = static_cast<int>((std::max(jint(*e, "start_ms"), d0) - d0) / 60'000);
            const int en = static_cast<int>((std::min(jint(*e, "end_ms"), d1) - d0) / 60'000);
            spans.push_back({s, std::max(en, s + 1)});
        }
        const auto lanes = lay_out_lanes(spans);
        for (size_t i = 0; i < timed.size(); ++i) {
            CalendarGrid::Slot slot;
            slot.day = di;
            slot.start = spans[i].start;
            slot.end = spans[i].end;
            slot.lane = lanes[i];
            items.push_back({timed[i], slot});
            spans_all.push_back(spans[i]);
        }
    }
    const auto [first, last] = grid_hours(spans_all);
    int today = -1, now_min = 0;
    for (size_t di = 0; di < days.size(); ++di)
        if (now >= days[di] && now < local_midnight(days[di], 1)) {
            today = static_cast<int>(di);
            now_min = static_cast<int>((now - days[di]) / 60'000);
        }
    std::vector<std::u16string> names;
    for (long long d : days) names.push_back(day_label(shell_, d));
    const int rows = days.empty() ? 0 : *std::max_element(all_day_rows.begin(), all_day_rows.end());
    auto* grid = emplace<CalendarGrid>(shell_.ui(), std::move(names), today, now_min, first, last, rows);
    grid->name = shell_.t(calendar_mode_ == "day" ? "calendar.mode.day" : "calendar.mode.week");
    if (items.empty()) text(*this, shell_.t("calendar.empty"), Paint::text_secondary, "calendar-empty");
    for (const Item& it : items) {
        const json& e = *it.e;
        const std::string id = jstr(e, "id");
        const bool busy = e.contains("withheld") || jbool(e, "private");
        const bool cancelled = jbool(e, "cancelled");
        const std::u16string title = (cancelled ? shell_.t("calendar.cancelled") + u" " : std::u16string()) +
                                     (busy ? shell_.t("calendar.busy") : u16(jstr(e, "subject").empty() ? "(no subject)" : jstr(e, "subject")));
        const std::u16string when = it.slot.all_day ? shell_.t("calendar.all_day") : hhmm(jint(e, "start_ms")) + u"–" + hhmm(jint(e, "end_ms"));
        const std::u16string place = busy ? std::u16string() : u16(jstr(e, "location"));
        auto block = std::make_unique<EventBlock>(shell_.ui(), "calendar-event-" + id, it.slot.all_day ? std::u16string() : hhmm(jint(e, "start_ms")),
                                                  title, place, calendar_open_ == id, busy, cancelled, it.slot.all_day);
        block->name = day_label(shell_, days[it.slot.day]) + u" " + when + u" " + title + (place.empty() ? u"" : u" · " + place);
        block->help = block->name;  // the tooltip shows what a small block cuts off
        block->enabled = !busy;
        block->on_click = [this, id] {
            calendar_open_ = calendar_open_ == id ? std::string{} : id;
            rebuild();
        };
        grid->add_block(std::move(block), it.slot);
    }
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
    row(c, "settings-teams-last-read", shell_.t("settings.teams.last_read"), last.empty() ? shell_.t("settings.mail.never") : shell_.moment_of(last));
    row(c, "settings-teams-counts", shell_.t("settings.teams.local_copy"),
        shell_.t("settings.teams.counts", {{"conversations", shell_.number(jint(teams_, "conversations"))},
                                           {"messages", shell_.number(jint(teams_, "messages"))}}));
    if (const std::string e = jstr(teams_, "error"); !e.empty()) text(c, shell_.t("settings.teams.problem", {{"reason", u16(e)}}), Paint::warning, "settings-teams-error");
    if (const json* n = jobj(teams_, "counts"); n && (jint(*n, "skipped") > 0 || jint(*n, "decode_errors") > 0))
        text(c, shell_.t("settings.teams.skipped", {{"skipped", shell_.number(jint(*n, "skipped") + jint(*n, "decode_errors"))}}),
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
    // The time on one row: its label, a short field and Save (client/work-inbox Labelled Fields, Settings Row Layout).
    auto* time_row = c.emplace<Node>("settings-report-time-row");
    time_row->axis = ui::Axis::row;
    time_row->gap = 12;
    auto* time_label = time_row->emplace<ui::Label>(shell_.ui(), shell_.t("settings.daily_report.time"), TextStyle::body, Paint::text, false);
    time_label->role = ui::Role::none;
    time_label->padding.top = 6;
    auto* time = time_row->emplace<ui::TextField>(shell_.ui(), "settings-report-time", shell_.t("settings.daily_report.time"));
    time->width = ui::Sizing::fixed;
    time->fixed_w = 96;
    time->set_text(report_time_);
    ui::TextField* tp = time;
    time->on_change = [this, tp] { report_time_ = tp->text(); };
    button(*time_row, "settings-report-time-save", shell_.t("settings.daily_report.save_time"), [this, configure] {
        configure({{"time", ui::to_utf8(report_time_)}});
    })->enabled = busy_.empty();
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.daily_report.days"), TextStyle::body_strong)->role = ui::Role::heading;
    const json days = report_.value("days", json::array());
    auto* day_row = c.emplace<Node>("settings-report-days");
    day_row->axis = ui::Axis::row;
    day_row->wraps = true;
    day_row->gap = 16;
    for (int d : {1, 2, 3, 4, 5, 6, 0}) {
        const bool has = std::find(days.begin(), days.end(), json(d)) != days.end();
        setting_switch(*day_row, "settings-report-day-" + std::to_string(d), shell_.t("calendar.weekday." + std::to_string(d)), has, false,
                       [configure, days, d](bool v) {
                           json next = json::array();
                           for (const auto& x : days)
                               if (x != json(d)) next.push_back(x);
                           if (v) next.push_back(d);
                           configure({{"days", next}});
                       });
        day_row->children().back()->width = ui::Sizing::fit;
    }
    if (const std::string last = jstr(report_, "last_day"); !last.empty())
        row(c, "settings-report-last", shell_.t("settings.daily_report.last"), shell_.date_of(last));
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
