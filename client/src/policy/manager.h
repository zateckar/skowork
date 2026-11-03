// Policy bundle retrieval, verification, encrypted cache and refresh (client/policy-enforcement Policy Bundle
// Verification, Policy Retrieval And Refresh, Cached Policy And Fail-Closed; design D1).
#pragma once

#include "base/json.h"
#include "base/jwt.h"
#include "base/result.h"
#include "base/time.h"
#include "policy/core.h"
#include "storage/app_store.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace wb::policy {

inline constexpr long long kRefreshIntervalS = 6 * 3600;  // at least every 6 hours

class PolicyManager {
public:
    struct Deps {
        // GET <services.policy>/bundle -> {bundle, version, expires_at, stale}
        std::function<Result<json>()> fetch_bundle;
        // GET /.well-known/policy-keys.json -> {keys: [...]}
        std::function<Result<json>()> fetch_keys;
        // The signed-in user principal; bundles for anybody else are refused.
        std::function<std::string()> user_principal;
        // Called after a new bundle is accepted (on the refreshing thread).
        std::function<void(std::shared_ptr<const EffectivePolicy>)> on_change;
    };

    PolicyManager(AppStore& store, const Clock& clock, Deps deps);

    // Loads the cached bundle of the signed-in user, verifying it again with the cached keys.
    void load_cached();
    // Fetches, verifies and applies the bundle (signature, subject, expiry, anti-rollback).
    Status refresh();
    // Verifies and applies a compact JWS; exposed for tests.
    Status accept(const std::string& jws);

    // The policy in force, or null when there is none or it has expired: then every call but reads of the client's own
    // data is denied.
    [[nodiscard]] std::shared_ptr<const EffectivePolicy> current() const;
    // The version of the accepted bundle (also an expired one), for heartbeats; empty when none.
    [[nodiscard]] std::string version() const;
    [[nodiscard]] bool refresh_due() const;
    // {state: valid|expired|none, version, expires_at, groups, last_error, last_refresh}
    [[nodiscard]] json status() const;

private:
    Status verify_signature(const Jwt& jwt, bool allow_key_fetch);
    void apply(std::shared_ptr<const EffectivePolicy> p);
    std::string max_version_key(const std::string& user) const { return "policy.max_version:" + user; }

    AppStore& store_;
    const Clock& clock_;
    Deps deps_;
    mutable std::mutex mx_;
    std::shared_ptr<const EffectivePolicy> current_;
    json keys_ = json::object();
    std::string last_error_;
    long long last_refresh_ = 0;  // unix seconds of the last successful fetch
};

}  // namespace wb::policy
