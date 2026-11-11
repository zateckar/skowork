// A loopback HTTP/1.1 test server: one request per connection, answered by a handler that sees the request head and
// returns the raw response. Records every request head it receives.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wb::test {

class LoopbackHttpServer {
public:
    struct Request {
        std::string method, path, head;  // head: request line and headers
        std::string body;                // the Content-Length bytes after the head (empty when none)
        [[nodiscard]] bool has_header(const std::string& lower_name_prefix) const {
            std::string low;
            for (const char c : head) low += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            return low.find("\r\n" + lower_name_prefix) != std::string::npos;
        }
    };
    using Handler = std::function<std::string(const Request&)>;

    explicit LoopbackHttpServer(Handler handler) : handler_(std::move(handler)) {
        WSADATA w;
        ::WSAStartup(MAKEWORD(2, 2), &w);
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        ::getsockname(sock_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        ::listen(sock_, 8);
        thread_ = std::thread([this] {
            for (;;) {
                SOCKET c = ::accept(sock_, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                // A connection per thread: a browser's idle preconnect must not hold up the next request.
                const DWORD timeout_ms = 15000;
                ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
                const std::lock_guard lk(mx_);
                connections_.emplace_back([this, c] { serve(c); });
            }
        });
    }
    ~LoopbackHttpServer() {
        ::closesocket(sock_);
        thread_.join();
        std::vector<std::thread> conns;
        {
            const std::lock_guard lk(mx_);
            conns.swap(connections_);
        }
        for (auto& t : conns) t.join();
        ::WSACleanup();
    }
    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    [[nodiscard]] std::string url_as(const std::string& host, const std::string& path) const {
        return "http://" + host + ":" + std::to_string(port_) + path;
    }
    [[nodiscard]] std::vector<Request> requests() const {
        std::lock_guard lk(mx_);
        return requests_;
    }

    // A complete response with Content-Length and Connection: close.
    static std::string response(int status, const std::string& reason, const std::string& body,
                                const std::string& extra_headers = "Content-Type: text/plain\r\n") {
        return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" + extra_headers + "Content-Length: " +
               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    }

private:
    void serve(SOCKET c) {
        std::string raw;
        char buf[4096];
        while (raw.find("\r\n\r\n") == std::string::npos) {
            const int n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            raw.append(buf, n);
        }
        if (raw.empty()) {  // a preconnect that never sent a request
            ::closesocket(c);
            return;
        }
        Request r;
        const size_t head_end = raw.find("\r\n\r\n") + 4;
        r.head = raw.substr(0, head_end - 2);
        // The body: read Content-Length bytes after the head.
        std::string low;
        for (const char c : r.head) low += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (const size_t at = low.find("\r\ncontent-length:"); at != std::string::npos) {
            const size_t want = std::stoul(low.substr(at + 17));
            while (raw.size() < head_end + want) {
                const int n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                raw.append(buf, n);
            }
            r.body = raw.substr(head_end, want);
        }
        const auto sp1 = raw.find(' '), sp2 = raw.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            r.method = raw.substr(0, sp1);
            r.path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
        }
        {
            std::lock_guard lk(mx_);
            requests_.push_back(r);
        }
        const std::string response = handler_(r);
        for (size_t sent = 0; sent < response.size();) {
            const int n = ::send(c, response.data() + sent, static_cast<int>(std::min<size_t>(response.size() - sent, 1 << 20)), 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        ::shutdown(c, SD_SEND);
        ::closesocket(c);
    }

    Handler handler_;
    SOCKET sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
    mutable std::mutex mx_;
    std::vector<Request> requests_;
    std::vector<std::thread> connections_;
};

}  // namespace wb::test
