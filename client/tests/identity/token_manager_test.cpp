// client/identity Session Kept While Offline (add-offline-operation 1.3): refreshes that fail because the identity
// provider cannot be reached keep the session for a day; the first refresh after the outage renews it, and a refused
// one ends it.
#include "identity/token_manager.h"
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

// A fake identity provider: while `up` is false it closes every connection without an answer (the network is down).
struct FakeIdp {
    std::atomic<bool> up{false};
    std::atomic<bool> refuse{false};
    std::atomic<int> refreshes{0};
    test::LoopbackHttpServer server{[this](const test::LoopbackHttpServer::Request& r) -> std::string {
        if (!up) return {};
        using S = test::LoopbackHttpServer;
        const std::string json_type = "Content-Type: application/json\r\n";
        if (r.path == "/realms/t/.well-known/openid-configuration") {
            const std::string base = server.url("/realms/t");
            return S::response(200, "OK",
                               json{{"issuer", base}, {"authorization_endpoint", base + "/auth"}, {"token_endpoint", base + "/token"},
                                    {"jwks_uri", base + "/certs"}}.dump(),
                               json_type);
        }
        if (r.path == "/realms/t/token") {
            if (refuse) return S::response(400, "Bad Request", R"({"error":"invalid_grant"})", json_type);
            const int n = ++refreshes;
            return S::response(200, "OK",
                               json{{"access_token", "at-" + std::to_string(n)}, {"expires_in", 300},
                                    {"refresh_token", "rt-" + std::to_string(n)}}.dump(),
                               json_type);
        }
        return S::response(404, "Not Found", "");
    }};
    [[nodiscard]] std::string issuer() const { return server.url("/realms/t"); }
};

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
