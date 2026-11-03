// Named-pipe local API (add-client-foundation design D5): `\\.\pipe\Workbench-<user SID>`, a DACL that grants only
// the current user, PIPE_REJECT_REMOTE_CLIENTS, length-prefixed frames (4-byte little-endian length + UTF-8 JSON).
// The first pipe instance doubles as the single-instance lock.
#pragma once

#include "base/result.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wb {

std::wstring default_pipe_name();

class PipeServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    // Creates the first pipe instance. Fails with code "exists" when another process owns the name.
    static Result<std::unique_ptr<PipeServer>> create(const std::wstring& name);
    ~PipeServer();

    // Starts serving on a background thread. Each connection is served on its own short-lived thread.
    void start(Handler handler);
    void stop();

private:
    explicit PipeServer(std::wstring name) : name_(std::move(name)) {}
    void accept_loop();
    void* make_instance(bool first);

    std::wstring name_;
    Handler handler_;
    void* first_ = nullptr;
    std::thread accept_thread_;
    std::atomic<bool> stopping_{false};
    std::mutex workers_mx_;
    std::vector<std::thread> workers_;
};

// Client side: sends one request and waits for one response.
Result<std::string> pipe_call(const std::wstring& name, const std::string& request, unsigned timeout_ms = 5000);

// Frame helpers (shared with tests).
bool pipe_write_frame(void* handle, const std::string& body);
bool pipe_read_frame(void* handle, std::string& body, size_t max_len = 16u * 1024 * 1024);

}  // namespace wb
