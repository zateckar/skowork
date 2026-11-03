// A CDP connection over the two pipe handles inherited by Chrome (add-browser-use design D1). Responses are matched
// by id; events are delivered on a worker thread of their own, so a handler may itself make calls.
#pragma once

#include "base/result.h"
#include "browser/cdp_frame.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wb::browser {

class CdpConnection {
public:
    using EventFn = std::function<void(const CdpMessage&)>;

    // Takes ownership of both handles (HANDLE): `to_chrome` is written, `from_chrome` read. `on_closed` runs once when
    // the pipe breaks (Chrome exited) or close() is called.
    CdpConnection(void* to_chrome, void* from_chrome, EventFn on_event, std::function<void()> on_closed = {});
    ~CdpConnection();
    CdpConnection(const CdpConnection&) = delete;
    CdpConnection& operator=(const CdpConnection&) = delete;

    // Sends a command and waits for its response. Errors: "closed", "timeout", "cdp" (the protocol error message).
    Result<json> call(const std::string& method, const json& params = json::object(), const std::string& session = {},
                      std::chrono::milliseconds timeout = std::chrono::seconds(30));
    // Sends a command without waiting for the response.
    void send(const std::string& method, const json& params = json::object(), const std::string& session = {});

    [[nodiscard]] bool alive() const { return alive_.load(); }
    void close();

private:
    struct Pending {
        bool done = false;
        std::string error;
        json result;
    };
    bool write(const std::string& bytes);
    void read_loop();
    void event_loop();
    void mark_closed();

    void* out_ = nullptr;
    void* in_ = nullptr;
    EventFn on_event_;
    std::function<void()> on_closed_;
    std::atomic<bool> alive_{true};
    std::atomic<long long> next_id_{1};
    std::mutex write_mx_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::map<long long, std::shared_ptr<Pending>> pending_;
    std::deque<CdpMessage> events_;
    bool stopping_ = false;
    std::once_flag closed_once_;
    std::atomic<void*> reader_thread_{nullptr};  // Win32 handle of the reader thread, for CancelSynchronousIo
    std::atomic<bool> reader_done_{false};
    std::thread reader_, dispatcher_;
};

}  // namespace wb::browser
