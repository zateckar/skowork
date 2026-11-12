// Speech recognition setup in Settings > General (client/speech-models Speech Setup Entry, Speech Repair And Removal):
// the package state, Download from the catalog in policy, Import package from a file, Cancel and Remove. The host does
// the work (speech.status, .install, .import, .cancel, .remove) and reports each step with speech.changed. Nothing here
// records: the microphone is used only by the Dictate control.
#include "platform/win.h"
#include "shell/shell.h"
#include "ui/text16.h"

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
    error_line(c, "speech");
}

}  // namespace wb::shell
