// Loopback redirect listener (client/identity Loopback Redirect During Sign-In Only): listens on the loopback
// interface only while one sign-in is pending, hands the redirect URI over once and then closes its sockets.
#pragma once

#include "base/result.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace wb {

class LoopbackListener {
public:
    struct Callbacks {
        // A request on the redirect path: the full URI (http://host:port/path?query). Called on the listener thread
        // before the browser gets its answer. Returns true when the redirect belonged to the pending sign-in: the
        // listener then closes. False (for example a forged state) keeps it listening for the real one.
        std::function<bool(const std::string& uri)> redirect;
        // No redirect within the timeout. Called on the listener thread, after the sockets are closed.
        std::function<void()> timed_out;
    };

    // Binds 127.0.0.1 and ::1 on the port of `redirect_uri` with exclusive use. Error code "port_in_use" when
    // another process holds the port; "redirect" when the URI is not an http loopback URI.
    static Result<std::unique_ptr<LoopbackListener>> start(const std::string& redirect_uri, Callbacks cb,
                                                            std::chrono::milliseconds timeout = std::chrono::minutes(5));

    // True when `redirect_uri` is an http URI on a loopback host (localhost, 127.0.0.1, [::1]).
    static bool is_loopback_redirect(const std::string& redirect_uri);

    ~LoopbackListener();  // cancels and joins
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    // Stops listening without calling back; does not wait. Safe from any thread, including a callback.
    void cancel() { cancelled_ = true; }
    [[nodiscard]] bool listening() const { return listening_; }
    [[nodiscard]] int port() const { return port_; }

private:
    LoopbackListener() = default;
    void run(std::chrono::steady_clock::time_point deadline);

    std::uintptr_t sockets_[2] = {~std::uintptr_t{0}, ~std::uintptr_t{0}};
    int port_ = 0;
    std::string host_;
    std::string path_;
    Callbacks cb_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> listening_{false};
    std::thread thread_;
};

}  // namespace wb
