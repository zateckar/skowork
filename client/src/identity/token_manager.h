// Sign-in with OAuth 2.0 Authorization Code + PKCE in the system browser, and the token manager that keeps a
// fresh access token for every control-plane call (client/identity; add-client-foundation design D7).
#pragma once

#include "base/result.h"
#include "base/time.h"
#include "identity/loopback_listener.h"
#include "identity/oidc.h"
#include "storage/app_store.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb {

struct IdentityConfig {
    std::string issuer;
    std::string client_id;
    // A loopback redirect (http://localhost:<port>/<path>) is received by a listener that runs only while a
    // sign-in is pending (client/identity Loopback Redirect During Sign-In Only).
    std::string redirect_uri = "http://localhost:4200/auth/callback";
    std::string scope = "openid profile email";
    std::chrono::milliseconds sign_in_timeout = std::chrono::minutes(5);
};

// Opens the authorization URL. The default implementation uses the user's default browser; tests substitute a
// launcher that completes the sign-in without a browser.
class BrowserLauncher {
public:
    virtual ~BrowserLauncher() = default;
    virtual Status open(const std::string& url) = 0;
};

class SystemBrowserLauncher final : public BrowserLauncher {
public:
    Status open(const std::string& url) override;
};

enum class AuthState {
    starting,          // before start()
    signing_in,        // browser opened, waiting for the redirect
    signed_in,         // a refresh token is held
    sign_in_required,  // no usable refresh token (client/identity Sign-In Required State)
};

const char* to_string(AuthState s);

struct PkceAttempt {
    std::string state;
    std::string nonce;
    std::string verifier;
    std::string challenge;
    long long created_at = 0;
};

// Builds a fresh attempt: 64-byte verifier (86 base64url chars), S256 challenge, 32-byte state, 32-byte nonce.
PkceAttempt make_pkce_attempt(long long now);
std::string build_authorize_url(const std::string& authorization_endpoint, const IdentityConfig& cfg,
                                const PkceAttempt& attempt, const std::string& login_hint = {});

class TokenManager {
public:
    TokenManager(IdentityConfig cfg, AppStore& store, const Clock& clock, BrowserLauncher& launcher);
    ~TokenManager();
    TokenManager(const TokenManager&) = delete;
    TokenManager& operator=(const TokenManager&) = delete;

    // Loads the stored refresh token and obtains an access token without the browser; with none stored, opens the
    // browser only on first start (no bound subject). Network failures leave the state signed_in (retried later).
    void start();

    // User chose "Sign in" (or first start): opens the browser with a fresh PKCE attempt.
    Status begin_sign_in();

    // The redirect URI received by the loopback listener (or by a test launcher).
    Status handle_redirect(const std::string& uri);

    // Returns an access token valid for at least 60 s, refreshing first when needed. Concurrent callers share one
    // refresh. Error code "sign_in_required" when no session exists.
    Result<std::string> access_token();

    // After a 401 for `rejected`: refreshes once unless another caller already replaced that token.
    Result<std::string> refresh_after_unauthorized(const std::string& rejected);

    // Marks the session as unusable after a second 401 (client/identity: enters the sign-in required state).
    void session_rejected(const std::string& reason);

    // Revokes and deletes the refresh token, drops the access token, enters sign_in_required. Local data stays.
    Status sign_out();

    // Discards the access token so that the next call refreshes (revocation message, client/control-channel).
    void drop_access_token();

    [[nodiscard]] AuthState state() const;
    [[nodiscard]] std::string subject() const;
    [[nodiscard]] std::string user_principal() const;  // preferred_username / email of the bound user
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] long long access_token_expiry() const;  // unix seconds, 0 when none
    [[nodiscard]] bool redirect_listener_active() const;  // a loopback listener is open

    // Observers. Called on the thread that caused the change; must not call back into the manager under lock.
    void on_state_changed(std::function<void(AuthState)> fn);
    void on_new_access_token(std::function<void(const std::string& token, long long expires_at)> fn);
    void on_security_event(std::function<void(const std::string& kind, const std::string& detail)> fn);

    // Number of token-endpoint requests made (refresh and code exchange); for tests.
    [[nodiscard]] int token_requests() const { return token_requests_; }

private:
    Status refresh_locked();  // requires refresh_mx_
    Status redeem_code(const std::string& code, const PkceAttempt& attempt);
    Status store_tokens(const json& token_response, bool from_code, const std::string& subject_hint);
    void set_state(AuthState s);
    void security_event(const std::string& kind, const std::string& detail);
    void enter_sign_in_required(const std::string& reason, bool delete_refresh);
    void close_listener();

    IdentityConfig cfg_;
    AppStore& store_;
    const Clock& clock_;
    BrowserLauncher& launcher_;
    OidcProvider oidc_;

    mutable std::mutex mx_;        // guards the fields below
    std::mutex refresh_mx_;        // single-flight token requests
    AuthState state_ = AuthState::starting;
    std::string access_token_;
    long long access_expiry_ = 0;  // unix seconds
    std::vector<PkceAttempt> pending_;
    std::unique_ptr<LoopbackListener> listener_;
    std::string last_error_;
    int token_requests_ = 0;

    std::vector<std::function<void(AuthState)>> state_observers_;
    std::vector<std::function<void(const std::string&, long long)>> token_observers_;
    std::vector<std::function<void(const std::string&, const std::string&)>> security_observers_;
};

}  // namespace wb
