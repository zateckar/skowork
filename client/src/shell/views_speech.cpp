// Speech recognition setup in Settings > General (client/speech-models Speech Setup Entry, Speech Repair And Removal,
// Explicit Speech Check): the package state and disk use, Download from the catalog in policy, Import package from a
// file, Verify, Repair, Cancel and Remove, and the microphone state. The host does the work (speech.status, .install,
// .import, .verify, .repair, .cancel, .remove) and reports each step with speech.changed. None of those records: only
// Test microphone does, through the window's one dictation, and its text is shown here, never inserted or sent.
#include "platform/win.h"
#include "shell/shell.h"
#include "ui/text16.h"

#include <algorithm>

namespace wb::shell {

using ui::Node;
using ui::Paint;
using ui::TextStyle;

void PagePane::load_speech() {
    std::weak_ptr<int> alive = alive_;
    shell_.call("speech.status", json::object(), [this, alive](Result<json> res) {
        if (!alive.lock()) return;
        if (res) {
            speech_ = *res;
            errors_.erase("speech");
        } else {
            speech_ = nullptr;
            errors_["speech"] = ui::to_utf16(res.error().message);
        }
        rebuild();
    });
}

void PagePane::speech_changed(const json& event) {
    const std::string state = jstr(event, "state");
    if (state == "downloading" && event.contains("bytes")) {  // progress only: no round trip
        speech_bytes_ = jint(event, "bytes");
        if (speech_.is_object()) speech_["state"] = state;
        rebuild();
        return;
    }
    if (state != "downloading") speech_bytes_ = 0;
    if (state == "ready") shell_.announce(shell_.t("settings.speech.announce_ready"));
    if (state == "failed") shell_.announce(shell_.t("settings.speech.announce_failed"));
    load_speech();
}

namespace {

constexpr char kSpeechCheck[] = "settings:speech-check";

std::u16string clock_text(long long ms) {
    const long long s = std::max(0LL, ms / 1000);
    return ui::to_utf16(std::to_string(s / 60) + (s % 60 < 10 ? ":0" : ":") + std::to_string(s % 60));
}

}  // namespace

void PagePane::speech_check_changed() {
    const bool mine = shell_.dictation_owner() == kSpeechCheck;
    const std::string shown = std::to_string(static_cast<int>(shell_.dictation_phase())) + (mine ? "m" : "");
    if (shown == speech_check_shown_) {
        if (speech_check_time_ && mine && shell_.dictation_phase() == Shell::DictationPhase::recording) {
            speech_check_time_->set_text(shell_.t("settings.speech.test_recording", {{"time", clock_text(shell_.now() - shell_.dictation_started())}}));
            speech_check_time_->invalidate();
        }
        return;
    }
    rebuild();
}
void PagePane::build_speech(Node& c) {
    c.emplace<ui::Label>(shell_.ui(), shell_.t("settings.speech.title"), TextStyle::body_strong)->role = ui::Role::heading;
    text(c, shell_.t("settings.speech.body"), Paint::text_secondary);
    if (!speech_.is_object()) {
        if (!errors_.contains("speech")) text(c, shell_.t("common.loading"), Paint::text_secondary);
        error_line(c, "speech");
        return;
    }
    const std::string state = jstr(speech_, "state", "idle");
    const bool installed = jbool(speech_, "installed");
    const bool working = state == "downloading" || state == "verifying" || state == "probing";
    const std::string version = jstr(speech_, "version");
    const std::string available = jstr(speech_, "available_version");
    const ui::Params v{{"version", ui::to_utf16(version)}};

    std::u16string line;
    if (state == "downloading")
        line = shell_.t("settings.speech.state.downloading", {{"mb", ui::to_utf16(std::to_string(speech_bytes_ >> 20))}});
    else if (working)
        line = shell_.t("settings.speech.state." + state);
    else if (installed)
        line = shell_.t("settings.speech.state.ready", v);
    else
        line = shell_.t("settings.speech.state.not_installed");
    text(c, line, Paint::text, "settings-speech-state");
    if (!working && state == "failed")
        text(c, shell_.t("settings.speech.failed", {{"why", ui::to_utf16(jstr(speech_, "error"))}}), Paint::text, "settings-speech-error");
    if (!working && state == "cancelled") text(c, shell_.t("settings.speech.cancelled"), Paint::text_secondary);
    if (installed && speech_.contains("disk_bytes"))
        text(c, shell_.t("settings.speech.disk", {{"mb", ui::to_utf16(std::to_string((jint(speech_, "disk_bytes") + (1 << 19)) >> 20))}}),
             Paint::text_secondary, "settings-speech-disk");

    // The microphone, for Dictate and the check: missing, or busy with a meeting or another composer's dictation.
    const json d = shell_.status().value("dictation", json::object());
    const bool mine = shell_.dictation_owner() == kSpeechCheck;
    const Shell::DictationPhase phase = shell_.dictation_phase();
    speech_check_shown_ = std::to_string(static_cast<int>(phase)) + (mine ? "m" : "");
    speech_check_time_ = nullptr;
    const std::string reason = jstr(d, "reason");
    std::string mic;
    if (!mine && phase != Shell::DictationPhase::idle) mic = "other";
    else if (reason == "no_microphone" || reason == "meeting") mic = reason;
    if (installed && !mic.empty()) text(c, shell_.t("settings.speech.mic." + mic), Paint::text_secondary, "settings-speech-mic");

    auto* actions = c.emplace<Node>();
    actions->axis = ui::Axis::row;
    actions->gap = 8;
    if (working) {
        button(*actions, "settings-speech-cancel", shell_.t("settings.speech.cancel"), [this] { act("speech", "speech.cancel", json::object()); });
    } else {
        const bool can_download = jbool(speech_, "download");
        // An installed package that is the published version needs no second download.
        const bool newer = !installed || (!available.empty() && available != version);
        if (can_download && newer)
            button(*actions, "settings-speech-download",
                   shell_.t(installed ? "settings.speech.update" : "settings.speech.download"),
                   [this] { act("speech", "speech.install", json::object(), [this](const json&) { load_speech(); }); },
                   installed ? ui::Button::Kind::standard : ui::Button::Kind::primary)
                ->enabled = busy_.empty();
        button(*actions, "settings-speech-import", shell_.t("settings.speech.import"), [this] {
            const auto picked = shell_.pick_files();
            if (picked.empty()) return;
            act("speech", "speech.import", {{"path", narrow(picked.front())}}, [this](const json&) { load_speech(); });
        })->enabled = busy_.empty();
        if (installed) {
            button(*actions, "settings-speech-verify", shell_.t("settings.speech.verify"),
                   [this] { act("speech", "speech.verify", json::object(), [this](const json&) { load_speech(); }); })
                ->enabled = busy_.empty();
            button(*actions, "settings-speech-repair", shell_.t("settings.speech.repair"),
                   [this] { act("speech", "speech.repair", json::object(), [this](const json&) { load_speech(); }); })
                ->enabled = busy_.empty();
        }
        if (installed)
            button(*actions, "settings-speech-remove", shell_.t("settings.speech.remove"),
                   [this, version] {
                       act("speech", "speech.remove", {{"version", version}}, [this](const json& r) {
                           shell_.announce(shell_.t(jbool(r, "removed") ? "settings.speech.removed" : "settings.speech.remove_later"));
                           load_speech();
                       });
                   },
                   ui::Button::Kind::subtle)
                ->enabled = busy_.empty();
        if (!can_download && !installed) text(c, shell_.t("settings.speech.no_download"), Paint::text_secondary, "settings-speech-no-download");
    }

    // Test microphone: records only when chosen, with Stop and Cancel; the recognised text stays in this block.
    if (installed && !working && jbool(d, "enabled")) {
        auto* check = c.emplace<Node>();
        check->axis = ui::Axis::row;
        check->gap = 8;
        if (mine && phase == Shell::DictationPhase::recording) {
            auto* stop = button(*check, "settings-speech-test-stop", shell_.t("settings.speech.test_stop"), [this] { shell_.stop_dictation(); },
                                ui::Button::Kind::primary);
            button(*check, "settings-speech-test-cancel", shell_.t("settings.speech.test_cancel"), [this] { shell_.cancel_dictation(); });
            speech_check_time_ = text(c, shell_.t("settings.speech.test_recording", {{"time", clock_text(shell_.now() - shell_.dictation_started())}}),
                                      Paint::text, "settings-speech-test-recording");
            if (ui::Tree* tr = Node::tree(); tr && !tr->focused()) tr->set_focus(stop);
        } else if (mine) {
            text(c, shell_.t("settings.speech.test_transcribing"), Paint::text_secondary, "settings-speech-test-progress");
        } else {
            button(*check, "settings-speech-test", shell_.t("settings.speech.test"), [this] {
                speech_check_done_ = false;
                speech_check_text_.clear();
                speech_check_error_.clear();
                std::weak_ptr<int> alive = alive_;
                shell_.start_dictation(kSpeechCheck, alive, {}, [this, alive](const std::u16string& text, const std::u16string& error) {
                    if (!alive.lock()) return;
                    speech_check_done_ = true;
                    speech_check_text_ = text;
                    speech_check_error_ = error;
                    shell_.announce(!error.empty() ? shell_.t("settings.speech.test_failed", {{"why", error}})
                                    : text.empty() ? shell_.t("settings.speech.test_empty")
                                                   : shell_.t("settings.speech.test_result", {{"text", text}}));
                    rebuild();
                });
            })->enabled = jbool(d, "available") && phase == Shell::DictationPhase::idle && busy_.empty();
            if (speech_check_done_)
                text(c, !speech_check_error_.empty() ? shell_.t("settings.speech.test_failed", {{"why", speech_check_error_}})
                        : speech_check_text_.empty() ? shell_.t("settings.speech.test_empty")
                                                     : shell_.t("settings.speech.test_result", {{"text", speech_check_text_}}),
                     Paint::text, "settings-speech-test-result");
        }
        text(c, shell_.t("settings.speech.test_note"), Paint::text_secondary);
    }
    error_line(c, "speech");
}

}  // namespace wb::shell
