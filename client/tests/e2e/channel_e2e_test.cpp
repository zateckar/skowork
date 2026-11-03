// client/control-channel and control-plane/fleet-registry end to end against the local control plane and the
// organization Keycloak test realm (add-client-foundation task 5.4).
#include "e2e_support.h"

#include "channel/control_channel.h"
#include "channel/control_plane.h"
#include "platform/crypto.h"

#include <condition_variable>

using namespace wb;
using namespace wb::e2e;
using namespace std::chrono_literals;

namespace {

template <typename F>
bool wait_for(F cond, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return cond();
}

std::string new_id() {
    auto b = random_bytes(16);
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    const std::string h = hex_encode(b);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

// One client installation with its control channel. Every client uses the shared signed-in session (one browser
// sign-in per run); each has its own store, installation and channel.
struct Client {
    explicit Client(const std::string& version = "0.1.0") : session(*shared_session()), tm(session->tm), cp(env()->cp, tm) {
        RegistrationInfo info;
        info.installation_id = new_id();
        info.machine_name = "E2E-" + info.installation_id.substr(0, 8);
        info.user_principal = tm.user_principal();
        info.version = version;
        info.channel = "e2e";
        info.commit = "test";
        info.os_build = "10.0.22631";
        info.hardware_class = "cpu:8c;ram:16GB";
        ControlChannel::Hooks hooks;
        hooks.connection_changed = [this](bool up) {
            const std::lock_guard l(mx);
            transitions.push_back(up);
        };
        hooks.min_version = [this](const std::string& v, bool blocked) {
            const std::lock_guard l(mx);
            min_version = v;
            version_blocked = blocked;
        };
        hooks.revoked = [this] { revoked = true; };
        channel = std::make_unique<ControlChannel>(cp, tm, ts.store(), system_clock(), info, hooks);
        sub = session->subscribe([this] { channel->auth_changed(); });
    }
    ~Client() {
        session->unsubscribe(sub);
        if (channel) channel->stop();
    }
    std::vector<bool> connection_transitions() {
        const std::lock_guard l(mx);
        return transitions;
    }

    SharedSession* session;
    TempStore ts;
    TokenManager& tm;
    ControlPlaneClient cp;
    std::unique_ptr<ControlChannel> channel;
    int sub = 0;
    std::mutex mx;
    std::vector<bool> transitions;
    std::string min_version;
    bool version_blocked = false;
    std::atomic<bool> revoked{false};
};

struct ChannelE2E : ::testing::Test {
    void SetUp() override {
        WORKBENCH_REQUIRE_STACK();
        auto t = admin_token();
        ASSERT_TRUE(t) << t.error().message;
        admin = *t;
        auto [status, me] = cp_call("GET", "/api/v1/admin/me", admin);
        ASSERT_EQ(status, 200) << me.dump();
        const json roles = me.value("admin_roles", json::array());
        ASSERT_NE(std::find(roles.begin(), roles.end(), json("platform-admin")), roles.end())
            << "start the control plane with -BootstrapAdmins " << jstr(me, "user_name");
    }
    void TearDown() override {
        if (!env() || admin.empty()) return;
        if (auto t = admin_token()) cp_call("PUT", "/api/v1/admin/fleet/min-versions/e2e", *t, json{{"version", "0.0.0"}});
    }
    std::string admin;
};
}  // namespace

TEST_F(ChannelE2E, RegistersConnectsAndAppearsOnlineWithGroups) {
    // Groups come from role mappings and direct membership (control-plane/identity Groups And Admin Roles).
    const std::string me = user_name_of(admin);
    auto [ms, mb] = cp_call("POST", "/api/v1/admin/identity/group-members", admin,
                            json{{"user_name", me}, {"workbench_group", "e2e-developers"}});
    ASSERT_EQ(ms, 200) << mb.dump();
    Client c;
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    EXPECT_TRUE(c.channel->push_up());
    EXPECT_FALSE(c.channel->services().push.empty());
    const std::string machine = "E2E-" + c.channel->installation_id().substr(0, 8);
    auto [status, list] = cp_call("GET", "/api/v1/admin/fleet/installations?machine=" + machine + "&page_size=200", admin);
    ASSERT_EQ(status, 200) << list.dump();
    bool found = false;
    const json& items = list.contains("items") ? list["items"] : list;
    for (const auto& i : items) {
        if (jstr(i, "installation_id") != c.channel->installation_id() && jstr(i, "id") != c.channel->installation_id()) continue;
        found = true;
        EXPECT_TRUE(jbool(i, "online")) << i.dump();
        const json groups = i.value("groups", json::array());
        EXPECT_NE(std::find(groups.begin(), groups.end(), json("e2e-developers")), groups.end())
            << "the user's Groups are recorded: " << i.dump();
        EXPECT_NE(std::find(groups.begin(), groups.end(), json("everyone")), groups.end()) << i.dump();
    }
    EXPECT_TRUE(found) << list.dump();
    cp_call("DELETE", "/api/v1/admin/identity/group-members", admin, json{{"user_name", me}, {"workbench_group", "e2e-developers"}});
}

TEST_F(ChannelE2E, PolicyChangedPushIsHandledAndAckedWithinFiveSeconds) {
    Client c;
    std::mutex mx;
    std::condition_variable cv;
    long long got = 0;
    c.channel->on_push("policy-changed", [&](const PushEnvelope& e) {
        const std::lock_guard l(mx);
        got = e.id;
        cv.notify_all();
    });
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    const auto t0 = std::chrono::steady_clock::now();
    auto [status, body] = cp_call("POST", "/api/v1/admin/fleet/push", admin,
                                  json{{"type", "policy-changed"},
                                       {"target", {{"installation_id", c.channel->installation_id()}}},
                                       {"payload", {{"policy_version", "e2e-1"}}}});
    ASSERT_EQ(status, 200) << body.dump();
    {
        std::unique_lock l(mx);
        ASSERT_TRUE(cv.wait_for(l, 5s, [&] { return got != 0; }));
    }
    ASSERT_TRUE(wait_for([&] { return c.channel->last_ack() >= got; }, 5s));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 5s);
}

TEST_F(ChannelE2E, PushToAnOfflineInstallationIsDeliveredOnReconnect) {
    auto c = std::make_unique<Client>();
    std::atomic<int> received{0};
    c->channel->on_push("policy-changed", [&](const PushEnvelope&) { ++received; });
    c->channel->start();
    ASSERT_TRUE(wait_for([&] { return c->channel->connected(); }, 10s));
    c->channel->stop();
    auto [status, body] = cp_call("POST", "/api/v1/admin/fleet/push", admin,
                                  json{{"type", "policy-changed"},
                                       {"target", {{"installation_id", c->channel->installation_id()}}},
                                       {"payload", {{"policy_version", "e2e-offline"}}}});
    ASSERT_EQ(status, 200) << body.dump();
    // Same installation and store, new channel object (a client restart).
    RegistrationInfo info;
    info.installation_id = c->channel->installation_id();
    info.version = "0.1.0";
    info.channel = "e2e";
    info.machine_name = "E2E-restart";
    ControlChannel again(c->cp, c->tm, c->ts.store(), system_clock(), info, {});
    again.on_push("policy-changed", [&](const PushEnvelope&) { ++received; });
    again.start();
    EXPECT_TRUE(wait_for([&] { return received.load() == 1; }, 10s));
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(received.load(), 1) << "delivered exactly once";
    again.stop();
}

TEST_F(ChannelE2E, ThirtySecondBlipNeverEntersTheDisconnectedState) {
    if (!cp_proxy()) GTEST_SKIP() << "set WORKBENCH_E2E_CP_UPSTREAM to run blip tests through a proxy";
    Client c;
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    const int connects = c.channel->connects();
    cp_proxy()->set_down(true);
    ASSERT_TRUE(wait_for([&] { return !c.channel->push_up(); }, 10s));
    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(c.channel->connected()) << "after " << i << " s of outage";
        std::this_thread::sleep_for(1s);
    }
    cp_proxy()->set_down(false);
    EXPECT_TRUE(wait_for([&] { return c.channel->push_up(); }, 90s));  // backoff may be up to ~32 s here
    EXPECT_GT(c.channel->connects(), connects);
    for (bool up : c.connection_transitions()) EXPECT_TRUE(up) << "no disconnected transition after the first connect";
}

TEST_F(ChannelE2E, OutageLongerThanSixtySecondsIsReportedAsDisconnected) {
    if (!cp_proxy()) GTEST_SKIP() << "set WORKBENCH_E2E_CP_UPSTREAM to run blip tests through a proxy";
    Client c;
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    cp_proxy()->set_down(true);
    EXPECT_TRUE(wait_for([&] { return !c.channel->connected(); }, 75s));
    cp_proxy()->set_down(false);
    EXPECT_TRUE(wait_for([&] { return c.channel->connected(); }, 120s));
}

TEST_F(ChannelE2E, MinimumVersionPushBlocksAnOlderClient) {
    Client c("0.1.0");
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    EXPECT_FALSE(c.channel->version_blocked());
    auto [status, body] = cp_call("PUT", "/api/v1/admin/fleet/min-versions/e2e", admin, json{{"version", "9.0.0"}});
    ASSERT_EQ(status, 200) << body.dump();
    ASSERT_TRUE(wait_for([&] { return c.channel->version_blocked(); }, 5s));
    EXPECT_EQ(c.channel->min_version(), "9.0.0");
    // A client registering below the minimum learns it at registration.
    Client late("0.1.0");
    late.channel->start();
    EXPECT_TRUE(wait_for([&] { return late.channel->version_blocked(); }, 10s));
}

TEST_F(ChannelE2E, RevokedInstallationIsLocked) {
    Client c;
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    auto [status, body] = cp_call("POST", "/api/v1/admin/fleet/installations/" + c.channel->installation_id() + "/revoke",
                                  admin, json{{"reason", "e2e"}});
    ASSERT_EQ(status, 200) << body.dump();
    EXPECT_TRUE(wait_for([&] { return c.channel->locked(); }, 10s));
    EXPECT_FALSE(c.channel->connected());
}

TEST_F(ChannelE2E, AuthRenewKeepsThePushConnectionOpenPastTokenExpiry) {
    if (!slow_tests_enabled()) GTEST_SKIP() << "waits past a real access token's expiry; set WORKBENCH_E2E_SLOW=1";
    Client c;
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    const int connects = c.channel->connects();
    // The control plane closes a connection with 4401 once its token has expired, unless auth-renew extended it.
    const long long wait = c.tm.access_token_expiry() - system_clock().unix_seconds() + 45;
    std::this_thread::sleep_for(std::chrono::seconds(wait));
    EXPECT_TRUE(c.channel->connected());
    EXPECT_EQ(c.channel->connects(), connects) << "the connection was never closed";
    EXPECT_GE(c.channel->auth_renews_sent(), 1);
}

TEST_F(ChannelE2E, ClientMessagesAreQueuedOfflineAndDelivered) {
    Client c;
    // Queued before any connection exists.
    ASSERT_TRUE(c.channel->send("remote-task-progress", 999999, json{{"note", "queued offline"}}));
    EXPECT_EQ(c.ts.store().outbox_pending().size(), 1u);
    c.channel->start();
    ASSERT_TRUE(wait_for([&] { return c.channel->connected(); }, 10s));
    // The control plane answers an undelivered ref with an error, but the outbox entry is consumed either way.
    EXPECT_TRUE(wait_for([&] { return c.ts.store().outbox_pending().empty(); }, 10s));
}
