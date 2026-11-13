// Support for end-to-end tests against the organization Keycloak test realm and the local control plane (design D12).
// Tests skip when either is not reachable. Real-service tests run locally only; never in CI.
#pragma once

#include "base/json.h"
#include "base/time.h"
#include "identity/token_manager.h"
#include "storage/app_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wb::e2e {

class TcpProxy;

// The organization Keycloak test realm from the repository .env (OIDC_*; environment variables win) and the local
// control plane started by dev/scripts/dev-stack.ps1 (SKOWORK_E2E_CP, default http://localhost:8080).
struct Env {
    std::string issuer;
    std::string client_id;
    std::string redirect_uri;
    std::string scope;
    std::string cp;
    json discovery;  // the identity provider's discovery document
};

// Returns the stack or nullopt when the identity provider or the control plane is not reachable.
const std::optional<Env>& env();
std::string& skip_reason();  // why env() is empty

#define SKOWORK_REQUIRE_STACK()                                                                                 \
    do {                                                                                                        \
        if (!::wb::e2e::env()) GTEST_SKIP() << ::wb::e2e::skip_reason();                                    \
    } while (0)

// True when SKOWORK_E2E_SLOW is set: tests that wait for a real 5-minute token to expire.
bool slow_tests_enabled();

IdentityConfig desktop_config();

// A fresh encrypted app.db in a temporary data root under %TEMP%.
class TempStore {
public:
    TempStore();
    ~TempStore();
    AppStore& store() { return *store_; }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    std::unique_ptr<AppStore> store_;
};

// Records the authorize URL instead of opening a browser.
class RecordingLauncher final : public BrowserLauncher {
public:
    Status open(const std::string& url) override {
        last_url = url;
        ++opens;
        return {};
    }
    std::string last_url;
    int opens = 0;
};

// Signs `tokens` in through the system browser and the loopback redirect, as a user would. The first sign-in of a
// run may need the user at the browser; later ones complete on the identity provider's single sign-on session.
// `tokens` must have been built with a SystemBrowserLauncher.
Status sign_in(TokenManager& tokens, std::chrono::seconds timeout = std::chrono::seconds(180));

// One signed-in session shared by the tests that do not end their session (one browser sign-in per run).
// TokenManager observers cannot be removed, so short-lived users subscribe here instead.
struct SharedSession {
    TempStore ts;
    SystemBrowserLauncher launcher;
    TokenManager tm{desktop_config(), ts.store(), system_clock(), launcher};

    int subscribe(std::function<void()> on_auth_change);
    void unsubscribe(int id);
    void notify();

private:
    std::mutex mx_;
    std::map<int, std::function<void()>> subs_;
    int next_ = 0;
};
Result<SharedSession*> shared_session();

// The shared user's access token; the user must be a bootstrap admin of the control plane
// (dev-stack.ps1 -BootstrapAdmins, default: the Windows user name).
Result<std::string> admin_token();
std::string user_name_of(const std::string& access_token);  // preferred_username claim

// Calls the control plane with a bearer token: returns {status, json body}.
std::pair<int, json> cp_call(const std::string& method, const std::string& path, const std::string& token,
                             const json& body = nullptr);

// Posts a form to the identity provider's token or revocation endpoint.
std::pair<int, json> idp_token(const std::vector<std::pair<std::string, std::string>>& form);
std::pair<int, json> idp_revoke(const std::string& refresh_token);
// When SKOWORK_E2E_CP_UPSTREAM names the real control-plane port, every test reaches the control plane through
// a proxy on the SKOWORK_E2E_CP port, and blip tests can cut it. Returns nullptr otherwise.
TcpProxy* cp_proxy();

// A settable clock that starts at real time.
class FakeClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override { return sys_clock::time_point(sys_clock::duration(t.load())); }
    void advance(std::chrono::seconds s) { t += std::chrono::duration_cast<sys_clock::duration>(s).count(); }
    std::atomic<sys_clock::duration::rep> t{sys_clock::now().time_since_epoch().count()};
};

// A loopback TCP forwarder that can drop all connections and refuse new ones (simulated network blip).
class TcpProxy {
public:
    TcpProxy(int listen_port, int target_port);  // listen_port 0 picks a free port
    ~TcpProxy();
    [[nodiscard]] int port() const { return port_; }
    void set_down(bool down);  // down: closes every connection and refuses new ones

private:
    void accept_loop();
    int target_port_;
    int port_ = 0;
    std::uintptr_t listener_;
    std::atomic<bool> down_{false};
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::mutex mx_;
    std::vector<std::uintptr_t> sockets_;
    std::vector<std::thread> pumps_;
};

}  // namespace wb::e2e
