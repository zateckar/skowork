// Graph access tokens (client/mail-calendar Entra OAuth Delegated Access, Token Acquisition Through The Broker Or The
// System Browser, OAuth Token Storage, Mock Authentication Restricted To Development Builds; design D1): silent through
// the Windows account broker when one is plugged in, otherwise the authorization code flow with PKCE in the system
// browser and a loopback redirect on a port chosen per sign-in. Access tokens stay in memory; a refresh token is stored
// only encrypted (AppStore secret).
#pragma once

#include "base/result.h"
#include "base/time.h"
#include "identity/loopback_listener.h"
#include "identity/token_manager.h"
#include "mail/mail_config.h"
#include "storage/app_store.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wb::mail {

// The Windows account broker (WAM). Spike 1.1 decides whether it is usable on organization devices; until then no broker
// is plugged in and every token comes from the browser sign-in and its refresh token.
class TokenBroker {
public:
    virtual ~TokenBroker() = default;
    struct Token {
        std::string access_token;
        long long expires_at = 0;  // unix seconds
    };
    // Error "interaction_required" when the broker needs the user; "unavailable" when it cannot serve the tenant.
    virtual Result<Token> silent(const MailConfig& cfg) = 0;
};

enum class GraphAuthState {
    disconnected,          // no usable token; the user has to connect
    connecting,            // the browser sign-in is pending
    connected,             // a refresh token or the broker supplies tokens
    interaction_required,  // the identity provider asked for the user (consent, conditional access)
};
const char* to_string(GraphAuthState s);

class GraphAuth {
public:
    GraphAuth(MailConfig cfg, AppStore& store, const Clock& clock, BrowserLauncher& launcher, TokenBroker* broker = nullptr);
    ~GraphAuth();
    GraphAuth(const GraphAuth&) = delete;
    GraphAuth& operator=(const GraphAuth&) = delete;

    // Restores the connection without prompting: the broker, then the stored refresh token. Never opens the browser.
    void start();
    // The user chose Connect: opens the authorize URL in the browser with a fresh PKCE attempt and listens on a loopback
    // port until the redirect arrives or the sign-in times out.
    Status connect();
    // The redirect URI (from the loopback listener, or handed over by a test).
    Status handle_redirect(const std::string& uri);
    // An access token valid for at least 60 s. Errors: "not_connected", "interaction_required", "offline" (the token
    // endpoint cannot be reached), "token_error".
    Result<std::string> access_token();
    // After a 401: refreshes once unless the token was already replaced.
    Result<std::string> refresh_after_unauthorized(const std::string& rejected);
    // Disconnect mailbox: deletes the stored refresh token and every in-memory token; stops a pending sign-in.
    void disconnect();
    // The local-storage forget operations: the same as disconnect.
    static Status forget_secrets(AppStore& store);

    [[nodiscard]] GraphAuthState state() const;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] bool listener_active() const;
    [[nodiscard]] int listener_port() const;
    [[nodiscard]] const MailConfig& config() const { return cfg_; }
    // Login hint for the identity provider (the Workbench user's principal name).
    void set_login_hint(std::string hint) { login_hint_ = std::move(hint); }
    void set_sign_in_timeout(std::chrono::milliseconds t) { timeout_ = t; }
    void on_state_changed(std::function<void(GraphAuthState)> fn);

    [[nodiscard]] std::string redirect_uri() const;  // of the pending sign-in

private:
    Status refresh_locked();
    Status store_tokens(const json& body);
    void set_state(GraphAuthState s, const std::string& error = {});
    void close_listener();

    MailConfig cfg_;
    AppStore& store_;
    const Clock& clock_;
    BrowserLauncher& launcher_;
    TokenBroker* broker_;
    std::string login_hint_;
    std::chrono::milliseconds timeout_ = std::chrono::minutes(5);

    mutable std::mutex mx_;
    std::mutex refresh_mx_;
    GraphAuthState state_ = GraphAuthState::disconnected;
    std::string last_error_;
    std::string access_token_;
    long long access_expiry_ = 0;
    bool from_broker_ = false;
    PkceAttempt pending_;
    std::string pending_redirect_;
    std::unique_ptr<LoopbackListener> listener_;
    std::vector<std::function<void(GraphAuthState)>> observers_;
};

}  // namespace wb::mail
