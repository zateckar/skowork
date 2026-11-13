// client/model-access Streaming Responses, scenario "Small chunks are not held back" (fix-long-thread-model-calls D8):
// HttpClient hands each chunk of a streamed body to on_chunk when it arrives, not when its read buffer fills.
#include "platform/http.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace wb;
using namespace std::chrono_literals;

namespace {

// A loopback server that sends response headers, then `count` small chunks `gap` apart, then closes.
class TrickleServer {
public:
    TrickleServer(int count, std::chrono::milliseconds gap) : count_(count), gap_(gap) {
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
        ::listen(sock_, 1);
        thread_ = std::thread([this] {
            SOCKET c = ::accept(sock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const int n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, n);
            }
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
            ::send(c, head.data(), static_cast<int>(head.size()), 0);
            const std::string chunk = ": keep-alive\n\n";
            for (int i = 0; i < count_; ++i) {
                std::this_thread::sleep_for(gap_);
                {
                    std::lock_guard lock(mu);
                    sent_at.push_back(std::chrono::steady_clock::now());
                }
                ::send(c, chunk.data(), static_cast<int>(chunk.size()), 0);
            }
            ::shutdown(c, SD_SEND);
            ::closesocket(c);
        });
    }
    ~TrickleServer() {
        ::closesocket(sock_);
        thread_.join();
        ::WSACleanup();
    }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }
    std::mutex mu;
    std::vector<std::chrono::steady_clock::time_point> sent_at;

private:
    int count_;
    std::chrono::milliseconds gap_;
    SOCKET sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
};

}  // namespace

TEST(HttpStream, SmallChunksReachTheCallerAsTheyArrive) {
    if (!HttpClient::url_allowed("http://127.0.0.1:1/")) GTEST_SKIP() << "http loopback needs SKOWORK_DEV_ENDPOINTS";
    TrickleServer srv(4, 400ms);
    HttpClient client;
    HttpRequest req;
    req.method = "GET";
    req.url = srv.url();
    // Shorter than the whole response, longer than any gap: holding chunks back until the end would not fail here,
    // but the delivery times below would.
    req.idle_timeout = 3000ms;
    std::vector<std::chrono::steady_clock::time_point> got_at;
    std::string body;
    req.on_chunk = [&](std::string_view c) {
        got_at.push_back(std::chrono::steady_clock::now());
        body.append(c);
        return true;
    };
    auto r = client.send(req);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(body.size(), 4u * 14u);
    std::lock_guard lock(srv.mu);
    ASSERT_EQ(srv.sent_at.size(), 4u);
    ASSERT_GE(got_at.size(), 4u);
    for (size_t i = 0; i < srv.sent_at.size(); ++i)
        EXPECT_LT(got_at[i] - srv.sent_at[i], 300ms) << "chunk " << i << " was held back";
}
