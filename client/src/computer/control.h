// The agent's control of the desktop (client/computer-use Control Overlay, Global Stop Hotkey and User Input Takes
// Precedence; add-computer-use design D4, D6). While a ControlSession lives, a dedicated thread with its own message
// loop shows the click-through overlay (excluded from capture), holds the global stop hotkey and runs low-level mouse
// and keyboard hooks that tell the user's input from the agent's (injected) input. One session at a time.
#pragma once

#include "base/result.h"
#include "computer/classify.h"
#include "computer/uia.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wb::computer {

struct ControlCallbacks {
    std::function<void()> stop;        // the stop hotkey was pressed
    std::function<void()> user_input;  // the user moved the mouse, clicked, scrolled or typed
};

class ControlSession {
public:
    // Starts the thread, shows the overlay and registers the hotkey. Fails when the hotkey is taken or another session
    // is active.
    static Result<std::unique_ptr<ControlSession>> start(const Hotkey& hotkey, ControlCallbacks callbacks);
    ~ControlSession();
    ControlSession(const ControlSession&) = delete;
    ControlSession& operator=(const ControlSession&) = delete;

    // What the overlay shows: the banner text and the target window's frame.
    void show(const std::wstring& banner, const Box& target);
    [[nodiscard]] HWND overlay() const { return overlay_.load(); }
    [[nodiscard]] bool overlay_visible() const;
    [[nodiscard]] const Hotkey& hotkey() const { return hotkey_; }

    // Tests: what the hooks do on physical input, without physical input.
    void simulate_user_input();

private:
    ControlSession(Hotkey hotkey, ControlCallbacks callbacks);
    void run(std::promise<Status>* ready);
    void paint(HWND hwnd);
    void fire(const std::function<void()>& cb);
    static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK keyboard_hook(int code, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    Hotkey hotkey_;
    ControlCallbacks callbacks_;
    std::thread thread_;
    DWORD thread_id_ = 0;
    std::atomic<HWND> overlay_{nullptr};
    DWORD last_user_fire_ = 0;  // the user-input callback fires at most twice a second (session thread only)
    std::mutex mx_;
    std::wstring banner_;
    Box target_;
    // Mouse travel of the user (hook thread only).
    POINT last_seen_{LONG_MIN, LONG_MIN};
    long travel_ = 0;
    DWORD last_travel_ = 0;
};

}  // namespace wb::computer
