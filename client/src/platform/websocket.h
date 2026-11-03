// WinHTTP WebSocket client (add-client-foundation design D8). Blocking receive on a dedicated thread; send and
// close may be called from other threads.
#pragma once

#include "base/result.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wb {

struct WsMessage {
    enum class Kind { text, closed } kind = Kind::text;
    std::string text;
    unsigned short close_status = 0;
    std::string close_reason;
};

class WebSocket {
public:
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Opens a WebSocket; `url` is ws:// or wss:// (ws only to loopback in dev builds). Error code "http_<status>"
    // when the handshake is refused (for example "http_401").
    static Result<std::unique_ptr<WebSocket>> connect(const std::string& url,
                                                      const std::vector<std::pair<std::string, std::string>>& headers,
                                                      std::chrono::milliseconds timeout);

    Status send_text(const std::string& text);
    // Blocks until a complete message or close arrives. Errors when the connection fails or is aborted.
    Result<WsMessage> receive(size_t max_message = 4u * 1024 * 1024);
    // Graceful close; safe from any thread.
    void close(unsigned short status = 1000, const std::string& reason = {});
    // Abrupt close that unblocks a pending receive; safe from any thread.
    void abort();

private:
    WebSocket() = default;
    std::mutex mx_;
    void* ws_ = nullptr;
    bool closed_ = false;
};

}  // namespace wb
