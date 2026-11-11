// client/identity Loopback Redirect During Sign-In Only: the listener behind the sign-in redirect.
#include "support/dev_endpoints.h"
#include "identity/loopback_listener.h"
#include "platform/http.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

using namespace wb;
using namespace std::chrono_literals;

namespace {

int test_port() {
    static std::mt19937 rng(std::random_device{}());
    return 41000 + static_cast<int>(rng() % 2000);
}

struct Received {
    std::mutex mx;
    std::condition_variable cv;
    std::string uri;
    bool timed_out = false;

    LoopbackListener::Callbacks callbacks() {
        LoopbackListener::Callbacks cb;
        cb.redirect = [this](const std::string& u) {
            if (u.find("state=forged") != std::string::npos) return false;  // not the pending sign-in
            const std::lock_guard lock(mx);
            uri = u;
            cv.notify_all();
            return true;
        };
        cb.timed_out = [this] {
            const std::lock_guard lock(mx);
            timed_out = true;
            cv.notify_all();
        };
        return cb;
    }
    bool wait(std::chrono::milliseconds d) {
        std::unique_lock lock(mx);
        return cv.wait_for(lock, d, [this] { return !uri.empty() || timed_out; });
    }
};

Result<HttpResponse> get(const std::string& url) {
    HttpRequest r;
    r.url = url;
    r.connect_timeout = 3s;
    r.idle_timeout = 3s;
    return http().send(r);
}

}  // namespace

TEST(LoopbackListener, AcceptsOnlyLoopbackHttpRedirects) {
    EXPECT_TRUE(LoopbackListener::is_loopback_redirect("http://localhost:4200/auth/callback"));
    EXPECT_TRUE(LoopbackListener::is_loopback_redirect("http://127.0.0.1:4200/cb"));
    EXPECT_FALSE(LoopbackListener::is_loopback_redirect("https://localhost:4200/cb"));
    EXPECT_FALSE(LoopbackListener::is_loopback_redirect("http://example.com:4200/cb"));
    EXPECT_FALSE(LoopbackListener::is_loopback_redirect("com.example.workbench:/oauth2redirect"));
}

TEST(LoopbackListener, DeliversTheRedirectOnceAndStopsListening) {
    WB_REQUIRES_DEV_ENDPOINTS();
    const int port = test_port();
    const std::string base = "http://localhost:" + std::to_string(port);
    Received rx;
    auto l = LoopbackListener::start(base + "/auth/callback", rx.callbacks());
    ASSERT_TRUE(l) << l.error().message;
    EXPECT_TRUE((*l)->listening());

    // Other paths are refused and the listener keeps waiting.
    auto other = get("http://127.0.0.1:" + std::to_string(port) + "/favicon.ico");
    ASSERT_TRUE(other) << other.error().message;
    EXPECT_EQ(other->status, 404);
    EXPECT_TRUE((*l)->listening());
    // A response that is not for the pending sign-in does not end it (no denial of service by a local process).
    auto forged = get(base + "/auth/callback?code=x&state=forged");
    ASSERT_TRUE(forged) << forged.error().message;
    EXPECT_EQ(forged->status, 400);
    EXPECT_TRUE((*l)->listening());

    const auto t0 = std::chrono::steady_clock::now();
    auto page = get(base + "/auth/callback?code=abc&state=xyz");
    ASSERT_TRUE(page) << page.error().message;
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("close this tab"), std::string::npos);
    ASSERT_TRUE(rx.wait(2s));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
    EXPECT_EQ(rx.uri, base + "/auth/callback?code=abc&state=xyz");

    // The listener is closed: nothing accepts on the port any more. It closes on its own thread right after
    // answering the browser and delivering the redirect, so wait briefly for that instead of racing it.
    const auto close_by = std::chrono::steady_clock::now() + 2s;
    while ((*l)->listening() && std::chrono::steady_clock::now() < close_by) std::this_thread::sleep_for(5ms);
    EXPECT_FALSE((*l)->listening());
    EXPECT_FALSE(get("http://127.0.0.1:" + std::to_string(port) + "/auth/callback?code=again"));
}

TEST(LoopbackListener, PortInUseIsReported) {
    const int port = test_port();
    const std::string uri = "http://localhost:" + std::to_string(port) + "/cb";
    Received a, b;
    auto first = LoopbackListener::start(uri, a.callbacks());
    ASSERT_TRUE(first) << first.error().message;
    auto second = LoopbackListener::start(uri, b.callbacks());
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, "port_in_use");
}

TEST(LoopbackListener, TimesOutAndClosesThePort) {
    const int port = test_port();
    const std::string uri = "http://localhost:" + std::to_string(port) + "/cb";
    Received rx;
    auto l = LoopbackListener::start(uri, rx.callbacks(), 400ms);
    ASSERT_TRUE(l) << l.error().message;
    ASSERT_TRUE(rx.wait(3s));
    EXPECT_TRUE(rx.timed_out);
    EXPECT_FALSE((*l)->listening());
    // The port is free again for the next sign-in.
    Received again;
    EXPECT_TRUE(LoopbackListener::start(uri, again.callbacks()));
}

TEST(LoopbackListener, CancelStopsWithoutCallback) {
    const int port = test_port();
    const std::string uri = "http://localhost:" + std::to_string(port) + "/cb";
    Received rx;
    auto l = LoopbackListener::start(uri, rx.callbacks());
    ASSERT_TRUE(l);
    (*l)->cancel();
    l->reset();
    EXPECT_FALSE(rx.wait(500ms));
    EXPECT_FALSE(get("http://127.0.0.1:" + std::to_string(port) + "/cb?code=x"));
}
