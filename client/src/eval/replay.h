// Replay of one step over an evaluation export (client/evaluation Replay Without Side Effects, Replay Variants, Replay
// Limits And Progress; add-evaluation-export design D4). A replay holds only the model, the speech worker and the export
// folder: it cannot change stored items, results, decisions, Threads, the Agenda, knowledge or live call budgets.
#pragma once

#include "agenda/core.h"
#include "base/json.h"
#include "base/result.h"
#include "eval/registry.h"
#include "model/types.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wb::eval {

inline constexpr int kDefaultReplayItems = 200;
inline constexpr int kMaxReplayItems = 2000;

struct ReplayRequest {
    std::filesystem::path folder;
    Step step = Step::triage;
    json variant = json::object();
    int max_items = kDefaultReplayItems;
};

struct ReplayEnv {
    model::IChatModel* model = nullptr;                    // the same small Model role as the live steps
    std::function<std::string()> responsibilities;           // triage's user setting
    std::function<agenda::Identity()> identity;              // the user, for Agenda detection
    std::function<int(long long)> tz_offset_min;
    // The speech worker: {kind: transcribe|diarize, tracks: [wav paths], language?, settings: {...}} -> {language, segments}
    std::function<Result<json>(const json& request)> speech;
    std::function<bool()> speech_busy;  // a capture or live transcription is running: speech replay pauses
    std::function<std::string()> user_name;
    std::function<long long()> now;
    std::chrono::milliseconds pause_poll{10'000};
};

// Parses and checks a request before anything runs: step, variant and max_items (1 to 2000, default 200).
Result<ReplayRequest> replay_request_from_json(const json& params);

// For diarize.clusters = attendees: the number of attendees other than the user when at least 2, else -1 (automatic).
int cluster_count(const std::string& clusters, const std::vector<std::string>& attendees, const std::string& user_name);

class Replayer {
public:
    explicit Replayer(ReplayEnv env) : env_(std::move(env)) {}
    ~Replayer();
    Replayer(const Replayer&) = delete;
    Replayer& operator=(const Replayer&) = delete;

    // Starts a replay and returns its run id; "busy" while another runs. The variant is checked first.
    Result<std::string> start(const ReplayRequest& r);
    // {run, state: running|paused|done|failed|cancelled, step, done, total, failures, error?}
    [[nodiscard]] json status() const;
    void cancel();
    void wait();  // for tests

private:
    void run(ReplayRequest r, Variant v, std::string id);
    void set(const std::function<void(json&)>& f);
    ReplayEnv env_;
    mutable std::mutex mx_;
    json status_ = json::object();
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    bool running_ = false;
};

}  // namespace wb::eval
