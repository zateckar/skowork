// Meeting Progress In Its Thread (show-meeting-transcription-progress D3) and transcribing again (improve-transcript-language
// D4): a line under a meeting-origin Thread's origin with the meeting's stage, polled every 5 seconds while the meeting
// is not done, with Transcribe now while it waits or failed and Transcribe again while its audio is kept.
#include "shell/format.h"
#include "shell/shell.h"
#include "ui/text16.h"

#include <cmath>
#include <ctime>

namespace wb::shell {

using ui::Paint;
using ui::Sizing;
using ui::TextStyle;

namespace {

constexpr UINT kPollMs = 5000;

std::u16string clock(long long ms) {
    const long long s = std::max(0LL, ms / 1000);
    const long long h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    std::string out = (h > 0 ? std::to_string(h) + ":" + (m < 10 ? "0" : "") : "") + std::to_string(m) + (sec < 10 ? ":0" : ":") + std::to_string(sec);
    return ui::to_utf16(out);
}

bool running_stage(const std::string& stage) {
    return stage == "asking" || stage == "recording" || stage == "waiting" || stage == "transcribing" || stage == "summarizing";
}

std::tm local_tm(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm lt{};
    localtime_s(&lt, &t);
    return lt;
}

std::u16string hhmm(const std::tm& t) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
    return ui::to_utf16(buf);
}

}  // namespace

// Thread Header (improve-threads-calendar-agenda D2): when the meeting took place, how long, who attended, and the
// calendar event when the meeting was matched to one.
void ThreadPane::rebuild_meeting_facts() {
    if (!meeting_facts_) return;
    meeting_facts_->clear_children();
    const json& p = progress_data_;
    if (!p.is_object()) {
        meeting_facts_->relayout();
        return;
    }
    ui::Ui& u = shell_.ui();
    const long long start = jint(p, "started_at"), end = jint(p, "ended_at");
    std::u16string facts;
    if (start > 0) {
        const std::tm s = local_tm(start);
        char day[16];
        std::snprintf(day, sizeof day, "%d.%d.%d", s.tm_mday, s.tm_mon + 1, s.tm_year + 1900);
        facts = shell_.t(std::string("calendar.weekday.") + std::to_string(s.tm_wday)) + u" " + ui::to_utf16(day) + u", " + hhmm(s);
        if (end > start) {
            facts += u"–" + hhmm(local_tm(end));
            facts += u" (" + ui::to_utf16(format_elapsed(((end - start) / 60'000) * 60'000)) + u")";
        }
    }
    std::vector<std::string> names;
    for (const auto& a : p.value("attendees", json::array()))
        if (a.is_string() && !a.get<std::string>().empty()) names.push_back(a.get<std::string>());
    if (!names.empty()) {
        constexpr size_t kShown = 3;
        std::u16string who;
        for (size_t i = 0; i < names.size() && i < kShown; ++i) who += (i ? u", " : u"") + ui::to_utf16(names[i]);
        if (names.size() > kShown) who += u" " + shell_.t("thread.meeting.more_attendees", {{"count", ui::to_utf16(std::to_string(names.size() - kShown))}});
        facts += (facts.empty() ? u"" : u" · ") + who;
    }
    if (!facts.empty()) {
        auto* l = meeting_facts_->emplace<ui::Label>(u, facts, TextStyle::caption, Paint::text_secondary, false, "thread-meeting-facts-text");
        l->padding.top = 1;
        std::u16string all;
        for (const auto& n : names) all += (all.empty() ? u"" : u", ") + ui::to_utf16(n);
        l->help = all;  // every attendee, for the tooltip and screen readers
    }
    if (const std::string event = jstr(p, "event_id"); !event.empty()) {
        auto* b = meeting_facts_->emplace<ui::Button>(u, "thread-meeting-calendar", shell_.t("thread.meeting.show_in_calendar"), ui::Button::Kind::subtle);
        b->width = Sizing::fit;
        b->role = ui::Role::link;
        b->on_click = [this, event, start] { shell_.show_calendar_event(event, start > 0 ? start : shell_.now()); };
    }
    meeting_facts_->relayout();
    meeting_facts_->invalidate();
}

void ThreadPane::meetings_changed() {
    if (!progress_meeting_.empty()) load_progress();
}

void ThreadPane::load_progress() {
    if (progress_timer_) shell_.window().cancel_timer(progress_timer_);
    progress_timer_ = 0;
    std::weak_ptr<int> alive = alive_;
    const std::string id = progress_meeting_;
    shell_.call("meetings.progress", {{"id", id}}, [this, alive, id](Result<json> r) {
        if (!alive.lock() || id != progress_meeting_) return;
        progress_data_ = r ? *r : json(nullptr);
        rebuild_progress();
        rebuild_meeting_facts();
        // Polled while it runs; the share done and the elapsed time change without an event.
        if (r && running_stage(jstr(*r, "stage")) && !progress_timer_)
            progress_timer_ = shell_.window().set_timer(kPollMs, [this, alive, id] {
                if (!alive.lock()) return;
                progress_timer_ = 0;
                if (id == progress_meeting_) load_progress();
            });
    });
}

void ThreadPane::rebuild_progress() {
    if (!progress_) return;
    progress_->clear_children();
    progress_->padding = {};
    const json& p = progress_data_;
    const std::string stage = p.is_object() ? jstr(p, "stage") : std::string();
    const bool again = p.is_object() && jbool(p, "can_transcribe_again");
    if (stage.empty() || (stage == "done" && !again)) {
        progress_->relayout();
        progress_->invalidate();
        return;
    }
    ui::Ui& u = shell_.ui();
    progress_->axis = ui::Axis::row;
    progress_->gap = 8;
    progress_->padding = {20, 0, 12, 8};
    std::u16string text;
    Paint ink = Paint::text_secondary;
    if (stage == "asking") {
        text = shell_.t("thread.meeting.asking");
    } else if (stage == "recording") {
        text = shell_.t("thread.meeting.recording", {{"time", clock(jint(p, "elapsed_ms"))}});
        ink = Paint::error;
    } else if (stage == "waiting") {
        const std::string reason = jstr(p, "reason");
        text = shell_.t("thread.meeting.waiting." + (reason.empty() ? std::string("queued") : reason));
    } else if (stage == "transcribing") {
        const double done = p.contains("done") && p["done"].is_number() ? p["done"].get<double>() : 0;
        const std::u16string percent = ui::to_utf16(std::to_string(static_cast<int>(std::floor(done * 100))));
        if (p.contains("left_ms") && p["left_ms"].is_number()) {
            const long long left = std::max(1LL, (jint(p, "left_ms") + 59'999) / 60'000);  // whole minutes, at least one
            text = shell_.t("thread.meeting.transcribing_left", {{"percent", percent}, {"minutes", ui::to_utf16(std::to_string(left))}});
        } else {
            text = shell_.t("thread.meeting.transcribing", {{"percent", percent}});
        }
    } else if (stage == "summarizing") {
        text = shell_.t("thread.meeting.summarizing");
    } else if (stage == "failed") {
        const std::string why = jstr(p, "error");
        text = why.empty() ? shell_.t("thread.meeting.failed_no_reason") : shell_.t("thread.meeting.failed", {{"why", ui::to_utf16(why)}});
        ink = Paint::warning;
    } else {
        text = shell_.t("thread.meeting.done");
    }
    auto* l = progress_->emplace<ui::Label>(u, text, TextStyle::caption, ink, true, "thread-meeting-progress-text");
    l->padding.top = 6;
    l->width = Sizing::fill;
    l->role = ui::Role::text;
    // A screen reader hears a change of stage, not every new share.
    if (stage != progress_announced_) {
        if (!progress_announced_.empty()) shell_.announce(text);
        progress_announced_ = stage;
    }
    const std::string id = jstr(p, "id");
    std::weak_ptr<int> alive = alive_;
    if (jbool(p, "can_transcribe_now") && (stage == "waiting" || stage == "failed")) {
        auto* b = progress_->emplace<ui::Button>(u, "thread-meeting-transcribe-now", shell_.t("thread.meeting.transcribe_now"),
                                                 ui::Button::Kind::subtle);
        b->width = Sizing::fit;
        b->enabled = !progress_busy_;
        b->on_click = [this, alive, id] {
            progress_busy_ = true;
            rebuild_progress();
            shell_.call("meetings.transcribe_now", {{"id", id}}, [this, alive](Result<json> r) {
                if (!alive.lock()) return;
                progress_busy_ = false;
                if (!r) shell_.alert(r.error());
                load_progress();
            });
        };
    }
    if (again) {
        auto* b = progress_->emplace<ui::Button>(u, "thread-meeting-transcribe-again", shell_.t("thread.meeting.transcribe_again"),
                                                 ui::Button::Kind::subtle, ui::icon::chevron_down);
        b->width = Sizing::fit;
        b->enabled = !progress_busy_;
        b->on_click = [this, alive, id, b] {
            auto menu = std::make_unique<ui::Menu>("transcribe-again-menu", shell_.t("thread.meeting.transcribe_again"));
            for (const char* lang : {"cs", "de", "en"}) {
                const std::string code = lang;
                menu->add_item(shell_.ui(), std::string("transcribe-again-") + lang, shell_.t(std::string("settings.general.dictation_lang.") + lang),
                               [this, alive, id, code] {
                                   if (!alive.lock()) return;
                                   progress_busy_ = true;
                                   rebuild_progress();
                                   shell_.call("meetings.transcribe_again", {{"id", id}, {"language", code}}, [this, alive](Result<json> r) {
                                       if (!alive.lock()) return;
                                       progress_busy_ = false;
                                       if (!r) shell_.alert(r.error());
                                       else shell_.announce(shell_.t("thread.meeting.transcribing_again"));
                                       load_progress();
                                   });
                               });
            }
            shell_.window().overlay().show(std::move(menu), b->frame(), true);
            shell_.window().render();
        };
    }
    progress_->relayout();
    progress_->invalidate();
}

}  // namespace wb::shell
