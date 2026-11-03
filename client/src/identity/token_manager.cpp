#include "identity/token_manager.h"

#include "base/strings.h"
#include "base/url.h"
#include "platform/crypto.h"
#include "platform/http.h"
#include "platform/log.h"
#include "platform/win.h"

#include <shellapi.h>

namespace wb {
namespace {

constexpr char kRefreshSecret[] = "identity.refresh_token";
constexpr char kBoundSubject[] = "identity.bound_subject";
constexpr char kBoundUpn[] = "identity.bound_upn";
constexpr long long kMinValidity = 60;       // seconds of validity a caller must get (client/identity)
constexpr long long kAttemptLifetime = 600;  // a sign-in attempt older than this is discarded
constexpr char kLog[] = "identity";

Error token_error(const HttpResponse& resp) {
    const auto body = parse_json(resp.body);
    const std::string err = body ? jstr(*body, "error") : std::string{};
    if (err == "invalid_grant") return make_error("invalid_grant", "the identity provider rejected the grant");
    if (resp.status >= 500) return make_error("idp_unavailable", "identity provider HTTP " + std::to_string(resp.status));
    return make_error(err.empty() ? "token_error" : err, "token endpoint HTTP " + std::to_string(resp.status));
}

}  // namespace

const char* to_string(AuthState s) {
    switch (s) {
    case AuthState::starting: return "starting";
    case AuthState::signing_in: return "signing_in";
    case AuthState::signed_in: return "signed_in";
    case AuthState::sign_in_required: return "sign_in_required";
    }
    return "?";
}

Status SystemBrowserLauncher::open(const std::string& url) {
    const auto r = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) return make_error("browser", "cannot open the default browser");
    return {};
}

PkceAttempt make_pkce_attempt(long long now) {
    PkceAttempt a;
    a.verifier = random_urlsafe(64);
    a.challenge = base64url_encode(sha256(a.verifier));
    a.state = random_urlsafe(32);
    a.nonce = random_urlsafe(32);
    a.created_at = now;
    return a;
}

std::string build_authorize_url(const std::string& authorization_endpoint, const IdentityConfig& cfg,
                                const PkceAttempt& attempt, const std::string& login_hint) {
    std::vector<std::pair<std::string, std::string>> q = {
        {"response_type", "code"},
        {"client_id", cfg.client_id},
        {"redirect_uri", cfg.redirect_uri},
        {"scope", cfg.scope},
        {"state", attempt.state},
        {"nonce", attempt.nonce},
        {"code_challenge", attempt.challenge},
        {"code_challenge_method", "S256"},
    };
    if (!login_hint.empty()) q.emplace_back("login_hint", login_hint);
    return authorization_endpoint + (authorization_endpoint.find('?') == std::string::npos ? "?" : "&") + form_encode(q);
}

TokenManager::TokenManager(IdentityConfig cfg, AppStore& store, const Clock& clock, BrowserLauncher& launcher)
    : cfg_(std::move(cfg)), store_(store), clock_(clock), launcher_(launcher), oidc_(cfg_.issuer) {}

TokenManager::~TokenManager() { close_listener(); }

void TokenManager::close_listener() {
    std::unique_ptr<LoopbackListener> l;
    {
        const std::lock_guard lock(mx_);
        l = std::move(listener_);
    }
    // Destroyed outside the lock: joins the listener thread (or detaches when called from it).
    l.reset();
}

bool TokenManager::redirect_listener_active() const {
    const std::lock_guard lock(mx_);
    return listener_ && listener_->listening();
}

void TokenManager::on_state_changed(std::function<void(AuthState)> fn) {
    const std::lock_guard lock(mx_);
    state_observers_.push_back(std::move(fn));
}

void TokenManager::on_new_access_token(std::function<void(const std::string&, long long)> fn) {
    const std::lock_guard lock(mx_);
    token_observers_.push_back(std::move(fn));
}

void TokenManager::on_security_event(std::function<void(const std::string&, const std::string&)> fn) {
    const std::lock_guard lock(mx_);
    security_observers_.push_back(std::move(fn));
}

AuthState TokenManager::state() const {
    const std::lock_guard lock(mx_);
    return state_;
}

std::string TokenManager::subject() const { return store_.setting(kBoundSubject).value_or(""); }
std::string TokenManager::user_principal() const { return store_.setting(kBoundUpn).value_or(""); }

std::string TokenManager::last_error() const {
    const std::lock_guard lock(mx_);
    return last_error_;
}

long long TokenManager::access_token_expiry() const {
    const std::lock_guard lock(mx_);
    return access_expiry_;
}

void TokenManager::set_state(AuthState s) {
    std::vector<std::function<void(AuthState)>> observers;
    {
        const std::lock_guard lock(mx_);
        if (state_ == s) return;
        state_ = s;
        observers = state_observers_;
    }
    log::info(kLog, std::string("state ") + to_string(s));
    for (const auto& fn : observers) fn(s);
}

void TokenManager::security_event(const std::string& kind, const std::string& detail) {
    store_.security_event(kind, detail);
    log::warn(kLog, "security event " + kind + ": " + detail);
    std::vector<std::function<void(const std::string&, const std::string&)>> observers;
    {
        const std::lock_guard lock(mx_);
        observers = security_observers_;
    }
    for (const auto& fn : observers) fn(kind, detail);
}

void TokenManager::enter_sign_in_required(const std::string& reason, bool delete_refresh) {
    if (delete_refresh) store_.delete_secret(kRefreshSecret);
    {
        const std::lock_guard lock(mx_);
        access_token_.clear();
        access_expiry_ = 0;
        last_error_ = reason;
    }
    set_state(AuthState::sign_in_required);
}

void TokenManager::start() {
    if (store_.secret(kRefreshSecret)) {
        set_state(AuthState::signed_in);
        const std::lock_guard lock(refresh_mx_);
        if (Status s = refresh_locked(); !s) {
            log::warn(kLog, "initial refresh failed: " + s.error().message);
            // invalid_grant has already moved to sign_in_required; network errors keep the session for later.
        }
        return;
    }
    if (subject().empty()) {
        // First start: open the browser once on our own (client/identity Sign-In Required State).
        if (Status s = begin_sign_in(); !s) enter_sign_in_required(s.error().message, false);
        return;
    }
    enter_sign_in_required("no stored session", false);
}

Status TokenManager::begin_sign_in() {
    auto ep = oidc_.endpoints();
    if (!ep) {
        const std::lock_guard lock(mx_);
        last_error_ = ep.error().message;
        return ep.error();
    }
    PkceAttempt attempt = make_pkce_attempt(clock_.unix_seconds());
    const std::string url = build_authorize_url(ep->authorization, cfg_, attempt, user_principal());
    if (LoopbackListener::is_loopback_redirect(cfg_.redirect_uri)) {
        // One listener per pending sign-in: a second click replaces it (and its port) with a fresh one.
        close_listener();
        LoopbackListener::Callbacks cb;
        cb.redirect = [this](const std::string& uri) {
            const Status s = handle_redirect(uri);
            if (s) return true;
            log::warn(kLog, "sign-in redirect refused: " + s.error().message);
            // A response for no pending attempt (forged or stale) leaves the listener waiting for the real one.
            return s.error().code != "state_mismatch" && s.error().code != "redirect";
        };
        cb.timed_out = [this] {
            log::warn(kLog, "sign-in not completed in time; the redirect listener is closed");
            {
                const std::lock_guard lock(mx_);
                pending_.clear();
            }
            if (state() == AuthState::signing_in) enter_sign_in_required("the sign-in was not completed in time", false);
        };
        auto l = LoopbackListener::start(cfg_.redirect_uri, std::move(cb), cfg_.sign_in_timeout);
        if (!l) {
            const std::string why = l.error().code == "port_in_use"
                                        ? "Sign-in needs port " + std::to_string(parse_url(cfg_.redirect_uri)->port) +
                                              ", which another program is using. Close that program and sign in again."
                                        : "cannot receive the sign-in: " + l.error().message;
            log::warn(kLog, why);
            enter_sign_in_required(why, false);
            return l.error();
        }
        const std::lock_guard lock(mx_);
        listener_ = std::move(*l);
    }
    {
        const std::lock_guard lock(mx_);
        // Keep at most a few attempts (the user may click Sign in twice); drop stale ones.
        const long long now = clock_.unix_seconds();
        std::erase_if(pending_, [&](const PkceAttempt& p) { return now - p.created_at > kAttemptLifetime; });
        if (pending_.size() >= 3) pending_.erase(pending_.begin());
        pending_.push_back(attempt);
    }
    set_state(AuthState::signing_in);
    log::info(kLog, "opening the identity provider sign-in page");
    if (Status s = launcher_.open(url); !s) {
        close_listener();
        return s;
    }
    return {};
}

Status TokenManager::handle_redirect(const std::string& uri) {
    const auto u = parse_url(uri);
    const auto expected = parse_url(cfg_.redirect_uri);
    if (!u || !expected || u->scheme != expected->scheme || u->port != expected->port || u->path != expected->path)
        return make_error("redirect", "not a sign-in redirect");
    const auto q = parse_query(u->query);
    auto get = [&](const char* k) {
        const auto it = q.find(k);
        return it == q.end() ? std::string{} : it->second;
    };
    const std::string state = get("state");
    std::optional<PkceAttempt> attempt;
    {
        const std::lock_guard lock(mx_);
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (!state.empty() && constant_time_equals(it->state, state)) {
                attempt = *it;
                pending_.erase(it);
                break;
            }
        }
    }
    if (!attempt) {
        security_event("oauth_state_mismatch", "redirect with unknown state discarded");
        return make_error("state_mismatch", "the redirect does not match a pending sign-in");
    }
    // The response for a pending attempt has arrived: stop listening (the listener closes on its own when it
    // delivered this redirect; a test launcher may hand it over directly).
    {
        const std::lock_guard lock(mx_);
        if (listener_ && pending_.empty()) listener_->cancel();
    }
    if (const std::string err = get("error"); !err.empty()) {
        enter_sign_in_required("sign-in failed at the identity provider: " + err, false);
        return make_error("authorize_error", err);
    }
    if (const std::string iss = get("iss"); !iss.empty() && iss != cfg_.issuer) {
        security_event("oauth_issuer_mismatch", "redirect from unexpected issuer discarded");
        enter_sign_in_required("redirect from an unexpected issuer", false);
        return make_error("issuer_mismatch", "redirect from unexpected issuer");
    }
    const std::string code = get("code");
    if (code.empty()) return make_error("redirect", "no authorization code");
    const std::lock_guard lock(refresh_mx_);
    return redeem_code(code, *attempt);
}

Status TokenManager::redeem_code(const std::string& code, const PkceAttempt& attempt) {
    auto ep = oidc_.endpoints();
    if (!ep) return ep.error();
    HttpRequest req;
    req.method = "POST";
    req.url = ep->token;
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    req.body = form_encode({{"grant_type", "authorization_code"},
                            {"code", code},
                            {"redirect_uri", cfg_.redirect_uri},
                            {"client_id", cfg_.client_id},
                            {"code_verifier", attempt.verifier}});
    ++token_requests_;
    auto resp = http().send(req);
    if (!resp) {
        enter_sign_in_required("cannot reach the identity provider: " + resp.error().message, false);
        return resp.error();
    }
    if (resp->status != 200) {
        Error e = token_error(*resp);
        enter_sign_in_required("code exchange failed: " + e.message, false);
        return e;
    }
    auto body = parse_json(resp->body);
    if (!body) return make_error("token_error", "token response is not JSON");
    Jwt id;
    if (Status s = oidc_.verify_id_token(jstr(*body, "id_token"), cfg_.client_id, attempt.nonce, clock_, id); !s) {
        security_event("id_token_invalid", s.error().message);
        enter_sign_in_required("ID token verification failed", false);
        return s;
    }
    const std::string bound = subject();
    if (!bound.empty() && bound != id.sub()) {
        security_event("subject_mismatch", "sign-in for a different person was refused");
        enter_sign_in_required("This Workbench data belongs to " + user_principal() +
                                   ". Sign in with that account.",
                               false);
        return make_error("subject_mismatch", "data root is bound to another account");
    }
    if (bound.empty()) {
        store_.set_setting(kBoundSubject, id.sub());
        std::string upn = jstr(id.payload, "preferred_username");
        if (upn.empty()) upn = jstr(id.payload, "email");
        store_.set_setting(kBoundUpn, upn);
    }
    return store_tokens(*body, true, id.sub());
}

Status TokenManager::store_tokens(const json& body, bool /*from_code*/, const std::string& /*subject_hint*/) {
    const std::string access = jstr(body, "access_token");
    const long long expires_in = jint(body, "expires_in", 0);
    if (access.empty() || expires_in <= 0) return make_error("token_error", "token response without access token");
    // Rotation: store the new refresh token before the new access token is used (client/identity).
    if (const std::string refresh = jstr(body, "refresh_token"); !refresh.empty()) {
        if (Status s = store_.set_secret(kRefreshSecret, refresh); !s) return s;
    }
    const long long expiry = clock_.unix_seconds() + expires_in;
    std::vector<std::function<void(const std::string&, long long)>> observers;
    {
        const std::lock_guard lock(mx_);
        access_token_ = access;
        access_expiry_ = expiry;
        last_error_.clear();
        observers = token_observers_;
    }
    set_state(AuthState::signed_in);
    for (const auto& fn : observers) fn(access, expiry);
    return {};
}

Status TokenManager::refresh_locked() {
    const auto refresh = store_.secret(kRefreshSecret);
    if (!refresh) {
        enter_sign_in_required("no stored session", false);
        return make_error("sign_in_required", "no refresh token");
    }
    auto ep = oidc_.endpoints();
    if (!ep) {
        const std::lock_guard lock(mx_);
        last_error_ = ep.error().message;
        return ep.error();
    }
    HttpRequest req;
    req.method = "POST";
    req.url = ep->token;
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    req.body = form_encode({{"grant_type", "refresh_token"}, {"refresh_token", *refresh}, {"client_id", cfg_.client_id}});
    ++token_requests_;
    auto resp = http().send(req);
    if (!resp) {
        const std::lock_guard lock(mx_);
        last_error_ = "cannot reach the identity provider: " + resp.error().message;
        return resp.error();
    }
    if (resp->status != 200) {
        Error e = token_error(*resp);
        if (e.code == "invalid_grant") {
            // End of the session: delete the stored token, never present it again.
            enter_sign_in_required("your session has ended; sign in again", true);
        } else {
            const std::lock_guard lock(mx_);
            last_error_ = e.message;
        }
        return e;
    }
    auto body = parse_json(resp->body);
    if (!body) return make_error("token_error", "token response is not JSON");
    return store_tokens(*body, false, {});
}

Result<std::string> TokenManager::access_token() {
    {
        const std::lock_guard lock(mx_);
        if (state_ == AuthState::sign_in_required || state_ == AuthState::signing_in)
            return make_error("sign_in_required", "sign-in required");
        if (!access_token_.empty() && access_expiry_ - clock_.unix_seconds() >= kMinValidity) return access_token_;
    }
    const std::lock_guard refresh_lock(refresh_mx_);
    {
        // Another caller may have refreshed while this one waited for the lock.
        const std::lock_guard lock(mx_);
        if (!access_token_.empty() && access_expiry_ - clock_.unix_seconds() >= kMinValidity) return access_token_;
    }
    if (Status s = refresh_locked(); !s) return s.error();
    const std::lock_guard lock(mx_);
    return access_token_;
}

Result<std::string> TokenManager::refresh_after_unauthorized(const std::string& rejected) {
    const std::lock_guard refresh_lock(refresh_mx_);
    {
        const std::lock_guard lock(mx_);
        if (state_ == AuthState::sign_in_required) return make_error("sign_in_required", "sign-in required");
        if (!access_token_.empty() && access_token_ != rejected) return access_token_;
    }
    if (Status s = refresh_locked(); !s) return s.error();
    const std::lock_guard lock(mx_);
    return access_token_;
}

void TokenManager::session_rejected(const std::string& reason) {
    log::warn(kLog, "control plane rejected the refreshed token: " + reason);
    enter_sign_in_required("the control plane rejected the session", false);
}

void TokenManager::drop_access_token() {
    const std::lock_guard lock(mx_);
    access_token_.clear();
    access_expiry_ = 0;
}

Status TokenManager::sign_out() {
    close_listener();
    const std::lock_guard refresh_lock(refresh_mx_);
    Status result;
    if (const auto refresh = store_.secret(kRefreshSecret)) {
        if (auto ep = oidc_.endpoints(); ep && !ep->revocation.empty()) {
            HttpRequest req;
            req.method = "POST";
            req.url = ep->revocation;
            req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
            req.body = form_encode({{"token", *refresh}, {"token_type_hint", "refresh_token"}, {"client_id", cfg_.client_id}});
            auto resp = http().send(req);
            if (!resp) result = resp.error();
            else if (resp->status != 200) result = make_error("revoke", "revocation HTTP " + std::to_string(resp->status));
        }
    }
    // Local sign-out happens even when revocation failed: the token is deleted and never used again.
    enter_sign_in_required("signed out", true);
    {
        const std::lock_guard lock(mx_);
        pending_.clear();
    }
    return result;
}

}  // namespace wb
