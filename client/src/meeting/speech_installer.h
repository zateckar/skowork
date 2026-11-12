#pragma once
// client/speech-models: runs speech setup (download or offline import) on one background thread, probes the installed
// engine in the stt worker and reports the state for Settings (task 5.4 will call these through speech.* RPCs).
#include "base/json.h"
#include "base/result.h"
#include "meeting/speech_package.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace wb::meeting {

class SpeechInstaller {
public:
    struct Env {
        std::filesystem::path data_root;
        // The catalog of the current Policy bundle (client/speech-models Speech Package Catalog), read at each use.
        std::function<SpeechSource()> source;
        std::function<bool()> download_allowed;  // policy.speech.download
        SpeechLimits limits;
        std::function<Status(const std::filesystem::path& speech_dir)> probe;  // the stt worker initializes the engine there
        std::function<bool()> in_use;                  // a capture or transcription uses the active package
        std::function<void(const json& event)> changed;  // speech.changed {state, version, error}
        std::function<void()> activated;                 // a package became ready: refresh Dictation availability
    };

    explicit SpeechInstaller(Env env);
    ~SpeechInstaller();  // cancels a running setup and waits for it
    SpeechInstaller(const SpeechInstaller&) = delete;
    SpeechInstaller& operator=(const SpeechInstaller&) = delete;

    // Each fails with busy while a setup runs. A download fails with unavailable when this build has no source.
    Status start_download();
    Status start_import(const std::filesystem::path& archive);
    void cancel();
    // Removes a version (deferred while in use). Fails with busy while a setup runs.
    Result<bool> remove(const std::string& version);
    // {state, installed, version, error}; state: idle, downloading, verifying, probing, ready, failed or cancelled.
    json status();

private:
    Status run(std::function<Result<std::string>(const std::shared_ptr<CancelToken>&)> work, const char* first_state);
    void emit(const std::string& state, const std::string& version, const std::string& error);

    Env env_;
    std::mutex mx_;
    std::thread thread_;
    bool running_ = false;
    std::string state_ = "idle", version_, error_;
    std::shared_ptr<CancelToken> cancel_;
    std::atomic<std::uint64_t> progress_step_{0};
};

}  // namespace wb::meeting
