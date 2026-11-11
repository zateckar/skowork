// Meetings (client/meeting-capture Meetings Settings Section): whether capture is available, the running capture with
// Stop, manual capture of an audio source when policy allows it, the captured meetings with their state, Transcribe now,
// the transcript and speaker renaming, the segments left out as echo with Restore, the user's vocabulary terms, and the
// series opted in for automatic capture. The host does the work (meetings.status, .stop, .capture, .transcribe_now,
// .rename_speaker, .restore_echo, .add_term, .remove_term, .remove_series, .forget).
#include "shell/shell.h"
#include "ui/text16.h"
#include "ui/textfield.h"

#include <cstdio>
#include <ctime>

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

namespace {

std::u16string u16(const std::string& s) { return ui::to_utf16(s); }

// Unix ms as local "2026-10-26 15:04".
std::u16string local_time(long long ms) {
    if (ms <= 0) return u"";
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return u16(buf);
}

}  // namespace

void PagePane::load_meetings() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("meetings.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            meetings_ = *res;
            errors_.erase("meetings");
        } else {
            errors_["meetings"] = u16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::meetings_changed() {
    if (shell_.route().name == RouteName::settings) load_meetings();
}

void PagePane::build_meetings(Node& c) {
    text(c, shell_.t("settings.meetings.body"), Paint::text_secondary);
    if (!meetings_.is_object()) {
        if (!errors_.contains("meetings")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "meetings");
        return;
    }
    // Feature Disabled: no capture controls at all.
    if (!jbool(meetings_, "available")) {
        text(c, shell_.t("settings.meetings.off"), Paint::text_secondary, "settings-meetings-off");
        error_line(c, "meetings");
        return;
    }
    text(c, shell_.t(jbool(meetings_, "require_notice") ? "settings.meetings.notice_required" : "settings.meetings.available"),
         Paint::text_secondary, "settings-meetings-available");

    // The running capture.
    if (const json* cap = jobj(meetings_, "capture"); cap && cap->is_object()) {
        auto* r = c.emplace<Node>("settings-meetings-capture");
        r->axis = ui::Axis::row;
        r->gap = 8;
        const std::u16string what = shell_.t("settings.meetings.capturing", {{"title", u16(jstr(*cap, "title"))},
                                                                            {"since", local_time(jint(*cap, "started_at"))}});
        r->name = what;
        r->emplace<ui::Label>(shell_.ui(), what, TextStyle::body_strong, Paint::error, true);
        button(*r, "settings-meetings-stop", shell_.t("settings.meetings.stop"), [this] {
            act("meetings", "meetings.stop", json::object(), [this](const json&) {
                shell_.announce(shell_.t("settings.meetings.stopped"));
                load_meetings();
            });
        })->enabled = busy_.empty();
        // Capture Trouble Indication: the tracks without a working stream.
        if (const json* tr = jobj(*cap, "trouble"); tr && tr->is_array() && !tr->empty()) {
            bool mic = false, audio = false;
            for (const auto& t : *tr) (t == "local" || t == "mic" ? mic : audio) = true;
            const std::u16string trouble = mic && audio ? shell_.t("settings.meetings.trouble_both")
                                        : mic       ? shell_.t("settings.meetings.trouble_mic")
                                                    : shell_.t("settings.meetings.trouble_audio");
            text(c, trouble, Paint::warning, "settings-meetings-trouble");
        }
    } else if (jbool(meetings_, "allow_manual")) {
        // Manual Audio Source Capture.
        c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.meetings.manual"), TextStyle::body_strong)->role = ui::Role::heading;
        text(c, shell_.t("settings.meetings.manual_body"), Paint::text_secondary);
        auto* list = c.emplace<Node>("settings-meetings-sources");
        list->role = ui::Role::list;
        list->name = shell_.t("settings.meetings.manual");
        list->gap = 4;
        if (const json* src = jobj(meetings_, "sources"))
            for (const auto& s : *src) {
                const std::string source = jstr(s, "source");
                const std::u16string label = source == "system" ? shell_.t("settings.meetings.source_system")
                                             : source == "mic"  ? shell_.t("settings.meetings.source_mic")
                                                                : shell_.t("settings.meetings.source_app", {{"name", u16(jstr(s, "name"))}});
                auto* b = button(*list, "meetings-capture-" + source, label, [this, source] {
                    act("meetings", "meetings.capture", {{"source", source}}, [this](const json&) { load_meetings(); });
                }, ui::Button::Kind::subtle);
                b->role = ui::Role::list_item;
                b->enabled = busy_.empty();
            }
    }

    // Captured meetings.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.meetings.list"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* list = c.emplace<Node>("settings-meetings-list");
    list->role = ui::Role::list;
    list->name = shell_.t("settings.meetings.list");
    list->gap = 6;
    const json* ms = jobj(meetings_, "meetings");
    if (!ms || ms->empty()) text(*list, shell_.t("settings.meetings.none"), Paint::text_secondary, "settings-meetings-none");
    if (ms)
        for (const auto& m : *ms) {
            const std::string id = jstr(m, "id"), state = jstr(m, "state");
            auto* item = list->emplace<Node>("meeting-" + id);
            item->role = ui::Role::list_item;
            item->gap = 4;
            const std::u16string title = u16(jstr(m, "title"));
            item->name = title;
            auto* head = item->emplace<Node>();
            head->axis = ui::Axis::row;
            head->gap = 8;
            head->emplace<ui::Label>(shell_.ui(), title, TextStyle::body_strong, Paint::text, true)->role = ui::Role::none;
            const std::string skey = "settings.meetings.states." + state;
            head->emplace<ui::Label>(shell_.ui(), shell_.ui().strings.has(skey) ? shell_.t(skey) : u16(state), TextStyle::caption,
                                     Paint::text_secondary, false, "meeting-state-" + id);
            text(*item, local_time(jint(m, "started_at") ? jint(m, "started_at") : jint(m, "detected_at")), Paint::text_secondary);
            if (const std::string e = jstr(m, "error"); !e.empty()) text(*item, u16(e), Paint::warning, "meeting-error-" + id);
            auto* actions = item->emplace<Node>();
            actions->axis = ui::Axis::row;
            actions->gap = 8;
            if (jbool(m, "has_audio") && (state == "pending" || state == "failed" || state == "unsupported"))
                button(*actions, "meeting-transcribe-" + id, shell_.t("settings.meetings.transcribe_now"), [this, id] {
                    act("meetings", "meetings.transcribe_now", {{"id", id}}, [this](const json&) {
                        shell_.announce(shell_.t("settings.meetings.transcribing"));
                        load_meetings();
                    });
                })->enabled = busy_.empty();
            const bool open = meetings_open_ == id;
            const bool has_text = !jstr(m, "transcript").empty();
            if (has_text)
                button(*actions, "meeting-show-" + id, shell_.t(open ? "settings.meetings.hide" : "settings.meetings.show"), [this, id, open] {
                    meetings_open_ = open ? std::string{} : id;
                    rebuild();
                }, ui::Button::Kind::subtle);
            if (const std::string thread = jstr(m, "thread_id"); !thread.empty() && state == "done")
                button(*actions, "meeting-open-" + id, shell_.t("settings.meetings.open_thread"),
                       [this, thread] { shell_.navigate(thread_hash(thread)); }, ui::Button::Kind::subtle);
            button(*actions, "meeting-forget-" + id, shell_.t("settings.meetings.forget"), [this, id] {
                act("meetings", "meetings.forget", {{"id", id}}, [this](const json&) { load_meetings(); });
            }, ui::Button::Kind::subtle)->enabled = busy_.empty();
            // Echo Review: how many segments were left out, the list, and Restore.
            if (const json* echo = jobj(m, "echo"); echo && echo->is_array() && !echo->empty()) {
                const bool echo_open = meetings_echo_open_ == id;
                auto* er = item->emplace<Node>();
                er->axis = ui::Axis::row;
                er->gap = 8;
                er->emplace<ui::Label>(shell_.ui(), shell_.t("settings.meetings.echo_count", {{"count", u16(std::to_string(echo->size()))}}),
                                       TextStyle::caption, Paint::text_secondary, false, "meeting-echo-count-" + id);
                button(*er, "meeting-echo-" + id, shell_.t(echo_open ? "settings.meetings.echo_hide" : "settings.meetings.echo_show"),
                       [this, id, echo_open] {
                           meetings_echo_open_ = echo_open ? std::string{} : id;
                           rebuild();
                       }, ui::Button::Kind::subtle);
                if (echo_open) {
                    auto* el = item->emplace<Node>("meeting-echo-list-" + id);
                    el->role = ui::Role::list;
                    el->name = shell_.t("settings.meetings.echo_list");
                    el->gap = 4;
                    for (const auto& e : *echo) {
                        const long long at = jint(e, "start_ms");
                        const std::string key = id + "-" + std::to_string(at);
                        auto* row = el->emplace<Node>("meeting-echo-" + key);
                        row->role = ui::Role::list_item;
                        row->axis = ui::Axis::row;
                        row->gap = 8;
                        char stamp[16];
                        std::snprintf(stamp, sizeof stamp, "[%02lld:%02lld] ", at / 60000, (at / 1000) % 60);
                        const std::u16string line = u16(stamp) + u16(jstr(e, "text"));
                        row->name = line;
                        row->emplace<ui::Label>(shell_.ui(), line, TextStyle::body, Paint::text_secondary, true)->role = ui::Role::none;
                        button(*row, "meeting-echo-restore-" + key, shell_.t("settings.meetings.echo_restore"), [this, id, at] {
                            act("meetings", "meetings.restore_echo", {{"id", id}, {"start_ms", at}}, [this](const json&) {
                                shell_.announce(shell_.t("settings.meetings.echo_restored"));
                                load_meetings();
                            });
                        }, ui::Button::Kind::subtle)->enabled = busy_.empty();
                    }
                }
            }
            if (!open || !has_text) continue;
            // Speaker Diarization And Naming: rename, and the transcript shows the new names.
            if (const json* sp = jobj(m, "speakers"))
                for (const auto& s : *sp) {
                    const std::string speaker = jstr(s, "id");
                    const std::string key = id + "-" + speaker;
                    auto* r = item->emplace<Node>();
                    r->axis = ui::Axis::row;
                    r->gap = 8;
                    auto* f = r->emplace<ui::TextField>(shell_.ui(), "meeting-speaker-" + key,
                                                        shell_.t("settings.meetings.speaker_name", {{"name", u16(jstr(s, "name"))}}));
                    f->set_text(speaker_drafts_.contains(key) ? speaker_drafts_[key] : u16(jstr(s, "name")));
                    ui::TextField* fp = f;
                    f->on_change = [this, fp, key] { speaker_drafts_[key] = fp->text(); };
                    button(*r, "meeting-rename-" + key, shell_.t("settings.meetings.rename"), [this, id, speaker, key] {
                        const std::string typed = ui::to_utf8(speaker_drafts_[key]);
                        if (typed.find_first_not_of(" \t") == std::string::npos) return;
                        act("meetings", "meetings.rename_speaker", {{"id", id}, {"speaker", speaker}, {"name", typed}}, [this, key](const json&) {
                            speaker_drafts_.erase(key);
                            shell_.announce(shell_.t("settings.meetings.renamed"));
                            load_meetings();
                        });
                    })->enabled = busy_.empty();
                }
            auto* t = item->emplace<ui::Label>(shell_.ui(), u16(jstr(m, "transcript")), TextStyle::body, Paint::text, true, "meeting-transcript-" + id);
            t->name = shell_.t("settings.meetings.transcript");
        }

    // User Vocabulary Terms: names and words transcription should spell as written here.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.meetings.terms"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.meetings.terms_body"), Paint::text_secondary);
    {
        auto* add = c.emplace<Node>();
        add->axis = ui::Axis::row;
        add->gap = 8;
        auto* f = add->emplace<ui::TextField>(shell_.ui(), "settings-meetings-term", shell_.t("settings.meetings.term"));
        f->set_text(term_draft_);
        ui::TextField* fp = f;
        f->on_change = [this, fp] { term_draft_ = fp->text(); };
        button(*add, "settings-meetings-term-add", shell_.t("settings.meetings.term_add"), [this] {
            const std::string typed = ui::to_utf8(term_draft_);
            if (typed.find_first_not_of(" \t") == std::string::npos) return;
            act("meetings", "meetings.add_term", {{"term", typed}}, [this](const json&) {
                term_draft_.clear();
                shell_.announce(shell_.t("settings.meetings.term_added"));
                load_meetings();
            });
        })->enabled = busy_.empty();
    }
    auto* tl = c.emplace<Node>("settings-meetings-terms");
    tl->role = ui::Role::list;
    tl->name = shell_.t("settings.meetings.terms");
    tl->gap = 4;
    const json* terms = jobj(meetings_, "terms");
    if (!terms || terms->empty()) text(*tl, shell_.t("settings.meetings.no_terms"), Paint::text_secondary, "settings-meetings-no-terms");
    if (terms)
        for (size_t i = 0; i < terms->size(); ++i) {
            if (!(*terms)[i].is_string()) continue;
            const std::string term = (*terms)[i].get<std::string>();
            const std::string n = std::to_string(i);  // terms can hold any letters; ids stay plain
            auto* item = tl->emplace<Node>("term-" + n);
            item->role = ui::Role::list_item;
            item->axis = ui::Axis::row;
            item->gap = 8;
            item->name = u16(term);
            item->emplace<ui::Label>(shell_.ui(), u16(term), TextStyle::body, Paint::text, false)->role = ui::Role::none;
            auto* rm = button(*item, "term-remove-" + n, shell_.t("settings.meetings.term_remove"), [this, term] {
                act("meetings", "meetings.remove_term", {{"term", term}}, [this](const json&) { load_meetings(); });
            }, ui::Button::Kind::subtle);
            rm->name = shell_.t("settings.meetings.term_remove_named", {{"name", u16(term)}});
            rm->enabled = busy_.empty();
        }

    // Series opted in for automatic capture.
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.meetings.series"), TextStyle::body_strong)->role = ui::Role::heading;
    auto* sl = c.emplace<Node>("settings-meetings-series");
    sl->role = ui::Role::list;
    sl->name = shell_.t("settings.meetings.series");
    sl->gap = 4;
    const json* series = jobj(meetings_, "series");
    if (!series || series->empty()) text(*sl, shell_.t("settings.meetings.no_series"), Paint::text_secondary, "settings-meetings-no-series");
    if (series)
        for (const auto& s : *series) {
            const std::string id = jstr(s, "id");
            auto* item = sl->emplace<Node>("series-" + id);
            item->role = ui::Role::list_item;
            item->axis = ui::Axis::row;
            item->gap = 8;
            const std::u16string series_title = u16(jstr(s, "title"));
            item->name = series_title;
            item->emplace<ui::Label>(shell_.ui(), series_title, TextStyle::body, Paint::text, false)->role = ui::Role::none;
            auto* rm = button(*item, "series-remove-" + id, shell_.t("settings.meetings.remove_series"), [this, id] {
                act("meetings", "meetings.remove_series", {{"id", id}}, [this](const json&) { load_meetings(); });
            }, ui::Button::Kind::subtle);
            rm->name = shell_.t("settings.meetings.remove_series_named", {{"name", series_title}});
            rm->enabled = busy_.empty();
        }
    error_line(c, "meetings");
}

}  // namespace wb::shell
