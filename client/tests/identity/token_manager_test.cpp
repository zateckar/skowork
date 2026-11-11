// client/identity Session Kept While Offline (add-offline-operation 1.3): refreshes that fail because the identity
// provider cannot be reached keep the session for a day; the first refresh after the outage renews it, and a refused
// one ends it.
#include "identity/token_manager.h"
#include "base/encoding.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/keystore.h"
#include "support/dev_endpoints.h"
#include "support/http_server.h"

#include <gtest/gtest.h>

#include <atomic>

using namespace wb;

namespace {

class FakeClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override { return t; }
    void advance(std::chrono::seconds s) { t += s; }
    sys_clock::time_point t = sys_clock::time_point(std::chrono::seconds(1'800'000'000));
};

struct TempDir {
    std::filesystem::path path = std::filesystem::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct TestStore {
    TempDir dir;
    std::unique_ptr<AppStore> store;
    TestStore() {
        auto root = DataRoot::open(dir.path / L"root");
        EXPECT_TRUE(root);
        KeyStore keys(root->keys());
        EXPECT_TRUE(keys.load());
        auto key = keys.get_or_create("app.db");
        MigrationOutcome o;
        auto db = open_and_migrate(root->file("app.db"), *key, app_migrations(), o);
        EXPECT_TRUE(db) << db.error().message;
        store = std::make_unique<AppStore>(std::move(*db));
    }
};

class NoBrowser final : public BrowserLauncher {
public:
    Status open(const std::string&) override {
        ++opened;
        return {};
    }
    int opened = 0;
};

// A refresh token shaped as Keycloak's: the `typ` claim says whether it is an offline token. The signature is not
// checked by the client, so the other segments are placeholders.
std::string refresh_token_of(const std::string& typ, int n) {
    return base64url_encode(std::string_view(R"({"alg":"HS256","typ":"JWT"})")) + "." +
           base64url_encode(json{{"typ", typ}, {"n", n}}.dump()) + "." + base64url_encode(std::string_view("sig"));
}

// Opens the browser in the test: records the authorization URL instead.
class CaptureBrowser final : public BrowserLauncher {
public:
    Status open(const std::string& url) override {
        ++opened;
        last_url = url;
        return {};
    }
    int opened = 0;
    std::string last_url;
};

// A fake identity provider: while `up` is false it closes every connection without an answer (the network is down).
// `offline` makes refreshes return offline tokens; `refuse` makes them invalid_grant; `revoke_fails` makes revocation fail.
struct FakeIdp {
    std::atomic<bool> up{false};
    std::atomic<bool> refuse{false};
    std::atomic<bool> offline{false};
    std::atomic<bool> revoke_fails{false};
    std::atomic<int> refreshes{0};
    std::mutex rev_mx;
    std::vector<std::string> revocations;  // bodies of the revocation requests
    test::LoopbackHttpServer server{[this](const test::LoopbackHttpServer::Request& r) -> std::string {
        if (!up) return {};
        using S = test::LoopbackHttpServer;
        const std::string json_type = "Content-Type: application/json\r\n";
        if (r.path == "/realms/t/.well-known/openid-configuration") {
            const std::string base = server.url("/realms/t");
            return S::response(200, "OK",
                               json{{"issuer", base}, {"authorization_endpoint", base + "/auth"}, {"token_endpoint", base + "/token"},
                                    {"revocation_endpoint", base + "/revoke"}, {"jwks_uri", base + "/certs"}}.dump(),
                               json_type);
        }
        if (r.path == "/realms/t/revoke") {
            {
                const std::lock_guard lk(rev_mx);
                revocations.push_back(r.body);
            }
            return revoke_fails ? S::response(500, "Internal Server Error", "") : S::response(200, "OK", "", json_type);
        }
        if (r.path == "/realms/t/token") {
            if (refuse) return S::response(400, "Bad Request", R"({"error":"invalid_grant"})", json_type);
            const int n = ++refreshes;
            const std::string refresh = offline ? refresh_token_of("Offline", n) : "rt-" + std::to_string(n);
            return S::response(200, "OK",
                               json{{"access_token", "at-" + std::to_string(n)}, {"expires_in", 300}, {"refresh_token", refresh}}.dump(),
                               json_type);
        }
        return S::response(404, "Not Found", "");
    }};
    [[nodiscard]] std::string issuer() const { return server.url("/realms/t"); }
    [[nodiscard]] std::vector<std::string> revocation_bodies() {
        const std::lock_guard lk(rev_mx);
        return revocations;
    }
};

IdentityConfig config_for(const FakeIdp& idp) {
    IdentityConfig cfg;
    cfg.issuer = idp.issuer();
    cfg.client_id = "workbench-desktop";
    cfg.redirect_uri = "https://app.test/callback";  // not a loopback redirect: no listener in these tests
    return cfg;
}

}  // namespace

TEST(TokenManagerOffline, SessionSurvivesADayWithoutTheIdentityProvider) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    NoBrowser browser;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", "rt-0"));
    ts.store->set_setting("identity.bound_subject", "user-1");
    IdentityConfig cfg;
    cfg.issuer = idp.issuer();
    cfg.client_id = "workbench-desktop";
    TokenManager tm(cfg, *ts.store, clock, browser);
    tm.start();
    EXPECT_EQ(tm.state(), AuthState::signed_in);

    // 24 hours of failed refreshes, one attempt every 10 minutes.
    for (int i = 0; i < 24 * 6; ++i) {
        clock.advance(std::chrono::minutes(10));
        auto t = tm.access_token();
        ASSERT_FALSE(t);
        EXPECT_NE(t.error().code, "sign_in_required");
        ASSERT_EQ(tm.state(), AuthState::signed_in) << "after " << (i + 1) * 10 << " minutes";
    }
    EXPECT_EQ(ts.store->secret("identity.refresh_token").value_or(""), "rt-0");
    EXPECT_EQ(browser.opened, 0);

    // The network returns: the first refresh renews the session and rotates the refresh token.
    idp.up = true;
    auto t = tm.access_token();
    ASSERT_TRUE(t) << t.error().message;
    EXPECT_EQ(*t, "at-1");
    EXPECT_EQ(ts.store->secret("identity.refresh_token").value_or(""), "rt-1");
    EXPECT_EQ(tm.state(), AuthState::signed_in);
}

TEST(TokenManagerOffline, RefusedRefreshAfterTheOutageEndsTheSession) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    NoBrowser browser;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", "rt-0"));
    ts.store->set_setting("identity.bound_subject", "user-1");
    IdentityConfig cfg;
    cfg.issuer = idp.issuer();
    cfg.client_id = "workbench-desktop";
    TokenManager tm(cfg, *ts.store, clock, browser);
    tm.start();
    clock.advance(std::chrono::hours(24));
    ASSERT_FALSE(tm.access_token());
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    idp.up = true;
    idp.refuse = true;
    auto t = tm.access_token();
    ASSERT_FALSE(t);
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(ts.store->secret("identity.refresh_token").has_value());
}

// client/identity Offline Sign-In Request, Offline Token Type, Offline Session Lifetime and Offline Sign-In Policy
// (add-offline-sign-in tasks 2.1 to 2.5), against the fake identity provider above and a fake clock.

TEST(TokenManagerOfflineSignIn, AuthorizeRequestAsksForOfflineAccessOnlyWhilePolicyAllows) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.set_offline_sign_in(true);
    ASSERT_TRUE(tm.begin_sign_in());
    EXPECT_NE(browser.last_url.find("offline_access"), std::string::npos) << browser.last_url;
    // A policy change applies to the next sign-in.
    tm.set_offline_sign_in(false);
    ASSERT_TRUE(tm.begin_sign_in());
    EXPECT_EQ(browser.last_url.find("offline_access"), std::string::npos) << browser.last_url;
    EXPECT_EQ(browser.opened, 2);
    EXPECT_TRUE(idp.revocation_bodies().empty());
}

TEST(TokenManagerOfflineSignIn, StatusIsActiveForAnOfflineTokenAndNotAvailableForANormalOne) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    idp.offline = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Offline", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.start();
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    EXPECT_EQ(tm.offline_sign_in_status(), OfflineStatus::active);

    // The realm now issues a normal refresh token: the user stays signed in and the status says so.
    idp.offline = false;
    clock.advance(std::chrono::minutes(10));
    ASSERT_TRUE(tm.access_token());
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    EXPECT_EQ(tm.offline_sign_in_status(), OfflineStatus::not_available);
    EXPECT_EQ(browser.opened, 0);
}

TEST(TokenManagerOfflineSignIn, PolicyOffRevokesTheOfflineTokenAndEndsTheSessionWithoutTheBrowser) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    idp.offline = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Offline", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.start();
    const std::string held = ts.store->secret("identity.refresh_token").value_or("");
    ASSERT_FALSE(held.empty());

    tm.set_offline_sign_in(false);
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(ts.store->secret("identity.refresh_token").has_value());
    EXPECT_EQ(browser.opened, 0);
    const auto revocations = idp.revocation_bodies();
    ASSERT_EQ(revocations.size(), 1u);
    EXPECT_NE(revocations[0].find(held), std::string::npos) << revocations[0];
    EXPECT_EQ(tm.offline_sign_in_status(), OfflineStatus::off);

    // The next sign-in does not ask for offline_access.
    ASSERT_TRUE(tm.begin_sign_in());
    EXPECT_EQ(browser.last_url.find("offline_access"), std::string::npos) << browser.last_url;
}

TEST(TokenManagerOfflineSignIn, PolicyOffAtStartEndsAStoredOfflineTokenBeforeAnyRefresh) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    idp.offline = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Offline", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.set_offline_sign_in(false);  // the cached bundle is applied before start()
    tm.start();
    EXPECT_EQ(idp.refreshes.load(), 0);
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(ts.store->secret("identity.refresh_token").has_value());
    EXPECT_EQ(idp.revocation_bodies().size(), 1u);
    EXPECT_EQ(browser.opened, 0);
}

TEST(TokenManagerOfflineSignIn, PolicyOffKeepsANormalRefreshToken) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Refresh", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.set_offline_sign_in(false);
    tm.start();
    EXPECT_EQ(idp.refreshes.load(), 1);
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    EXPECT_TRUE(idp.revocation_bodies().empty());
    EXPECT_TRUE(ts.store->secret("identity.refresh_token").has_value());
}

TEST(TokenManagerOfflineSignIn, FailedRevocationStillDeletesTheTokenAndRecordsAnEvent) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    idp.offline = true;
    idp.revoke_fails = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Offline", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.start();
    std::vector<std::string> kinds;
    tm.on_security_event([&](const std::string& kind, const std::string&) { kinds.push_back(kind); });
    tm.set_offline_sign_in(false);
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(ts.store->secret("identity.refresh_token").has_value());
    EXPECT_EQ(kinds, std::vector<std::string>{"offline_revoke_failed"});
    EXPECT_EQ(browser.opened, 0);
}

TEST(TokenManagerOfflineSignIn, OfflineSessionRefreshesAfterTwentyDaysAndEndsOnInvalidGrant) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    idp.offline = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Offline", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.start();
    const std::string before = ts.store->secret("identity.refresh_token").value_or("");

    clock.advance(std::chrono::hours(24 * 20));
    auto t = tm.access_token();
    ASSERT_TRUE(t) << t.error().message;
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    EXPECT_NE(ts.store->secret("identity.refresh_token").value_or(""), before);  // rotated, still offline
    EXPECT_EQ(tm.offline_sign_in_status(), OfflineStatus::active);
    EXPECT_EQ(browser.opened, 0);

    // The realm ends the session: the token is deleted and the browser is not opened on its own.
    idp.refuse = true;
    clock.advance(std::chrono::hours(1));
    ASSERT_FALSE(tm.access_token());
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(ts.store->secret("identity.refresh_token").has_value());
    EXPECT_EQ(browser.opened, 0);
}

TEST(TokenManagerOfflineSignIn, SignOutRevokesAndDeletesAnOfflineToken) {
    WB_REQUIRES_DEV_ENDPOINTS();
    TestStore ts;
    FakeIdp idp;
    FakeClock clock;
    CaptureBrowser browser;
    idp.up = true;
    idp.offline = true;
    ASSERT_TRUE(ts.store->set_secret("identity.refresh_token", refresh_token_of("Offline", 0)));
    ts.store->set_setting("identity.bound_subject", "user-1");
    TokenManager tm(config_for(idp), *ts.store, clock, browser);
    tm.start();
    const std::string held = ts.store->secret("identity.refresh_token").value_or("");
    ASSERT_TRUE(tm.sign_out());
    const auto revocations = idp.revocation_bodies();
    ASSERT_EQ(revocations.size(), 1u);
    EXPECT_NE(revocations[0].find(held), std::string::npos) << revocations[0];
    EXPECT_FALSE(ts.store->secret("identity.refresh_token").has_value());
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_EQ(browser.opened, 0);
}
