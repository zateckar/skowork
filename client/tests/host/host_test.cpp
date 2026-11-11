// client/desktop-host: tray state rules, background pause and deferral, per-user registration, workers,
// launch arguments and the RPC registry.
#include "support/dev_endpoints.h"
#include "app/app.h"
#include "host/dispatch.h"
#include "host/power.h"
#include "host/registration.h"
#include "host/rpc.h"
#include "host/tray.h"
#include "host/workers.h"
#include "platform/crypto.h"
#include "platform/registry.h"
#include "platform/win.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <thread>

using namespace wb;
using namespace std::chrono_literals;

namespace {

class FakeClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override { return t; }
    sys_clock::time_point t = sys_clock::time_point(std::chrono::seconds(1'800'000'000));
};

std::filesystem::path worker_exe() { return exe_dir() / L"workbench-worker.exe"; }

bool process_alive(unsigned long pid) {
    unique_handle h(::OpenProcess(SYNCHRONIZE, FALSE, pid));
    return h && ::WaitForSingleObject(h.get(), 0) == WAIT_TIMEOUT;
}

}  // namespace

TEST(Tray, StateRules) {
    EXPECT_EQ(compute_tray_state(false, false, 0, 0), TrayState::disconnected);
    EXPECT_EQ(compute_tray_state(true, false, 0, 0), TrayState::idle);
    EXPECT_EQ(compute_tray_state(true, false, 0, 2), TrayState::running);
    EXPECT_EQ(compute_tray_state(true, true, 0, 2), TrayState::paused);
    EXPECT_EQ(compute_tray_state(false, true, 3, 0), TrayState::action_required);
    EXPECT_NE(tray_tooltip(TrayState::action_required, 3, 0).find(L"3 items"), std::wstring::npos);
    EXPECT_NE(tray_tooltip(TrayState::action_required, 1, 0).find(L"1 item needs"), std::wstring::npos);
}

TEST(Tray, IconReflectsEachState) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WorkbenchTrayTest";
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    ASSERT_NE(hwnd, nullptr);
    {
        Tray tray(hwnd, {});
        EXPECT_EQ(tray.state(), TrayState::disconnected);
        for (auto s : {TrayState::idle, TrayState::running, TrayState::action_required, TrayState::paused,
                       TrayState::disconnected}) {
            tray.set(s, s == TrayState::action_required ? 2 : 0, s == TrayState::running ? 1 : 0, s == TrayState::paused);
            EXPECT_EQ(tray.state(), s);
            EXPECT_EQ(tray.tooltip(), tray_tooltip(s, s == TrayState::action_required ? 2 : 0, s == TrayState::running ? 1 : 0));
        }
        EXPECT_TRUE(tray.confirm_quit());  // no active Runs: no prompt
    }
    ::DestroyWindow(hwnd);
}

// client/desktop-host App Icon (improve-window-frame D4): the tray shows the app icon with a dot in the state's colour in
// the bottom-right corner (hollow when paused) and the red unread badge in the top-right corner; without the app icon it
// is the disc in the state's colour.
TEST(Tray, AppIconWithStateDot) {
    const int size = 32;
    const std::vector<std::uint32_t> base(size * size, 0xFF0E3A2Fu);  // an opaque Emerald square
    const auto at = [size](const std::vector<std::uint32_t>& img, int x, int y) { return img[static_cast<size_t>(y * size + x)]; };
    const double r = size * 0.24, c = size - r - 0.5;
    const int dot = static_cast<int>(c), hole_edge = static_cast<int>(c + r * 0.75);
    for (auto s : {TrayState::disconnected, TrayState::idle, TrayState::running, TrayState::action_required, TrayState::paused}) {
        const auto img = tray_image(s, false, base, size);
        ASSERT_EQ(img.size(), base.size());
        EXPECT_EQ(at(img, 4, 4), 0xFF0E3A2Fu) << to_string(s);  // the icon stays
        if (s == TrayState::paused) {
            EXPECT_EQ(at(img, dot, dot), 0xFF131A17u);  // hollow: the dark rim shows through
            EXPECT_EQ(at(img, hole_edge, dot), tray_colour(s));
        } else {
            EXPECT_EQ(at(img, dot, dot), tray_colour(s)) << to_string(s);
        }
        // The unread badge: red in the top-right corner.
        const auto badged = tray_image(s, true, base, size);
        EXPECT_EQ(at(badged, dot, size - 1 - dot), 0xFFD13438u) << to_string(s);
    }
    // Without the app icon: the state's disc, transparent in the corners.
    const auto plain = tray_image(TrayState::running, false, {}, size);
    EXPECT_EQ(at(plain, size / 2, size / 2), tray_colour(TrayState::running));
    EXPECT_EQ(at(plain, 0, 0) >> 24, 0u);
}

// client/user-messaging Chats View: the tray icon shows the total unread count.
TEST(Tray, UnreadMessagesBadgeAndTooltip) {
    EXPECT_NE(tray_tooltip(TrayState::idle, 0, 0, false, 3).find(L"\n3 unread messages"), std::wstring::npos);
    EXPECT_NE(tray_tooltip(TrayState::idle, 0, 0, false, 1).find(L"\n1 unread message"), std::wstring::npos);
    EXPECT_EQ(tray_tooltip(TrayState::idle, 0, 0, false, 0).find(L"unread"), std::wstring::npos);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WorkbenchTrayUnreadTest";
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    ASSERT_NE(hwnd, nullptr);
    {
        Tray tray(hwnd, {});
        tray.set(TrayState::idle, 0, 0, false, false, 4);
        EXPECT_EQ(tray.unread_messages(), 4);
        EXPECT_EQ(tray.tooltip(), tray_tooltip(TrayState::idle, 0, 0, false, 4));
        tray.set(TrayState::idle, 0, 0, false, false, 0);
        EXPECT_EQ(tray.unread_messages(), 0);
    }
    ::DestroyWindow(hwnd);
}

TEST(BackgroundGate, PauseCoversAllBackgroundWorkForTheChosenDuration) {
    FakeClock clock;
    BackgroundGate gate(clock);
    EXPECT_TRUE(gate.may_run(WorkKind::indexing));
    gate.pause_for(3600);
    EXPECT_TRUE(gate.paused());
    for (auto k : {WorkKind::indexing, WorkKind::transcription, WorkKind::sync, WorkKind::trigger})
        EXPECT_EQ(gate.hold_reason(k), "paused");
    EXPECT_TRUE(gate.may_run(WorkKind::indexing, /*user_started=*/true));
    clock.t += 3601s;
    EXPECT_FALSE(gate.paused());
    EXPECT_TRUE(gate.may_run(WorkKind::trigger));
    gate.pause_for(60);
    gate.resume();
    EXPECT_FALSE(gate.paused());
}

TEST(BackgroundGate, BatterySaverDefersButNotTriggersOrUserWork) {
    FakeClock clock;
    BackgroundGate gate(clock);
    gate.update({.battery_saver = true, .presentation = false});
    EXPECT_EQ(gate.hold_reason(WorkKind::indexing), "battery_saver");
    EXPECT_EQ(gate.hold_reason(WorkKind::transcription), "battery_saver");
    EXPECT_EQ(gate.hold_reason(WorkKind::sync), "battery_saver");
    EXPECT_TRUE(gate.may_run(WorkKind::trigger));
    EXPECT_TRUE(gate.may_run(WorkKind::transcription, true));
    gate.set_background_on_battery(true);  // policy.ui.background_on_battery
    EXPECT_TRUE(gate.may_run(WorkKind::indexing));
}

TEST(BackgroundGate, PresentationDefersAndResumesAutomatically) {
    FakeClock clock;
    BackgroundGate gate(clock);
    gate.update({.battery_saver = false, .presentation = true});
    EXPECT_EQ(gate.hold_reason(WorkKind::indexing), "presentation");
    EXPECT_EQ(gate.hold_reason(WorkKind::sync), "presentation");
    EXPECT_TRUE(gate.may_run(WorkKind::trigger));

    std::atomic<bool> resumed{false};
    std::thread job([&] {
        if (gate.wait_until_allowed(WorkKind::indexing, [] { return false; })) resumed = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(resumed.load());
    gate.update({.battery_saver = false, .presentation = false});
    job.join();
    EXPECT_TRUE(resumed.load());
}

TEST(Registration, WritesAndRepairsForAMovedExecutable) {
    const std::wstring base = L"Software\\WorkbenchTest\\" + widen(random_urlsafe(6));
    RegistrationPaths p;
    p.run_key = base + L"\\Run";
    p.aumid_key = base + L"\\AppUserModelId\\Workbench.Client";
    auto first = ensure_registration(L"C:\\Old\\Workbench.exe", p);
    EXPECT_TRUE(first.ok);
    EXPECT_TRUE(first.run_repaired);
    // The AUMID of the toast notifications (add-work-inbox design D6).
    EXPECT_EQ(reg_read_string(HKEY_CURRENT_USER, p.aumid_key, L"DisplayName"), L"SkoWork");
    auto again = ensure_registration(L"C:\\Old\\Workbench.exe", p);
    EXPECT_FALSE(again.run_repaired);
    auto moved = ensure_registration(L"D:\\New Place\\Workbench.exe", p);
    EXPECT_TRUE(moved.run_repaired);
    EXPECT_EQ(reg_read_string(HKEY_CURRENT_USER, p.run_key, p.run_value), L"\"D:\\New Place\\Workbench.exe\" --background");
    reg_delete_tree(HKEY_CURRENT_USER, L"Software\\WorkbenchTest");
}

TEST(Workers, IdleWorkerExitsAndIsRestartedOnDemand) {
    ASSERT_TRUE(std::filesystem::exists(worker_exe()));
    WorkerHost::Options o;
    o.worker_exe = worker_exe();
    o.idle_exit_seconds = 2;  // production: 60 s
    WorkerHost host(o);
    auto r = host.call(WorkerRole::index, "ping", json::object());
    ASSERT_TRUE(r) << r.error().message;
    const auto pid = host.pid(WorkerRole::index);
    ASSERT_NE(pid, 0u);
    EXPECT_EQ(static_cast<unsigned long>(jint(*r, "pid")), pid);
    std::this_thread::sleep_for(3500ms);
    EXPECT_FALSE(process_alive(pid));
    EXPECT_FALSE(host.running(WorkerRole::index));
    auto r2 = host.call(WorkerRole::index, "ping", json::object());
    ASSERT_TRUE(r2) << r2.error().message;
    EXPECT_NE(host.pid(WorkerRole::index), pid);
}

TEST(Workers, BusyWorkerIsNotReapedAsIdle) {
    WB_REQUIRES_DEV_ENDPOINTS();
    WorkerHost::Options o;
    o.worker_exe = worker_exe();
    o.idle_exit_seconds = 1;
    WorkerHost host(o);
    auto r = host.call(WorkerRole::extract, "sleep", json{{"ms", 2500}});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(jint(*r, "slept"), 2500);
}

TEST(Workers, OverBudgetWorkerIsTerminatedWithResourceLimit) {
    WB_REQUIRES_DEV_ENDPOINTS();
    WorkerHost::Options o;
    o.worker_exe = worker_exe();
    o.budget_override[WorkerRole::extract] = 64ull * 1024 * 1024;  // production: 1 GB
    WorkerHost host(o);
    auto ok = host.call(WorkerRole::extract, "alloc", json{{"mb", 8}});
    ASSERT_TRUE(ok) << ok.error().message;
    const auto pid = host.pid(WorkerRole::extract);
    auto r = host.call(WorkerRole::extract, "alloc", json{{"mb", 200}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "resource_limit");
    EXPECT_FALSE(process_alive(pid));
    // The host keeps working and starts a fresh worker for the next job.
    EXPECT_TRUE(host.call(WorkerRole::extract, "ping", json::object()));
}

TEST(Workers, Budgets) {
    EXPECT_EQ(worker_budget_bytes(WorkerRole::stt), 2048ull << 20);
    EXPECT_EQ(worker_budget_bytes(WorkerRole::index), 512ull << 20);
    EXPECT_EQ(worker_budget_bytes(WorkerRole::extract), 1024ull << 20);
}

TEST(LaunchArgs, BackgroundAndRoute) {
    EXPECT_FALSE(parse_launch_args({}).background);
    EXPECT_TRUE(parse_launch_args({L"--background"}).background);
    auto c = parse_launch_args({L"--route", L"#/about"});
    EXPECT_EQ(c.route, "#/about");
    // Sign-in redirects arrive on the loopback listener, never as launch arguments.
    auto d = parse_launch_args({L"com.example.workbench:/oauth2redirect?code=c&state=s"});
    EXPECT_FALSE(d.background);
    EXPECT_TRUE(d.route.empty());
}

TEST(Rpc, PipeExposureAndErrors) {
    RpcRegistry rpc;
    rpc.add("ui.only", RpcExposure::ui, [](const json&) -> Result<json> { return json{{"x", 1}}; });
    rpc.add("both", RpcExposure::ui_and_pipe, [](const json& p) -> Result<json> { return json{{"echo", p}}; });
    rpc.add("fails", RpcExposure::ui_and_pipe, [](const json&) -> Result<json> { return make_error("nope", "bad"); });
    EXPECT_NE(rpc.find("ui.only", false), nullptr);
    EXPECT_EQ(rpc.find("ui.only", true), nullptr);
    auto ok = parse_json(rpc.handle_pipe_request(R"({"jsonrpc":"2.0","id":5,"method":"both","params":{"a":1}})"));
    ASSERT_TRUE(ok);
    EXPECT_EQ((*ok)["id"], 5);
    EXPECT_EQ((*ok)["result"]["echo"]["a"], 1);
    auto hidden = parse_json(rpc.handle_pipe_request(R"({"jsonrpc":"2.0","id":1,"method":"ui.only"})"));
    EXPECT_EQ((*hidden)["error"]["code"], -32601);
    auto bad = parse_json(rpc.handle_pipe_request("{not json"));
    EXPECT_EQ((*bad)["error"]["code"], -32700);
    auto failed = parse_json(rpc.handle_pipe_request(R"({"jsonrpc":"2.0","id":2,"method":"fails"})"));
    EXPECT_EQ((*failed)["error"]["code"], -32000);
    EXPECT_EQ((*failed)["error"]["data"]["code"], "nope");
}

// Two pools (desktop-host design D5): four blocked background tasks do not delay a UI request.
TEST(HostPools, UiRequestsDoNotWaitBehindBlockedBackgroundWork) {
    ThreadPool background{2};
    ThreadPool ui{4};
    std::atomic<bool> release{false};
    for (int i = 0; i < 4; ++i) background.post([&release] { while (!release) std::this_thread::sleep_for(1ms); });
    std::atomic<bool> answered{false};
    ui.post([&answered] { answered = true; });
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!answered && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(answered);
    release = true;
    background.shutdown();
    ui.shutdown();
}
