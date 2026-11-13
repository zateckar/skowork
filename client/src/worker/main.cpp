// skowork-worker.exe: runs CPU-heavy or risky background work for the core process (client/desktop-host Worker
// Process Lifetime). Requests arrive as length-prefixed JSON frames on stdin; replies go to stdout. The worker
// exits after `--idle-exit` seconds without a request, and when stdin closes.
#include "base/json.h"
#include "platform/crash.h"
#include "platform/pipe.h"
#include "worker/handlers.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <new>
#include <string>
#include <thread>

namespace {

std::atomic<std::int64_t> g_last_activity{0};
std::atomic<bool> g_busy{false};

std::int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // An allocation failure means the job's memory budget was hit (client/desktop-host Worker Memory Budgets).
    std::set_new_handler([] { ::ExitProcess(0xE0000001); });
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::string role, kind;
    long idle_exit = 60;
    HANDLE dump = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wcscmp(argv[i], L"--role") == 0) role = wb::narrow_ascii(argv[++i]);
        else if (std::wcscmp(argv[i], L"--idle-exit") == 0) idle_exit = std::wcstol(argv[++i], nullptr, 10);
        else if (std::wcscmp(argv[i], L"--kind") == 0) kind = wb::narrow_ascii(argv[++i]);
        else if (std::wcscmp(argv[i], L"--crash-dump") == 0) dump = wb::crash::parse_dump_argument(argv[++i]);
    }
    ::LocalFree(argv);
    if (role.empty()) return 2;
    // Crash Reports (client/telemetry): the core passes a dump file only while crash reports are enabled.
    if (dump) wb::crash::install_to_handle(dump);

    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    // Document Text Extraction (client/core-tools): one document per process, started by the tool sandbox; the
    // document arrives on stdin and its text leaves on stdout.
    if (role == "extract" && !kind.empty()) return wb::worker::extract_once(kind, in, out, ::GetStdHandle(STD_ERROR_HANDLE));
    g_last_activity = now_s();
    std::thread idle([idle_exit] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!g_busy && now_s() - g_last_activity >= idle_exit) ::ExitProcess(0);
        }
    });
    idle.detach();

    std::string frame;
    while (wb::pipe_read_frame(in, frame)) {
        g_busy = true;
        g_last_activity = now_s();
        wb::json reply;
        auto req = wb::parse_json(frame);
        if (!req || !req->is_object()) {
            reply = {{"id", nullptr}, {"error", {{"code", "bad_request"}, {"message", "malformed request"}}}};
        } else {
            const wb::json id = req->contains("id") ? (*req)["id"] : wb::json(nullptr);
            const wb::json params = req->contains("params") ? (*req)["params"] : wb::json::object();
            auto result = wb::worker::dispatch(role, wb::jstr(*req, "method"), params);
            reply = result ? wb::json{{"id", id}, {"result", *result}}
                           : wb::json{{"id", id}, {"error", {{"code", result.error().code}, {"message", result.error().message}}}};
        }
        const bool ok = wb::pipe_write_frame(out, reply.dump());
        g_last_activity = now_s();
        g_busy = false;
        if (!ok) break;
    }
    return 0;
}
