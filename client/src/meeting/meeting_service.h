// client/meeting-capture (add-meeting-capture): the policy gate, detection with the calendar match, the consent and
// participant-notice Questions, capture into encrypted tracks with the recording indicator, the transcription queue
// in the stt worker, the outputs Run in the meeting Thread, knowledge indexing, retention and speaker renaming.
// Everything outside this module comes in through MeetingEnv so tests can drive it with fakes and a fake clock.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "meeting/audio_file.h"
#include "meeting/core.h"
#include "meeting/meeting_store.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wb::meeting {

// A running capture stream (WASAPI in the app, synthetic audio in tests). stop() finalises its track.
class Recorder {
public:
    virtual ~Recorder() = default;
    virtual void stop() = 0;
};

struct MeetingEnv {
    const Clock* clock = &system_clock();
    PolicyLookup policy;
    std::filesystem::path capture_dir;  // <data root>/capture
    std::function<Observation()> observe;
    std::function<Result<std::unique_ptr<Recorder>>(const CaptureSource& source, std::shared_ptr<TrackWriter> writer)> record;
    // Shows the recording indicator; the returned handle hides it when destroyed.
    std::function<std::shared_ptr<void>(const std::string& label, std::function<void()> on_stop)> indicator;
    // Calendar events overlapping [from, to) from the mail-calendar local copy.
    std::function<std::vector<CalendarEvent>(long long from, long long to)> calendar;
    // Creates the meeting-origin Thread (title, origin ref {meeting_id, title, link}) and returns its id.
    std::function<Result<std::string>(const std::string& title, const json& origin_ref)> create_thread;
    std::function<void(const std::string& thread_id)> delete_thread;
    // Raises a standalone Question (capability "meeting") in the Thread; returns the Action item id.
    std::function<Result<std::string>(const std::string& thread_id, const std::string& prompt,
                                      const std::vector<std::string>& choices, const std::string& copy)> ask;
    std::function<void(const std::string& action_id, const std::string& reason)> withdraw;
    // The stt worker: {tracks: [paths], language?} -> {language, segments}.
    std::function<Result<json>(const json& params)> transcribe;
    // The outputs Run: an Unattended run in the Thread whose message is untrusted content with the meeting as provenance.
    std::function<Result<std::string>(const std::string& thread_id, const std::string& meeting_id, const std::string& message,
                                      const std::string& preamble)> start_outputs;
    // "running" while the Run has not ended; "succeeded", "failed", "cancelled" after.
    std::function<std::string(const std::string& run_id)> run_state;
    // The map step for long transcripts (the main role): notes on one part.
    std::function<Result<std::string>(const std::string& part)> summarize_part;
    // client/knowledge-graph, Source meetings (it applies its own policy.knowledge.sources.meetings).
    std::function<void(const MeetingRecord& m, const json& turns)> knowledge;
    std::function<void(const std::string& meeting_id)> knowledge_removed;
    // "meeting detected" / "transcript ready" for client/scheduler (Meeting Events).
    std::function<void(const std::string& event, const json& meeting)> publish;
    // Capture became available or unavailable (the scheduler's meeting source, the Settings section).
    std::function<void(bool available)> availability;
    std::function<bool()> deferred;          // client/desktop-host defers background work
    std::function<std::string()> user_name;  // the local track's speaker
    std::function<std::string()> language;   // the UI language, for the notice before the meeting language is known
    std::function<void()> changed;           // the Settings section shows something new
    bool background = true;                  // false (tests): no threads; call tick() and run_transcriptions()
};

class MeetingService {
public:
    MeetingService(MeetingEnv env, std::unique_ptr<MeetingStore> store);
    ~MeetingService();
    MeetingService(const MeetingService&) = delete;
    MeetingService& operator=(const MeetingService&) = delete;

    void start();
    void stop();
    // Re-reads policy.meeting_capture (Disabled Unless Enabled By Policy): turning it off stops a running capture and
    // deletes its audio, withdraws open Questions and stops detection.
    void policy_changed();
    [[nodiscard]] bool available();
    [[nodiscard]] bool capturing();  // a capture runs (client/dictation waits for it)
    [[nodiscard]] Config config();

    // One detector step (every 2 seconds), Question expiry, the end of the outputs Run, the retention sweep.
    void tick();
    // Transcribes queued meetings that may run now (not deferred, or started by the user).
    void run_transcriptions();

    // The answer to a standalone Question of capability "meeting".
    void on_answer(const std::string& action_id, const std::string& answer);

    // ---- the Meetings settings section ----
    [[nodiscard]] json status();
    Status stop_capture();  // the indicator's Stop, or Stop in Settings
    // Manual Audio Source Capture: "app:<pid>", "system" or "mic". Starting it counts as consent.
    Status capture_manual(const std::string& source);
    Status transcribe_now(const std::string& meeting_id);
    Status rename_speaker(const std::string& meeting_id, const std::string& speaker, const std::string& name);
    Status remove_series(const std::string& series_id);
    Status forget(const std::string& meeting_id);  // the meeting, its audio and its knowledge item
    [[nodiscard]] json audio_sources();

    MeetingStore& store() { return *store_; }

private:
    struct Capture {
        std::string meeting_id;
        std::vector<std::unique_ptr<Recorder>> recorders;
        std::vector<std::shared_ptr<TrackWriter>> writers;
        std::shared_ptr<void> indicator;
        long long live_at = 0;  // the last live transcription
    };

    long long now() const { return env_.clock->unix_millis(); }
    void detected(const AppMatch& app);
    void ended(const AppMatch& app);
    void raise_consent(MeetingRecord& m);
    void after_consent(MeetingRecord& m);
    void raise_notice(MeetingRecord& m);
    Status begin_capture(MeetingRecord& m, const CaptureSource& source, bool with_mic);
    void finish_capture(bool discard, const std::string& reason);
    void decline(MeetingRecord& m, const std::string& reason);
    void transcribe_one(MeetingRecord m);
    void start_outputs(MeetingRecord& m);
    void complete(MeetingRecord& m);
    void delete_audio(MeetingRecord& m);
    void sweep();
    void live_step();
    void publish(const std::string& event, const MeetingRecord& m);
    void notify();
    json meeting_json(const MeetingRecord& m);

    MeetingEnv env_;
    std::unique_ptr<MeetingStore> store_;
    std::recursive_mutex mx_;
    Config cfg_;
    bool available_ = false;
    std::unique_ptr<Detector> detector_;
    std::unique_ptr<Capture> capture_;
    std::map<std::string, long long> asked_at_;  // Action item -> raised (2-minute expiry)
    long long last_sweep_ = 0;
    bool reported_ = false;  // availability was reported once
    std::atomic<bool> stop_requested_{false};  // the indicator's Stop

    std::thread ticker_, transcriber_;
    std::mutex wake_mx_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> transcribing_{false};
};

}  // namespace wb::meeting
