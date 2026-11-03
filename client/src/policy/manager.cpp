#include "policy/manager.h"

#include "base/strings.h"
#include "platform/crypto.h"
#include "platform/log.h"

namespace wb::policy {

namespace {
constexpr const char* kLog = "policy";
constexpr const char* kBundleSecret = "policy.bundle";
constexpr const char* kKeysSetting = "policy.keys";
constexpr const char* kBundleTyp = "workbench-policy+jwt";

bool same_user(const std::string& a, const std::string& b) { return !a.empty() && to_lower(a) == to_lower(b); }
}  // namespace

PolicyManager::PolicyManager(AppStore& store, const Clock& clock, Deps deps) : store_(store), clock_(clock), deps_(std::move(deps)) {
    if (auto k = store_.setting(kKeysSetting)) keys_ = parse_json(*k).value_or(json::object());
}

Status PolicyManager::verify_signature(const Jwt& jwt, bool allow_key_fetch) {
    json keys;
    {
        const std::lock_guard lock(mx_);
        keys = keys_;
    }
    if (keys.contains("keys") && verify_jwt_signature(jwt, keys)) return {};
    if (!allow_key_fetch || !deps_.fetch_keys) return make_error("signature", "the bundle signature does not verify");
    // An unknown key id: fetch the published keys once (key rotation).
    auto fetched = deps_.fetch_keys();
    if (!fetched) return make_error("keys", "policy keys unavailable: " + fetched.error().message);
    if (!fetched->contains("keys") || !(*fetched)["keys"].is_array()) return make_error("keys", "malformed policy key set");
    {
        const std::lock_guard lock(mx_);
        keys_ = *fetched;
    }
    (void)store_.set_setting(kKeysSetting, fetched->dump());
    if (Status s = verify_jwt_signature(jwt, *fetched); !s) return make_error("signature", "the bundle signature does not verify");
    return {};
}

Status PolicyManager::accept(const std::string& jws) {
    const auto reject = [this](const std::string& kind, const std::string& msg, bool security) -> Status {
        if (security) (void)store_.security_event(kind, msg);
        log::warn(kLog, "policy bundle rejected: " + msg);
        const std::lock_guard lock(mx_);
        last_error_ = msg;
        return make_error("policy_rejected", msg);
    };
    const auto jwt = parse_jwt(jws);
    if (!jwt) return reject("policy.bundle.malformed", "the bundle is not a compact JWS", true);
    if (jstr(jwt->header, "typ") != kBundleTyp || jwt->alg() != "ES256")
        return reject("policy.bundle.malformed", "unexpected bundle type or algorithm", true);
    if (Status s = verify_signature(*jwt, true); !s) {
        if (s.error().code == "keys") return reject("", s.error().message, false);
        return reject("policy.bundle.tampered", "signature verification failed", true);
    }
    auto b = bundle_from_payload(jwt->payload);
    if (!b) return reject("policy.bundle.malformed", b.error().message, true);
    const std::string user = deps_.user_principal ? deps_.user_principal() : std::string{};
    if (!same_user(b->sub, user)) return reject("policy.bundle.subject", "the bundle is addressed to another user", true);
    if (b->exp <= clock_.unix_seconds()) return reject("", "the bundle has expired", false);
    long long floor = 0;
    if (auto v = store_.setting(max_version_key(to_lower(user)))) floor = std::atoll(v->c_str());
    {
        const std::lock_guard lock(mx_);
        if (current_ && same_user(current_->bundle().sub, user)) floor = std::max(floor, current_->version());
        if (current_ && current_->bundle().raw == jws) return {};  // the same bundle again
    }
    if (b->ver < floor) return reject("policy.bundle.rollback", "bundle version " + std::to_string(b->ver) + " is lower than " + std::to_string(floor), true);
    b->raw = jws;
    (void)store_.set_secret(kBundleSecret, jws);
    (void)store_.set_setting(max_version_key(to_lower(user)), std::to_string(b->ver));
    apply(std::make_shared<const EffectivePolicy>(std::move(*b)));
    log::info(kLog, "policy version " + std::to_string(jwt->payload.value("ver", 0LL)) + " in force");
    return {};
}

void PolicyManager::apply(std::shared_ptr<const EffectivePolicy> p) {
    {
        const std::lock_guard lock(mx_);
        current_ = p;
        last_error_.clear();
    }
    if (deps_.on_change) deps_.on_change(p);
}

void PolicyManager::load_cached() {
    const auto jws = store_.secret(kBundleSecret);
    if (!jws) return;
    const auto jwt = parse_jwt(*jws);
    if (!jwt || !verify_signature(*jwt, false)) {
        (void)store_.security_event("policy.cache.invalid", "the cached bundle does not verify; it was discarded");
        (void)store_.delete_secret(kBundleSecret);
        return;
    }
    auto b = bundle_from_payload(jwt->payload);
    const std::string user = deps_.user_principal ? deps_.user_principal() : std::string{};
    if (!b || !same_user(b->sub, user)) return;  // another user's bundle stays unused
    b->raw = *jws;
    apply(std::make_shared<const EffectivePolicy>(std::move(*b)));  // current() still checks expiry
}

Status PolicyManager::refresh() {
    if (!deps_.fetch_bundle) return make_error("unavailable", "no policy service");
    auto r = deps_.fetch_bundle();
    if (!r) {
        const std::lock_guard lock(mx_);
        last_error_ = r.error().message;
        return r.error();
    }
    const std::string jws = jstr(*r, "bundle");
    if (jws.empty()) return make_error("policy_rejected", "the response holds no bundle");
    if (Status s = accept(jws); !s) return s;
    const std::lock_guard lock(mx_);
    last_refresh_ = clock_.unix_seconds();
    return {};
}

std::shared_ptr<const EffectivePolicy> PolicyManager::current() const {
    const std::lock_guard lock(mx_);
    if (!current_ || current_->bundle().exp <= clock_.unix_seconds()) return nullptr;
    return current_;
}

std::string PolicyManager::version() const {
    const std::lock_guard lock(mx_);
    return current_ ? std::to_string(current_->version()) : std::string{};
}

bool PolicyManager::refresh_due() const {
    const std::lock_guard lock(mx_);
    return last_refresh_ == 0 || clock_.unix_seconds() - last_refresh_ >= kRefreshIntervalS;
}

json PolicyManager::status() const {
    const std::lock_guard lock(mx_);
    const long long now = clock_.unix_seconds();
    json j = {{"state", !current_ ? "none" : current_->bundle().exp <= now ? "expired" : "valid"}, {"last_error", last_error_},
              {"last_refresh", last_refresh_}};
    if (current_) {
        j["version"] = current_->version();
        j["expires_at"] = current_->bundle().exp;
        j["groups"] = current_->bundle().groups;
    }
    return j;
}

}  // namespace wb::policy
