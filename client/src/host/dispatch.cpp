#include "host/dispatch.h"

#include <atomic>
#include <memory>

namespace wb {
namespace {

std::atomic<HWND> g_main_window{nullptr};
DWORD g_main_thread_id = 0;

}  // namespace

ThreadPool::ThreadPool(unsigned threads) {
    for (unsigned i = 0; i < threads; ++i)
        threads_.emplace_back([this] {
            for (;;) {
                std::function<void()> fn;
                {
                    std::unique_lock lock(mx_);
                    cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                    if (queue_.empty()) return;
                    fn = std::move(queue_.front());
                    queue_.pop_front();
                }
                fn();
            }
        });
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::post(std::function<void()> fn) {
    {
        const std::lock_guard lock(mx_);
        if (stopping_) return;
        queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    {
        const std::lock_guard lock(mx_);
        if (stopping_ && threads_.empty()) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void MainThread::init(HWND message_window) {
    g_main_window = message_window;
    g_main_thread_id = ::GetCurrentThreadId();
}

void MainThread::post(std::function<void()> fn) {
    HWND w = g_main_window.load();
    if (!w) return;
    auto* heap = new std::function<void()>(std::move(fn));
    if (!::PostMessageW(w, kRunMessage, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
}

void MainThread::run(LPARAM lparam) {
    std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lparam));
    if (fn && *fn) (*fn)();
}

bool MainThread::on_main_thread() { return ::GetCurrentThreadId() == g_main_thread_id; }

}  // namespace wb
