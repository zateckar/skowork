// Windows audio for client/meeting-capture (add-meeting-capture design D2, D3, D6): WASAPI capture of a process tree
// (process loopback), all system output or the default communications microphone into a TrackWriter; the detector's
// probe of audio sessions, microphone use, processes and window titles; and the recording indicator.
#pragma once

#include "base/result.h"
#include "meeting/audio_file.h"
#include "meeting/core.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace wb::meeting {

// One running capture stream. stop() returns once the stream is closed and the track finalised (well under 2 seconds).
class AudioCapture {
public:
    // Opens the stream on the capture thread; fails ("audio") when the device or the process cannot be captured.
    static Result<std::unique_ptr<AudioCapture>> start(const CaptureSource& source, std::shared_ptr<TrackWriter> writer);
    ~AudioCapture();
    void stop();
    [[nodiscard]] std::string error() const;  // a failure after the start, if any

private:
    AudioCapture() = default;
    void run(CaptureSource source, std::shared_ptr<TrackWriter> writer, std::function<void(Status)> started);
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mx_;
    std::string error_;
};

// The detector's view of the machine (design D3). Every call is read only.
Observation observe_machine();
// Image names of processes with an active render session, for manual capture: [{pid, name}].
std::vector<Process> audio_processes();
// An active default communications microphone (client/dictation Dictation Availability).
bool microphone_present();

// The recording indicator: a topmost tool window on its own thread, independent of the main window, with the elapsed
// time and a Stop button.
class Indicator {
public:
    struct Options {
        std::wstring label;               // "Recording: Teams"
        std::wstring stop_text = L"Stop";
        std::optional<std::pair<int, int>> position;  // tests: a fixed screen position
    };
    Indicator(Options o, std::function<void()> on_stop);
    ~Indicator();
    Indicator(const Indicator&) = delete;
    Indicator& operator=(const Indicator&) = delete;
    [[nodiscard]] void* hwnd() const { return hwnd_.load(); }
    [[nodiscard]] void* stop_button() const { return stop_.load(); }

private:
    void run(Options o, std::promise<void>& shown);
    std::thread thread_;
    unsigned long thread_id_ = 0;
    std::atomic<void*> hwnd_{nullptr}, stop_{nullptr};
    std::function<void()> on_stop_;
    long long started_ = 0;
};

}  // namespace wb::meeting
