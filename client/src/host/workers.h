// Worker processes (client/desktop-host Worker Process Lifetime and Worker Memory Budgets). The core starts
// `workbench-worker.exe --role <role>` on demand inside a Job Object that kills it with the core and caps its
// memory; requests are length-prefixed JSON frames over the worker's stdin/stdout. A worker exits on its own
// after 60 s without a request.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace wb {

enum class WorkerRole { stt, index, extract };

const char* to_string(WorkerRole r);
std::uint64_t worker_budget_bytes(WorkerRole r);  // 2 GB, 512 MB, 1 GB

inline constexpr unsigned kWorkerIdleExitSeconds = 60;
inline constexpr unsigned long kWorkerResourceLimitExit = 0xE0000001;

class WorkerHost {
public:
    struct Options {
        std::filesystem::path worker_exe;
        unsigned idle_exit_seconds = kWorkerIdleExitSeconds;  // passed to the worker; tests shorten it
        std::map<WorkerRole, std::uint64_t> budget_override;  // tests only
        // Crash Reports (client/telemetry): the folder for worker dumps while crash reports are enabled (nullopt: none),
        // and where a dump goes once its worker has ended.
        std::function<std::optional<std::filesystem::path>()> crash_dir;
        std::function<void(const std::filesystem::path& dump, const std::string& process)> on_crash;
    };

    explicit WorkerHost(Options options);
    ~WorkerHost();
    WorkerHost(const WorkerHost&) = delete;
    WorkerHost& operator=(const WorkerHost&) = delete;

    // Runs one request in the role's worker, starting it when needed. Blocks; call from a pool thread.
    // Errors: "resource_limit" when the worker exceeded its memory budget, "worker_failed", "worker_start".
    Result<json> call(WorkerRole role, const std::string& method, const json& params);

    [[nodiscard]] bool running(WorkerRole role);
    [[nodiscard]] unsigned long pid(WorkerRole role);
    void stop_all();

private:
    struct Worker;
    Result<Worker*> ensure(WorkerRole role);

    Options opt_;
    std::mutex mx_;
    std::map<WorkerRole, std::unique_ptr<Worker>> workers_;
};

}  // namespace wb
