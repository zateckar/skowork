// Meeting Progress In Its Thread (show-meeting-transcription-progress D3) and transcribing again (improve-transcript-language
// D4): a line under a meeting-origin Thread's origin with the meeting's stage, polled every 5 seconds while the meeting
// is not done, with Transcribe now while it waits or failed and Transcribe again while its audio is kept.
#include "shell/shell.h"
#include "ui/text16.h"

#include <cmath>

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

}  // namespace

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
