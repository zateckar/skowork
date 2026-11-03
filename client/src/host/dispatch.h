// Threading helpers (design D10): the main thread runs the Win32 message loop; blocking work runs on a small
// fixed pool; results come back to the main thread through posted messages.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace wb {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void post(std::function<void()> fn);
    void shutdown();  // finishes queued work, then joins

private:
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

// Posts closures to the window that owns the main thread's message loop.
class MainThread {
public:
    static constexpr UINT kRunMessage = WM_APP + 0x40;

    static void init(HWND message_window);
    static void post(std::function<void()> fn);
    // Runs the closure carried by a kRunMessage; call from the window procedure.
    static void run(LPARAM lparam);
    [[nodiscard]] static bool on_main_thread();
};

}  // namespace wb
