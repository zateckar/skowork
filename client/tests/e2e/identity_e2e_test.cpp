// client/identity end to end against the organization Keycloak test realm and the local control plane
// (add-client-foundation task 5.3). Sign-ins go through the system browser and the loopback redirect.
#include "e2e_support.h"

#include "base/jwt.h"
#include "base/url.h"
#include "channel/control_plane.h"
#include "identity/token_manager.h"

#include <winsock2.h>
#include <ws2tcpip.h>

using namespace wb;
using namespace wb::e2e;
using namespace std::chrono_literals;

namespace {

constexpr char kRefreshSecret[] = "identity.refresh_token";

struct IdentityE2E : ::testing::Test {
    void SetUp() override { WORKBENCH_REQUIRE_STACK(); }
};

// A store with its own signed-in session (a separate browser sign-in, completed by single sign-on).
struct OwnSession {
    TempStore ts;
    SystemBrowserLauncher launcher;
    TokenManager tm{desktop_config(), ts.store(), system_clock(), launcher};
};

bool port_accepts(int port) {
    WSADATA d;
    ::WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::closesocket(s);
    return ok;
}

int redirect_port() { return parse_url(env()->redirect_uri)->port; }

}  // namespace

TEST_F(IdentityE2E, SignInWithPkceThroughTheLoopbackRedirect) {
    // The authorize request carries PKCE, state and nonce (recorded, not sent to a browser).
    {
        TempStore ts;
        RecordingLauncher rec;
        TokenManager tm(desktop_config(), ts.store(), system_clock(), rec);
        tm.start();  // first start without a session opens the sign-in page once
        EXPECT_EQ(rec.opens, 1);
        EXPECT_EQ(tm.state(), AuthState::signing_in);
        EXPECT_NE(rec.last_url.find("code_challenge_method=S256"), std::string::npos);
        EXPECT_NE(rec.last_url.find("state="), std::string::npos);
        EXPECT_NE(rec.last_url.find("nonce="), std::string::npos);
        EXPECT_NE(rec.last_url.find("redirect_uri=" + url_encode(env()->redirect_uri)), std::string::npos);
        EXPECT_TRUE(tm.redirect_listener_active()) << "listening while the sign-in is pending";
    }
    EXPECT_FALSE(port_accepts(redirect_port())) << "the listener closes with its sign-in";

    auto s = shared_session();
    ASSERT_TRUE(s) << s.error().message;
    TokenManager& tm = (*s)->tm;
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    EXPECT_FALSE(tm.subject().empty());
    EXPECT_FALSE(tm.user_principal().empty());
    EXPECT_TRUE((*s)->ts.store().secret(kRefreshSecret).has_value());
    // client/identity Loopback Redirect During Sign-In Only: no listener once signed in.
    EXPECT_FALSE(tm.redirect_listener_active());
    EXPECT_FALSE(port_accepts(redirect_port()));

    auto at = tm.access_token();
    ASSERT_TRUE(at) << at.error().message;
    auto jwt = parse_jwt(*at);
    ASSERT_TRUE(jwt);
    EXPECT_EQ(jstr(jwt->payload, "azp"), env()->client_id);
    auto [status, who] = cp_call("GET", "/api/v1/identity/whoami", *at);
    EXPECT_EQ(status, 200) << who.dump();
    EXPECT_EQ(jstr(who, "user_name"), tm.user_principal());
}

TEST_F(IdentityE2E, RestartUsesTheStoredSessionWithoutABrowser) {
    auto s = shared_session();
    ASSERT_TRUE(s) << s.error().message;
    RecordingLauncher rec;
    TokenManager again(desktop_config(), (*s)->ts.store(), system_clock(), rec);
    again.start();
    EXPECT_EQ(again.state(), AuthState::signed_in);
    EXPECT_EQ(rec.opens, 0);
    EXPECT_TRUE(again.access_token());
    // The restart rotated the stored refresh token; the shared manager reads it from the same store.
    (*s)->tm.drop_access_token();
    EXPECT_TRUE((*s)->tm.access_token());
}

TEST_F(IdentityE2E, FiveMinuteTokenIsKeptFreshForTwentyMinutes) {
    auto s = shared_session();
    ASSERT_TRUE(s) << s.error().message;
    FakeClock clock;
    RecordingLauncher rec;
    TokenManager tm(desktop_config(), (*s)->ts.store(), clock, rec);
    tm.start();
    ASSERT_EQ(tm.state(), AuthState::signed_in);
    const long long lifetime = tm.access_token_expiry() - clock.unix_seconds();
    ASSERT_GE(lifetime, 120) << "the realm issues " << lifetime << " s access tokens";
    const int requests_after_start = tm.token_requests();
    std::string last;
    int distinct = 0;
    for (int step = 0; step < 40; ++step) {  // 40 x 30 s = 20 minutes
        clock.advance(30s);
        auto at = tm.access_token();
        ASSERT_TRUE(at) << at.error().message;
        EXPECT_GE(tm.access_token_expiry() - clock.unix_seconds(), 60) << "a caller must get at least 60 s";
        if (*at != last) {
            ++distinct;
            last = *at;
            auto [status, body] = cp_call("GET", "/api/v1/identity/whoami", *at);
            EXPECT_EQ(status, 200) << body.dump();
        }
    }
    // A 300 s token is refreshed once less than 60 s remain: every 240-270 s, so 4 or 5 times in 20 minutes.
    const int refreshes = tm.token_requests() - requests_after_start;
    EXPECT_GE(refreshes, 4);
    EXPECT_LE(refreshes, 5);
    EXPECT_EQ(distinct, refreshes + 1);  // the token from start() plus one per refresh
    EXPECT_EQ(tm.state(), AuthState::signed_in);
    (*s)->tm.drop_access_token();  // the shared manager picks up the rotated refresh token
}

TEST_F(IdentityE2E, ConcurrentCallersShareOneRefresh) {
    auto s = shared_session();
    ASSERT_TRUE(s) << s.error().message;
    FakeClock clock;
    RecordingLauncher rec;
    TokenManager tm(desktop_config(), (*s)->ts.store(), clock, rec);
    tm.start();
    ASSERT_EQ(tm.state(), AuthState::signed_in);
    clock.advance(std::chrono::seconds(tm.access_token_expiry() - clock.unix_seconds() - 30));  // inside the window
    const int before = tm.token_requests();
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            if (tm.access_token()) ++ok;
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(ok.load(), 8);
    EXPECT_EQ(tm.token_requests() - before, 1);
    (*s)->tm.drop_access_token();
}

TEST_F(IdentityE2E, RedirectPortInUseKeepsTheBrowserClosed) {
    // Another program holds the redirect port.
    WSADATA d;
    ::WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET blocker = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(redirect_port()));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(blocker, reinterpret_cast<sockaddr*>(&a), sizeof a), 0) << "port already in use before the test";
    ASSERT_EQ(::listen(blocker, 1), 0);

    TempStore ts;
    RecordingLauncher rec;
    TokenManager tm(desktop_config(), ts.store(), system_clock(), rec);
    auto r = tm.begin_sign_in();
    ::closesocket(blocker);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "port_in_use");
    EXPECT_EQ(rec.opens, 0) << "the browser is not opened";
    EXPECT_EQ(tm.state(), AuthState::sign_in_required);
    EXPECT_NE(tm.last_error().find(std::to_string(redirect_port())), std::string::npos) << tm.last_error();
}

TEST_F(IdentityE2E, ForgedOrForeignRedirectsAreRefusedAndAudited) {
    TempStore ts;
    RecordingLauncher rec;
    TokenManager tm(desktop_config(), ts.store(), system_clock(), rec);
    ASSERT_TRUE(tm.begin_sign_in());
    EXPECT_FALSE(tm.handle_redirect(env()->redirect_uri + "?code=abc&state=forged"));
    bool audited = false;
    for (const auto& e : ts.store().security_events()) audited |= e.kind.find("state") != std::string::npos;
    EXPECT_TRUE(audited);
    // Another path or a private-use scheme is not a sign-in redirect at all.
    EXPECT_FALSE(tm.handle_redirect("http://localhost:" + std::to_string(redirect_port()) + "/other?code=a&state=b"));
    EXPECT_FALSE(tm.handle_redirect("com.example.workbench:/oauth2redirect?code=abc&state=x"));
    // A forged request on the listener gets a page but no token request is made.
    HttpRequest req;
    req.url = env()->redirect_uri + "?code=abc&state=forged";
    (void)http().send(req);
    EXPECT_EQ(tm.token_requests(), 0);
}

TEST_F(IdentityE2E, ReusedRefreshTokenIsRefusedByTheIdentityProvider) {
    OwnSession o;
    ASSERT_TRUE(sign_in(o.tm));
    const std::string first_refresh = *o.ts.store().secret(kRefreshSecret);
    o.tm.drop_access_token();
    ASSERT_TRUE(o.tm.access_token());  // rotates
    const std::string second_refresh = *o.ts.store().secret(kRefreshSecret);
    EXPECT_NE(first_refresh, second_refresh) << "refresh tokens rotate";
    auto [status, body] = idp_token({{"grant_type", "refresh_token"}, {"refresh_token", first_refresh},
                                     {"client_id", env()->client_id}});
    if (status == 200) {
        (void)idp_revoke(jstr(body, "refresh_token"));
        (void)o.tm.sign_out();
        GTEST_SKIP() << "this realm accepts a rotated refresh token once more (Keycloak 'Revoke Refresh Token' is off)";
    }
    EXPECT_EQ(status, 400) << body.dump();
    EXPECT_EQ(jstr(body, "error"), "invalid_grant");
    (void)o.tm.sign_out();
}

TEST_F(IdentityE2E, RevokedSessionRequiresSignInAndDeletesTheRefreshToken) {
    OwnSession o;
    ASSERT_TRUE(sign_in(o.tm));
    // The session ends at the identity provider (here: its refresh token is revoked from outside the client).
    auto [status, body] = idp_revoke(*o.ts.store().secret(kRefreshSecret));
    ASSERT_EQ(status, 200) << body.dump();
    o.tm.drop_access_token();
    EXPECT_FALSE(o.tm.access_token());
    EXPECT_EQ(o.tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(o.ts.store().secret(kRefreshSecret).has_value()) << "a rejected refresh token is never presented again";
}

TEST_F(IdentityE2E, SignOutRevokesAndForgetsTheSession) {
    OwnSession o;
    ASSERT_TRUE(sign_in(o.tm));
    const std::string refresh = *o.ts.store().secret(kRefreshSecret);
    ASSERT_TRUE(o.tm.sign_out());
    EXPECT_EQ(o.tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(o.ts.store().secret(kRefreshSecret).has_value());
    auto [status, body] = idp_token({{"grant_type", "refresh_token"}, {"refresh_token", refresh}, {"client_id", env()->client_id}});
    EXPECT_EQ(status, 400) << body.dump();
}

TEST_F(IdentityE2E, DataBoundToAnotherUserRefusesTheSignIn) {
    OwnSession o;
    // This data root belongs to someone else (bound at an earlier sign-in).
    ASSERT_TRUE(o.ts.store().set_setting("identity.bound_subject", "00000000-0000-0000-0000-000000000000"));
    ASSERT_TRUE(o.ts.store().set_setting("identity.bound_upn", "someone.else"));
    auto s = sign_in(o.tm);
    ASSERT_FALSE(s);
    EXPECT_NE(o.tm.last_error().find("someone.else"), std::string::npos) << "the user is told which account owns the data";
    EXPECT_EQ(o.tm.state(), AuthState::sign_in_required);
    EXPECT_FALSE(o.ts.store().secret(kRefreshSecret).has_value());
    EXPECT_EQ(o.tm.user_principal(), "someone.else");
    bool audited = false;
    for (const auto& e : o.ts.store().security_events()) audited |= e.kind == "subject_mismatch";
    EXPECT_TRUE(audited);
}

TEST_F(IdentityE2E, UnauthorizedResponseTriggersOneRefreshAndRetry) {
    if (!slow_tests_enabled()) GTEST_SKIP() << "waits for a real access token to expire; set WORKBENCH_E2E_SLOW=1";
    auto s = shared_session();
    ASSERT_TRUE(s) << s.error().message;
    // The client's clock stands still, so it believes its token is fresh; the control plane sees it expire.
    FakeClock clock;
    RecordingLauncher rec;
    TokenManager tm(desktop_config(), (*s)->ts.store(), clock, rec);
    tm.start();
    ASSERT_EQ(tm.state(), AuthState::signed_in);
    const long long wait = tm.access_token_expiry() - system_clock().unix_seconds() + 35;  // past exp and the skew
    std::this_thread::sleep_for(std::chrono::seconds(wait));
    ControlPlaneClient cp(env()->cp, tm);
    const int before = tm.token_requests();
    auto r = cp.call_json("GET", env()->cp + "/api/v1/identity/whoami");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(tm.token_requests() - before, 1);
    (*s)->tm.drop_access_token();
}
