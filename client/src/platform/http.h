// WinHTTP client: system proxy/PAC, Windows certificate store, TLS-only (client/identity Server Identity
// Verification), streaming bodies, connect and idle timeouts, cancellation from another thread.
#pragma once

#include "base/result.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb {

// Cancels an in-flight request (and any later request that uses the same token) from another thread.
class CancelToken {
public:
    void cancel();
    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }

private:
    friend class HttpClient;
    friend class WebSocket;
    bool attach(void* handle);  // returns false if already cancelled
    void detach();
    std::atomic<bool> cancelled_{false};
    std::mutex mx_;
    void* handle_ = nullptr;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds idle_timeout{60000};  // maximum gap between received bytes
    // Streaming: called for every received chunk; return false to abort. When set, the response body stays empty.
    std::function<bool(std::string_view)> on_chunk;
    // With on_chunk: stream only the bodies of 2xx responses; error bodies are collected in HttpResponse::body (up to
    // max_body) so that the caller can classify them. Without it, every status streams.
    bool stream_success_only = false;
    std::shared_ptr<CancelToken> cancel;
    size_t max_body = 64u * 1024 * 1024;
    bool follow_redirects = true;
    // web.fetch only: plain http:// to any host (web content, never credentials or tokens).
    bool allow_plain_http = false;
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    [[nodiscard]] std::string header(const std::string& lower_name) const {
        const auto it = headers.find(lower_name);
        return it == headers.end() ? std::string{} : it->second;
    }
};

// Error codes: "url", "insecure_url", "tls", "connect", "timeout", "cancelled", "aborted", "io", "too_large".
class HttpClient {
public:
    explicit HttpClient(std::wstring user_agent = L"Skowork");
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    Result<HttpResponse> send(const HttpRequest& req);

    // True when the URL may be used: https/wss always; http/ws only to loopback in builds with
    // SKOWORK_DEV_ENDPOINTS (design D10).
    static bool url_allowed(std::string_view url);

    [[nodiscard]] void* session() const { return session_; }

private:
    void* session_ = nullptr;
};

// Shared session for the whole process.
HttpClient& http();

}  // namespace wb
