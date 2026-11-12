// client/dictation (add-dictation design D1, D2): one microphone recording at a time into an encrypted track under
// <data root>/capture, transcribed in the stt worker when the user stops it and deleted after. Everything outside this
// module comes in through DictationEnv so tests drive it with a fake recorder, the test speech engine and a fake clock.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "meeting/meeting_service.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace wb::meeting {

struct DictationEnv {
    const Clock* clock = &system_clock();
    std::filesystem::path capture_dir;  // <data root>/capture
    std::function<bool()> enabled;          // policy.dictation.mode is not off; null: on
    std::function<bool()> meeting_running;  // a meeting capture runs
    std::function<Result<std::unique_ptr<Recorder>>(const CaptureSource& source, std::shared_ptr<TrackWriter> writer)> record;
    std::function<Result<json>(const json& params)> transcribe;  // the stt worker: {tracks: [paths]} -> {language, segments}
    std::function<bool()> models_installed;  // without loading them
    std::function<bool()> microphone_present;
    std::function<std::string()> language;           // the chosen dictation language; "" or "auto": detect (D5)
    std::function<std::string()> fallback_language;  // when detection finds an unsupported language: the window's
    std::function<void(const json& event)> changed;  // dictation.changed {state}
    long long limit_ms = 5 * 60 * 1000;
};

class DictationService {
public:
    explicit DictationService(DictationEnv env);
    ~DictationService();  // a running recording is discarded
    DictationService(const DictationService&) = delete;
    DictationService& operator=(const DictationService&) = delete;

    // Deletes the audio of a dictation the client did not finish (it ended while recording).
    void remove_leftovers();
    // Fails with policy_denied, busy (a meeting capture or another dictation), models_missing, no_microphone or audio.
    Result<json> start();  // {started_at}
    // Stops the recording if it still runs, transcribes it and deletes the audio: {text, language}.
    Result<json> stop();
    Status cancel();
    // Once a second: stops a recording at the limit and tells the shell (state "limit"); discards it when policy turns
    // dictation off (state "discarded").
    void tick();
    // {enabled, available, reason, state, started_at}; reason: models_missing, no_microphone, meeting or "".
    json status();
    // Drops the cached microphone and model checks so that a speech setup shows as ready at once (speech-models: readiness refresh).
    void refresh_models();

private:
    enum class State { idle, recording, stopped, transcribing };
    void halt();     // stops the recorder and closes the track (mx_ held)
    void discard();  // deletes the file and goes idle (mx_ held)
    std::string reason();

    DictationEnv env_;
    std::mutex mx_;
    State state_ = State::idle;
    std::unique_ptr<Recorder> recorder_;
    std::shared_ptr<TrackWriter> writer_;
    std::filesystem::path file_;
    long long started_at_ = 0;
    long long checked_at_ = -1;  // the microphone and the models, at most every 30 seconds
    bool microphone_ = false, models_ = false;
};

}  // namespace wb::meeting
