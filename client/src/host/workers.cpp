#include "host/workers.h"

#include "platform/crash.h"
#include "platform/log.h"
#include "platform/pipe.h"
#include "platform/win.h"

#include <windows.h>

#include <atomic>
#include <thread>

namespace wb {
namespace {

constexpr char kLog[] = "workers";
constexpr ULONG_PTR kQuitKey = 1;
constexpr ULONG_PTR kJobKey = 2;

}  // namespace

const char* to_string(WorkerRole r) {
    switch (r) {
    case WorkerRole::stt: return "stt";
    case WorkerRole::index: return "index";
    case WorkerRole::extract: return "extract";
    }
    return "unknown";
}

std::uint64_t worker_budget_bytes(WorkerRole r) {
    constexpr std::uint64_t mb = 1024ull * 1024ull;
    switch (r) {
    case WorkerRole::stt: return 2048 * mb;
    case WorkerRole::index: return 512 * mb;
    case WorkerRole::extract: return 1024 * mb;
    }
    return 512 * mb;
}

struct WorkerHost::Worker {
    unique_handle process;
    unique_handle job;
    unique_handle port;
    unique_handle to_worker;
    unique_handle from_worker;
    unique_handle drained;  // set when the monitor has seen the job become empty
    std::thread monitor;
    std::atomic<bool> memory_exceeded{false};
    std::uint64_t next_id = 1;
    std::optional<crash::DumpSlot> dump;  // the file the worker writes a crash dump into
    std::function<void(const std::filesystem::path&)> on_dump;

    ~Worker() {
        to_worker.reset();  // EOF tells the worker to exit
        if (process && ::WaitForSingleObject(process.get(), 2000) != WAIT_OBJECT_0)
            ::TerminateProcess(process.get(), 1);
        if (process) ::WaitForSingleObject(process.get(), 2000);
        job.reset();  // kill-on-close
        if (port) ::PostQueuedCompletionStatus(port.get(), 0, kQuitKey, nullptr);
        if (monitor.joinable()) monitor.join();
        if (dump)
            if (auto written = crash::take_dump(*dump); written && on_dump) on_dump(*written);
    }

    [[nodiscard]] bool alive() const { return process && ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT; }

    [[nodiscard]] DWORD exit_code() const {
        DWORD code = 0;
        ::GetExitCodeProcess(process.get(), &code);
        return code;
    }
};

WorkerHost::WorkerHost(Options options) : opt_(std::move(options)) {}

WorkerHost::~WorkerHost() { stop_all(); }

void WorkerHost::stop_all() {
    std::map<WorkerRole, std::unique_ptr<Worker>> gone;
    {
        const std::lock_guard lock(mx_);
        gone.swap(workers_);
    }
}

bool WorkerHost::running(WorkerRole role) {
    const std::lock_guard lock(mx_);
    const auto it = workers_.find(role);
    return it != workers_.end() && it->second->alive();
}

unsigned long WorkerHost::pid(WorkerRole role) {
    const std::lock_guard lock(mx_);
    const auto it = workers_.find(role);
    return it != workers_.end() && it->second->alive() ? ::GetProcessId(it->second->process.get()) : 0;
}

Result<WorkerHost::Worker*> WorkerHost::ensure(WorkerRole role) {
    {
        const std::lock_guard lock(mx_);
        auto it = workers_.find(role);
        if (it != workers_.end() && it->second->alive()) return it->second.get();
    }
    std::unique_ptr<Worker> old;
    {
        const std::lock_guard lock(mx_);
        if (auto it = workers_.find(role); it != workers_.end()) {
            old = std::move(it->second);
            workers_.erase(it);
        }
    }
    old.reset();

    auto w = std::make_unique<Worker>();
    w->job.reset(::CreateJobObjectW(nullptr, nullptr));
    if (!w->job) return make_error("worker_start", "CreateJobObject failed: " + error_text(::GetLastError()));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                          JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    const auto ov = opt_.budget_override.find(role);
    li.ProcessMemoryLimit = static_cast<SIZE_T>(ov != opt_.budget_override.end() ? ov->second : worker_budget_bytes(role));
    if (!::SetInformationJobObject(w->job.get(), JobObjectExtendedLimitInformation, &li, sizeof li))
        return make_error("worker_start", "job limits: " + error_text(::GetLastError()));
    if (role == WorkerRole::index || role == WorkerRole::stt) {
        // Indexing (add-knowledge-graph design D7) and transcription (add-meeting-capture design D4) are background
        // work: CPU rate control caps them at half the machine.
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = 5000;  // hundredths of a percent
        if (!::SetInformationJobObject(w->job.get(), JobObjectCpuRateControlInformation, &cpu, sizeof cpu))
            log::warn(kLog, std::string(to_string(role)) + " worker CPU rate control: " + error_text(::GetLastError()));
    }
    w->port.reset(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp{};
    acp.CompletionKey = reinterpret_cast<PVOID>(kJobKey);
    acp.CompletionPort = w->port.get();
    if (!w->port || !::SetInformationJobObject(w->job.get(), JobObjectAssociateCompletionPortInformation, &acp, sizeof acp))
        return make_error("worker_start", "job port: " + error_text(::GetLastError()));
    w->drained.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE child_in = nullptr, parent_out = nullptr, parent_in = nullptr, child_out = nullptr;
    if (!::CreatePipe(&child_in, &parent_out, &sa, 0) || !::CreatePipe(&parent_in, &child_out, &sa, 0))
        return make_error("worker_start", "CreatePipe failed");
    unique_handle ci(child_in), co(child_out);
    w->to_worker.reset(parent_out);
    w->from_worker.reset(parent_in);
    ::SetHandleInformation(parent_out, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(parent_in, HANDLE_FLAG_INHERIT, 0);

    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    ::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    // Crash Reports: a dump file of its own, while crash reports are enabled.
    if (const auto dir = opt_.crash_dir ? opt_.crash_dir() : std::nullopt) {
        if (auto slot = crash::open_dump_slot(*dir, std::string("worker-") + to_string(role))) {
            w->dump = std::move(*slot);
            if (opt_.on_crash) w->on_dump = [cb = opt_.on_crash, role](const std::filesystem::path& p) { cb(p, std::string("worker-") + to_string(role)); };
        } else {
            log::warn(kLog, slot.error().message);
        }
    }
    // Reserved up front: a vector built from two handles and grown by push_back trips GCC 16's -Warray-bounds at -O2.
    std::vector<HANDLE> inherit;
    inherit.reserve(3);
    inherit.push_back(child_in);
    inherit.push_back(child_out);
    if (w->dump) inherit.push_back(w->dump->file.get());
    ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(), inherit.size() * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = nullptr;
    si.lpAttributeList = attrs;
    std::wstring cmd = L"\"" + opt_.worker_exe.wstring() + L"\" --role " + widen(to_string(role)) + L" --idle-exit " +
                       std::to_wstring(opt_.idle_exit_seconds);
    if (w->dump) cmd += L" " + crash::dump_argument(*w->dump);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(opt_.worker_exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                                         (role == WorkerRole::index || role == WorkerRole::stt ? BELOW_NORMAL_PRIORITY_CLASS : 0),
                                     nullptr,
                                     nullptr, &si.StartupInfo, &pi);
    ::DeleteProcThreadAttributeList(attrs);
    if (!ok) return make_error("worker_start", "CreateProcess failed: " + error_text(::GetLastError()));
    unique_handle thread(pi.hThread);
    w->process.reset(pi.hProcess);
    if (!::AssignProcessToJobObject(w->job.get(), pi.hProcess)) {
        ::TerminateProcess(pi.hProcess, 1);
        return make_error("worker_start", "AssignProcessToJobObject failed: " + error_text(::GetLastError()));
    }
    ::ResumeThread(pi.hThread);

    Worker* raw = w.get();
    w->monitor = std::thread([raw] {
        for (;;) {
            DWORD msg = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED ov_ptr = nullptr;
            if (!::GetQueuedCompletionStatus(raw->port.get(), &msg, &key, &ov_ptr, INFINITE)) return;
            if (key == kQuitKey) return;
            if (msg == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT || msg == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT) {
                raw->memory_exceeded = true;
                ::TerminateProcess(raw->process.get(), kWorkerResourceLimitExit);
            } else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
                ::SetEvent(raw->drained.get());
            }
        }
    });
    log::info(kLog, std::string("started ") + to_string(role) + " worker pid " + std::to_string(pi.dwProcessId));

    const std::lock_guard lock(mx_);
    auto& slot = workers_[role];
    slot = std::move(w);
    return slot.get();
}

Result<json> WorkerHost::call(WorkerRole role, const std::string& method, const json& params) {
    static std::mutex role_mx[3];
    const std::lock_guard role_lock(role_mx[static_cast<int>(role)]);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto w = ensure(role);
        if (!w) return w.error();
        Worker* worker = *w;
        const std::uint64_t id = worker->next_id++;
        std::string reply;
        const bool sent = pipe_write_frame(worker->to_worker.get(), json{{"id", id}, {"method", method}, {"params", params}}.dump());
        if (sent && pipe_read_frame(worker->from_worker.get(), reply)) {
            auto j = parse_json(reply);
            if (!j || !j->is_object() || jint(*j, "id") != static_cast<std::int64_t>(id))
                return make_error("worker_failed", "malformed worker reply");
            if (const auto e = j->find("error"); e != j->end() && e->is_object())
                return make_error(jstr(*e, "code"), jstr(*e, "message"));
            return j->contains("result") ? (*j)["result"] : json(nullptr);
        }
        // The worker is gone: either it idled out just before the request (retry once) or the job failed.
        ::WaitForSingleObject(worker->process.get(), 5000);
        ::WaitForSingleObject(worker->drained.get(), 2000);
        const DWORD code = worker->exit_code();
        const bool over_budget = worker->memory_exceeded || code == kWorkerResourceLimitExit;
        {
            std::unique_ptr<Worker> gone;
            const std::lock_guard lock(mx_);
            if (auto it = workers_.find(role); it != workers_.end()) {
                gone = std::move(it->second);
                workers_.erase(it);
            }
        }
        if (over_budget) {
            log::warn(kLog, std::string(to_string(role)) + " worker exceeded its memory budget and was terminated");
            return make_error("resource_limit", std::string("the ") + to_string(role) + " worker exceeded its memory budget");
        }
        if (code == 0 && !sent && attempt == 0) continue;
        if (code == 0 && attempt == 0 && reply.empty()) continue;
        return make_error("worker_failed", std::string(to_string(role)) + " worker exited with " + error_text(code));
    }
    return make_error("worker_failed", "worker unavailable");
}

}  // namespace wb
