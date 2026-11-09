#include "meeting/dictation.h"

#include "base/strings.h"
#include "meeting/core.h"
#include "platform/crypto.h"
#include "platform/log.h"

namespace wb::meeting {

namespace {
constexpr const char* kLog = "dictation";
constexpr long long kCheckEveryMs = 30 * 1000;
}  // namespace

DictationService::DictationService(DictationEnv env) : env_(std::move(env)) {}

DictationService::~DictationService() {
    const std::lock_guard lock(mx_);
    if (state_ == State::recording || state_ == State::stopped) {
        halt();
        discard();
    }
}

void DictationService::remove_leftovers() {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(env_.capture_dir, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"dictation-", 0) != 0 || !e.is_regular_file(ec)) continue;
        std::error_code rm;
        if (std::filesystem::remove(e.path(), rm)) log::info(kLog, "removed the audio of an unfinished dictation");
    }
}

std::string DictationService::reason() {
    const long long now = env_.clock->unix_millis();
    if (checked_at_ < 0 || now - checked_at_ >= kCheckEveryMs) {
        checked_at_ = now;
        models_ = !env_.models_installed || env_.models_installed();
        microphone_ = !env_.microphone_present || env_.microphone_present();
    }
    if (!models_) return "models_missing";
    if (!microphone_) return "no_microphone";
    if (env_.meeting_running && env_.meeting_running()) return "meeting";
    return "";
}

Result<json> DictationService::start() {
    const std::lock_guard lock(mx_);
    if (env_.enabled && !env_.enabled()) return make_error("policy_denied", "Dictation is turned off by your organisation's policy.");
    if (state_ != State::idle) return make_error("busy", "A dictation is already running.");
    checked_at_ = -1;  // look again: the user is waiting for the answer
    const std::string why = reason();
    if (why == "meeting") return make_error("busy", "A meeting capture is running.");
    if (why == "models_missing") return make_error("models_missing", "Speech recognition is not installed.");
    if (why == "no_microphone") return make_error("no_microphone", "No microphone was found.");
    if (!env_.record) return make_error("audio", "Audio capture is not available.");
    const long long t0 = env_.clock->unix_millis();
    const auto file = env_.capture_dir / ("dictation-" + random_urlsafe(9) + ".skau");
    std::error_code ec;
    std::filesystem::create_directories(env_.capture_dir, ec);
    auto w = TrackWriter::create(file, TrackKind::local, 16000, t0);
    if (!w) return w.error();
    std::shared_ptr<TrackWriter> writer(std::move(*w));
    auto r = env_.record({CaptureSource::Kind::mic, 0}, writer);
    if (!r) {
        (void)writer->close();
        std::filesystem::remove(file, ec);
        return r.error();
    }
    recorder_ = std::move(*r);
    writer_ = std::move(writer);
    file_ = file;
    started_at_ = t0;
    state_ = State::recording;
    log::info(kLog, "recording");
    return json{{"started_at", t0}};
}

void DictationService::halt() {
    if (recorder_) recorder_->stop();
    recorder_.reset();
    if (writer_) (void)writer_->close();
    writer_.reset();
}

void DictationService::discard() {
    std::error_code ec;
    if (!file_.empty()) std::filesystem::remove(file_, ec);
    file_.clear();
    state_ = State::idle;
    started_at_ = 0;
}

Result<json> DictationService::stop() {
    std::filesystem::path file;
    {
        const std::lock_guard lock(mx_);
        if (state_ == State::recording) halt();
        if (state_ != State::recording && state_ != State::stopped) return make_error("not_recording", "No dictation is recording.");
        state_ = State::transcribing;
        file = file_;
    }
    // User-started work: the stt worker runs it now, never deferred. The chosen language, or detection with the
    // window's language as the fallback (D5).
    json params = {{"tracks", json::array({path_to_utf8(file)})}};
    const std::string chosen = env_.language ? env_.language() : std::string();
    if (supported_language(chosen)) params["language"] = chosen;
    else params["fallback_language"] = env_.fallback_language ? env_.fallback_language() : std::string("en");
    Result<json> r = env_.transcribe ? env_.transcribe(params)
                                     : Result<json>(make_error("models_missing", "Speech recognition is not installed."));
    {
        const std::lock_guard lock(mx_);
        discard();
    }
    if (!r) {
        log::warn(kLog, "transcription failed: " + r.error().code + (r.error().code == "unsupported_language" ? " " + r.error().message : ""));
        return r.error();
    }
    std::string text;
    if (const json* segs = jobj(*r, "segments"); segs && segs->is_array())
        for (const auto& s : *segs) {
            const std::string t = trim(jstr(s, "text"));
            if (t.empty()) continue;
            if (!text.empty()) text += ' ';
            text += t;
        }
    log::info(kLog, "transcribed (" + jstr(*r, "language") + ")");
    return json{{"text", text}, {"language", jstr(*r, "language")}};
}

Status DictationService::cancel() {
    const std::lock_guard lock(mx_);
    if (state_ == State::transcribing) return make_error("busy", "The dictation is being transcribed.");
    if (state_ == State::idle) return {};
    halt();
    discard();
    log::info(kLog, "discarded");
    return {};
}

void DictationService::tick() {
    bool limit = false, discarded = false;
    {
        const std::lock_guard lock(mx_);
        // Dictation Policy: switched off during a recording, the recording is discarded.
        if ((state_ == State::recording || state_ == State::stopped) && env_.enabled && !env_.enabled()) {
            halt();
            discard();
            discarded = true;
            log::info(kLog, "discarded: turned off by policy");
        } else if (state_ == State::recording && env_.clock->unix_millis() - started_at_ >= env_.limit_ms) {
            halt();
            state_ = State::stopped;
            limit = true;
        }
    }
    if (limit && env_.changed) env_.changed(json{{"state", "limit"}});
    if (discarded && env_.changed) env_.changed(json{{"state", "discarded"}});
}

json DictationService::status() {
    const std::lock_guard lock(mx_);
    const bool enabled = !env_.enabled || env_.enabled();
    const std::string why = enabled ? reason() : std::string();
    static constexpr const char* kStates[] = {"idle", "recording", "stopped", "transcribing"};
    return json{{"enabled", enabled}, {"available", enabled && why.empty()}, {"reason", why},
                {"state", kStates[static_cast<int>(state_)]}, {"started_at", started_at_}};
}

}  // namespace wb::meeting
