#include "mail/graph_auth.h"

#include "base/strings.h"
#include "base/url.h"
#include "platform/crypto.h"
#include "platform/http.h"
#include "platform/log.h"

#include <random>

namespace wb::mail {
namespace {

constexpr char kRefreshSecret[] = "mail.refresh_token";
constexpr char kDisconnected[] = "mail.disconnected";  // the user disconnected: the broker is not asked until Connect
constexpr long long kMinValidity = 60;
constexpr char kLog[] = "mail";

// Entra answers a refresh that needs the user with interaction_required, consent_required or invalid_grant carrying a
// consent or MFA suberror; the stored token itself stays valid in those cases.
bool needs_interaction(const std::string& error) {
    return error == "interaction_required" || error == "consent_required" || error == "login_required";
}

int pick_port() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_int_distribution<int>(49152, 65000)(rng);
}

}  // namespace

const char* to_string(GraphAuthState s) {
    switch (s) {
    case GraphAuthState::disconnected: return "disconnected";
    case GraphAuthState::connecting: return "connecting";
    case GraphAuthState::connected: return "connected";
    case GraphAuthState::interaction_required: return "interaction_required";
    }
    return "?";
}

GraphAuth::GraphAuth(MailConfig cfg, AppStore& store, const Clock& clock, BrowserLauncher& launcher, TokenBroker* broker)
    : cfg_(std::move(cfg)), store_(store), clock_(clock), launcher_(launcher), broker_(broker) {}

GraphAuth::~GraphAuth() { close_listener(); }

void GraphAuth::close_listener() {
    std::unique_ptr<LoopbackListener> l;
    {
        const std::lock_guard lock(mx_);
        l = std::move(listener_);
    }
    l.reset();
}

void GraphAuth::on_state_changed(std::function<void(GraphAuthState)> fn) {
    const std::lock_guard lock(mx_);
    observers_.push_back(std::move(fn));
}

GraphAuthState GraphAuth::state() const {
    const std::lock_guard lock(mx_);
    return state_;
}

std::string GraphAuth::last_error() const {
    const std::lock_guard lock(mx_);
    return last_error_;
}

bool GraphAuth::listener_active() const {
    const std::lock_guard lock(mx_);
    return listener_ && listener_->listening();
}

int GraphAuth::listener_port() const {
    const std::lock_guard lock(mx_);
    return listener_ && listener_->listening() ? listener_->port() : 0;
}

std::string GraphAuth::redirect_uri() const {
    const std::lock_guard lock(mx_);
    return pending_redirect_;
}

void GraphAuth::set_state(GraphAuthState s, const std::string& error) {
    std::vector<std::function<void(GraphAuthState)>> observers;
    {
        const std::lock_guard lock(mx_);
        if (!error.empty() || s == GraphAuthState::connected) last_error_ = error;
        if (state_ == s) return;
        state_ = s;
        if (s != GraphAuthState::connected) {
            access_token_.clear();
            access_expiry_ = 0;
        }
        observers = observers_;
    }
    log::info(kLog, std::string("mailbox ") + to_string(s));
    for (const auto& fn : observers) fn(s);
}

void GraphAuth::start() {
    if (!cfg_.available) return set_state(GraphAuthState::disconnected, cfg_.unavailable_reason);
    if (broker_ && !store_.setting(kDisconnected)) {
        auto t = broker_->silent(cfg_);
        if (t) {
            {
                const std::lock_guard lock(mx_);
                access_token_ = t->access_token;
                access_expiry_ = t->expires_at;
                from_broker_ = true;
            }
            return set_state(GraphAuthState::connected);
        }
        if (t.error().code == "interaction_required") return set_state(GraphAuthState::interaction_required, t.error().message);
    }
    if (store_.secret(kRefreshSecret)) return set_state(GraphAuthState::connected);
    set_state(GraphAuthState::disconnected);
}

Status GraphAuth::connect() {
    if (!cfg_.available) return make_error(cfg_.unavailable_code, cfg_.unavailable_reason);
    close_listener();
    PkceAttempt attempt = make_pkce_attempt(clock_.unix_seconds());
    std::unique_ptr<LoopbackListener> listener;
    std::string redirect;
    // A loopback port chosen for this sign-in (Entra accepts any port on a localhost redirect of a public client).
    for (int i = 0; i < 20 && !listener; ++i) {
        redirect = "http://localhost:" + std::to_string(pick_port()) + "/";
        LoopbackListener::Callbacks cb;
        cb.redirect = [this](const std::string& uri) {
            const Status s = handle_redirect(uri);
            if (s) return true;
            log::warn(kLog, "mailbox sign-in redirect refused: " + s.error().message);
            return s.error().code != "state_mismatch" && s.error().code != "redirect";
        };
        cb.timed_out = [this] {
            log::warn(kLog, "mailbox sign-in not completed in time; the redirect listener is closed");
            {
                const std::lock_guard lock(mx_);
                pending_ = {};
            }
            if (state() == GraphAuthState::connecting) set_state(GraphAuthState::disconnected, "the sign-in was not completed in time");
        };
        auto l = LoopbackListener::start(redirect, std::move(cb), timeout_);
        if (l) listener = std::move(*l);
        else if (l.error().code != "port_in_use") return l.error();
    }
    if (!listener) return make_error("port_in_use", "no free loopback port for the sign-in");
    std::vector<std::pair<std::string, std::string>> q = {
        {"client_id", cfg_.client_id},
        {"response_type", "code"},
        {"redirect_uri", redirect},
        {"response_mode", "query"},
        {"scope", cfg_.scope_parameter()},
        {"state", attempt.state},
        {"code_challenge", attempt.challenge},
        {"code_challenge_method", "S256"},
    };
    if (!login_hint_.empty()) q.emplace_back("login_hint", login_hint_);
    const std::string url = cfg_.authorize_endpoint() + "?" + form_encode(q);
    {
        const std::lock_guard lock(mx_);
        pending_ = attempt;
        pending_redirect_ = redirect;
        listener_ = std::move(listener);
    }
    set_state(GraphAuthState::connecting);
    if (Status s = launcher_.open(url); !s) {
        close_listener();
        set_state(GraphAuthState::disconnected, s.error().message);
        return s;
    }
    return {};
}

Status GraphAuth::handle_redirect(const std::string& uri) {
    const auto u = parse_url(uri);
    std::string redirect;
    PkceAttempt attempt;
    {
        const std::lock_guard lock(mx_);
        redirect = pending_redirect_;
        attempt = pending_;
    }
    const auto expected = parse_url(redirect);
    if (!u || !expected || u->port != expected->port || u->path != expected->path) return make_error("redirect", "not a sign-in redirect");
    const auto q = parse_query(u->query);
    const auto get = [&](const char* k) {
        const auto it = q.find(k);
        return it == q.end() ? std::string{} : it->second;
    };
    if (attempt.state.empty() || !constant_time_equals(attempt.state, get("state"))) {
        store_.security_event("oauth_state_mismatch", "mailbox sign-in redirect with unknown state discarded");
        return make_error("state_mismatch", "the redirect does not match a pending sign-in");
    }
    {
        const std::lock_guard lock(mx_);
        pending_ = {};
        if (listener_) listener_->cancel();
    }
    if (const std::string err = get("error"); !err.empty()) {
        set_state(needs_interaction(err) ? GraphAuthState::interaction_required : GraphAuthState::disconnected,
                  "the sign-in failed at the identity provider: " + err);
        return make_error(err, get("error_description"));
    }
    const std::string code = get("code");
    if (code.empty()) return make_error("redirect", "no authorization code");
    HttpRequest req;
    req.method = "POST";
    req.url = cfg_.token_endpoint();
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    req.body = form_encode({{"grant_type", "authorization_code"},
                            {"code", code},
                            {"redirect_uri", redirect},
                            {"client_id", cfg_.client_id},
                            {"scope", cfg_.scope_parameter()},
                            {"code_verifier", attempt.verifier}});
    const std::lock_guard refresh_lock(refresh_mx_);
    auto resp = http().send(req);
    if (!resp) {
        set_state(GraphAuthState::disconnected, "cannot reach the identity provider: " + resp.error().message);
        return resp.error();
    }
    const auto body = parse_json(resp->body);
    if (resp->status != 200 || !body) {
        const std::string err = body ? jstr(*body, "error") : std::string("token_error");
        set_state(needs_interaction(err) ? GraphAuthState::interaction_required : GraphAuthState::disconnected,
                  "the code exchange failed: " + err);
        return make_error(err.empty() ? "token_error" : err, "token endpoint HTTP " + std::to_string(resp->status));
    }
    return store_tokens(*body);
}

Status GraphAuth::store_tokens(const json& body) {
    const std::string access = jstr(body, "access_token");
    const long long expires_in = jint(body, "expires_in", 0);
    if (access.empty() || expires_in <= 0) return make_error("token_error", "token response without an access token");
    // Rotation: the new refresh token replaces the old one before the access token is used.
    if (const std::string refresh = jstr(body, "refresh_token"); !refresh.empty())
        if (Status s = store_.set_secret(kRefreshSecret, refresh); !s) return s;
    (void)store_.delete_setting(kDisconnected);
    std::vector<std::function<void(GraphAuthState)>> observers;
    {
        const std::lock_guard lock(mx_);
        access_token_ = access;
        access_expiry_ = clock_.unix_seconds() + expires_in;
        from_broker_ = false;
        last_error_.clear();
        if (state_ != GraphAuthState::connected) observers = observers_;
        state_ = GraphAuthState::connected;
    }
    for (const auto& fn : observers) fn(GraphAuthState::connected);
    return {};
}

Status GraphAuth::refresh_locked() {
    if (broker_ && from_broker_) {
        auto t = broker_->silent(cfg_);
        if (t) {
            const std::lock_guard lock(mx_);
            access_token_ = t->access_token;
            access_expiry_ = t->expires_at;
            return {};
        }
        if (t.error().code == "interaction_required") {
            set_state(GraphAuthState::interaction_required, t.error().message);
            return make_error("interaction_required", t.error().message);
        }
    }
    const auto refresh = store_.secret(kRefreshSecret);
    if (!refresh) {
        set_state(GraphAuthState::disconnected);
        return make_error("not_connected", "the mailbox is not connected");
    }
    HttpRequest req;
    req.method = "POST";
    req.url = cfg_.token_endpoint();
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    req.body = form_encode({{"grant_type", "refresh_token"},
                            {"refresh_token", *refresh},
                            {"client_id", cfg_.client_id},
                            {"scope", cfg_.scope_parameter()}});
    auto resp = http().send(req);
    if (!resp) {
        const std::lock_guard lock(mx_);
        last_error_ = "cannot reach the identity provider: " + resp.error().message;
        return make_error("offline", last_error_);
    }
    const auto body = parse_json(resp->body);
    if (resp->status != 200 || !body) {
        const std::string err = body ? jstr(*body, "error") : std::string{};
        if (needs_interaction(err)) {
            set_state(GraphAuthState::interaction_required, "the identity provider needs you to sign in again (" + err + ")");
            return make_error("interaction_required", err);
        }
        if (err == "invalid_grant" || err == "expired_token") {
            store_.delete_secret(kRefreshSecret);
            set_state(GraphAuthState::disconnected, "the mailbox session has ended; connect again");
            return make_error("not_connected", err);
        }
        if (resp->status >= 500) return make_error("offline", "token endpoint HTTP " + std::to_string(resp->status));
        return make_error("token_error", err.empty() ? "token endpoint HTTP " + std::to_string(resp->status) : err);
    }
    return store_tokens(*body);
}

Result<std::string> GraphAuth::access_token() {
    {
        const std::lock_guard lock(mx_);
        if (state_ == GraphAuthState::interaction_required) return make_error("interaction_required", "the mailbox needs you to connect again");
        if (state_ != GraphAuthState::connected) return make_error("not_connected", "the mailbox is not connected");
        if (!access_token_.empty() && access_expiry_ - clock_.unix_seconds() >= kMinValidity) return access_token_;
    }
    const std::lock_guard refresh_lock(refresh_mx_);
    {
        const std::lock_guard lock(mx_);
        if (!access_token_.empty() && access_expiry_ - clock_.unix_seconds() >= kMinValidity) return access_token_;
    }
    if (Status s = refresh_locked(); !s) return s.error();
    const std::lock_guard lock(mx_);
    return access_token_;
}

Result<std::string> GraphAuth::refresh_after_unauthorized(const std::string& rejected) {
    const std::lock_guard refresh_lock(refresh_mx_);
    {
        const std::lock_guard lock(mx_);
        if (state_ != GraphAuthState::connected) return make_error("not_connected", "the mailbox is not connected");
        if (!access_token_.empty() && access_token_ != rejected) return access_token_;
        access_token_.clear();
        access_expiry_ = 0;
    }
    if (Status s = refresh_locked(); !s) return s.error();
    const std::lock_guard lock(mx_);
    return access_token_;
}

void GraphAuth::disconnect() {
    close_listener();
    store_.delete_secret(kRefreshSecret);
    (void)store_.set_setting(kDisconnected, "1");
    {
        const std::lock_guard lock(mx_);
        pending_ = {};
        pending_redirect_.clear();
        from_broker_ = false;
    }
    set_state(GraphAuthState::disconnected, "disconnected");
}

Status GraphAuth::forget_secrets(AppStore& store) { return store.delete_secret(kRefreshSecret); }

}  // namespace wb::mail
