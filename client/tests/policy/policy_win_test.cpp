// client/policy-enforcement on Windows (add-policy-and-sandbox tasks 3.1-3.8): bundle verification and the encrypted
// cache, fail-closed, the path escape corpus, folder grants, egress redirects and the approval log.
#include "base/encoding.h"
#include "platform/crypto.h"
#include "platform/http.h"
#include "platform/win.h"
#include "policy/egress.h"
#include "policy/enforcer.h"
#include "policy/manager.h"
#include "policy/paths.h"
#include "policy/store.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/keystore.h"
#include "support/junction.h"

#include <windows.h>
#include <bcrypt.h>
#include <winioctl.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <thread>

using namespace wb;
using namespace wb::policy;

namespace {

class FakeClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override { return t; }
    sys_clock::time_point t = sys_clock::time_point(std::chrono::seconds(1'800'000'000));
    [[nodiscard]] long long secs() const { return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count(); }
};

struct TempDir {
    std::filesystem::path path = std::filesystem::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::string str(const std::wstring& sub = {}) const { return narrow((sub.empty() ? path : path / sub).wstring()); }
};

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// An app.db with all migrations in a fresh data root.
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

// An ES256 key that signs test bundles the way the control plane does (design D1).
class Signer {
public:
    explicit Signer(std::string kid) : kid_(std::move(kid)) {
        EXPECT_EQ(::BCryptOpenAlgorithmProvider(&alg_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0), 0);
        EXPECT_EQ(::BCryptGenerateKeyPair(alg_, &key_, 256, 0), 0);
        EXPECT_EQ(::BCryptFinalizeKeyPair(key_, 0), 0);
    }
    ~Signer() {
        ::BCryptDestroyKey(key_);
        ::BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    [[nodiscard]] json jwk() const {
        ULONG n = 0;
        ::BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &n, 0);
        bytes blob(n);
        ::BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), n, &n, 0);
        const size_t off = sizeof(BCRYPT_ECCKEY_BLOB);
        return {{"kty", "EC"}, {"crv", "P-256"}, {"kid", kid_}, {"alg", "ES256"}, {"use", "sig"},
                {"x", base64url_encode(blob.data() + off, 32)}, {"y", base64url_encode(blob.data() + off + 32, 32)}};
    }

    [[nodiscard]] std::string sign(const json& payload, const std::string& typ = "workbench-policy+jwt") const {
        const json header = {{"alg", "ES256"}, {"typ", typ}, {"kid", kid_}};
        const std::string input = base64url_encode(header.dump()) + "." + base64url_encode(payload.dump());
        bytes hash = sha256(input);
        std::uint8_t sig[64];
        ULONG n = 0;
        EXPECT_EQ(::BCryptSignHash(key_, nullptr, hash.data(), static_cast<ULONG>(hash.size()), sig, sizeof sig, &n, 0), 0);
        return input + "." + base64url_encode(sig, n);
    }

private:
    std::string kid_;
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

json payload(const FakeClock& clock, long long ver, const std::string& sub = "dev1@corp.local", json policy = json::object()) {
    return {{"sub", sub}, {"uid", "kc-1"}, {"ver", ver}, {"iat", clock.secs()}, {"exp", clock.secs() + 3600}, {"schema", 1},
            {"groups", {"developers"}}, {"policy", std::move(policy)}, {"src", json::object()}};
}

bool has_event(AppStore& s, const std::string& kind) {
    for (const auto& e : s.security_events())
        if (e.kind == kind) return true;
    return false;
}

struct ManagerFixture {
    TestStore ts;
    FakeClock clock;
    Signer key{"k1"};
    int key_fetches = 0;
    std::string bundle;
    std::unique_ptr<PolicyManager> make() {
        PolicyManager::Deps d;
        d.fetch_keys = [this]() -> Result<json> {
            ++key_fetches;
            return json{{"keys", json::array({key.jwk()})}};
        };
        d.fetch_bundle = [this]() -> Result<json> { return json{{"bundle", bundle}}; };
        d.user_principal = [] { return std::string("DEV1@corp.local"); };
        return std::make_unique<PolicyManager>(*ts.store, clock, d);
    }
};

}  // namespace

// ---- Bundle verification, cache and fail-closed ------------------------------------------------------------------------

TEST(PolicyManager, AcceptsAValidBundleAndFetchesKeysOnce) {
    ManagerFixture f;
    auto m = f.make();
    EXPECT_EQ(m->current(), nullptr);
    f.bundle = f.key.sign(payload(f.clock, 3));
    ASSERT_TRUE(m->refresh());
    ASSERT_NE(m->current(), nullptr);
    EXPECT_EQ(m->current()->version(), 3);
    EXPECT_EQ(m->status()["state"], "valid");
    EXPECT_FALSE(m->refresh_due());
    ASSERT_TRUE(m->accept(f.key.sign(payload(f.clock, 4))));
    EXPECT_EQ(f.key_fetches, 1);  // the second bundle verifies with the cached key
    EXPECT_EQ(m->version(), "4");
    f.clock.t += std::chrono::hours(6);
    EXPECT_TRUE(m->refresh_due());
}

TEST(PolicyManager, RejectsTamperedBundles) {
    ManagerFixture f;
    auto m = f.make();
    const std::string good = f.key.sign(payload(f.clock, 3));
    const size_t a = good.find('.'), b = good.find('.', a + 1);
    json forged = payload(f.clock, 3, "dev1@corp.local", {{"tools", {{"risk", {{"destructive", "allow"}}}}}});
    const std::string tampered = good.substr(0, a + 1) + base64url_encode(forged.dump()) + good.substr(b);
    auto s = m->accept(tampered);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, "policy_rejected");
    EXPECT_TRUE(has_event(*f.ts.store, "policy.bundle.tampered"));
    EXPECT_EQ(m->current(), nullptr);
    // Signed by a key the control plane does not publish.
    Signer rogue("k1");
    EXPECT_FALSE(m->accept(rogue.sign(payload(f.clock, 3))));
    // Wrong type or algorithm.
    EXPECT_FALSE(m->accept(f.key.sign(payload(f.clock, 3), "JWT")));
    EXPECT_TRUE(has_event(*f.ts.store, "policy.bundle.malformed"));
    EXPECT_FALSE(m->accept("not-a-jws"));
}

TEST(PolicyManager, RejectsExpiredRolledBackAndForeignBundles) {
    ManagerFixture f;
    auto m = f.make();
    json expired = payload(f.clock, 3);
    expired["exp"] = f.clock.secs() - 1;
    EXPECT_FALSE(m->accept(f.key.sign(expired)));
    EXPECT_FALSE(m->accept(f.key.sign(payload(f.clock, 3, "someone.else@corp.local"))));
    EXPECT_TRUE(has_event(*f.ts.store, "policy.bundle.subject"));
    ASSERT_TRUE(m->accept(f.key.sign(payload(f.clock, 5))));
    auto s = m->accept(f.key.sign(payload(f.clock, 4)));
    ASSERT_FALSE(s);
    EXPECT_NE(s.error().message.find("lower than 5"), std::string::npos);
    EXPECT_TRUE(has_event(*f.ts.store, "policy.bundle.rollback"));
    EXPECT_EQ(m->current()->version(), 5);
    // The anti-rollback floor survives a restart.
    auto again = f.make();
    EXPECT_FALSE(again->accept(f.key.sign(payload(f.clock, 4))));
    EXPECT_TRUE(again->accept(f.key.sign(payload(f.clock, 5))));
}

TEST(PolicyManager, CachedBundleAndFailClosed) {
    ManagerFixture f;
    {
        auto m = f.make();
        ASSERT_TRUE(m->accept(f.key.sign(payload(f.clock, 2, "dev1@corp.local", {{"tools", {{"rules", {{"web.fetch", "allow"}}}}}}))));
    }
    // A restart without the network: the cached bundle applies, verified with the cached keys.
    auto m = f.make();
    m->load_cached();
    ASSERT_NE(m->current(), nullptr);
    EXPECT_EQ(m->current()->version(), 2);
    EXPECT_EQ(f.key_fetches, 1);
    CallFacts fetch{"web.fetch", "external"};
    EXPECT_EQ(decide(m->current().get(), fetch).decision, Decision::allow);
    // Expired: fail closed until a fresh bundle arrives.
    f.clock.t += std::chrono::hours(2);
    EXPECT_EQ(m->current(), nullptr);
    EXPECT_EQ(m->status()["state"], "expired");
    EXPECT_EQ(decide(m->current().get(), fetch).decision, Decision::deny);
    CallFacts own{"ask_user", "read"};
    own.own_data = true;
    EXPECT_EQ(decide(m->current().get(), own).decision, Decision::allow);
    // A cached bundle that no longer verifies is discarded.
    ASSERT_TRUE(f.ts.store->set_secret("policy.bundle", "x.y.z"));
    auto m2 = f.make();
    m2->load_cached();
    EXPECT_EQ(m2->current(), nullptr);
    EXPECT_TRUE(has_event(*f.ts.store, "policy.cache.invalid"));
}

// ---- Filesystem escape corpus -----------------------------------------------------------------------------------------

namespace {

struct EscapeFixture {
    TempDir t;
    std::filesystem::path data = t.path / L"data", ws = data / L"workspace", outside = t.path / L"outside";
    PathRules rules;
    EscapeFixture() {
        std::filesystem::create_directories(ws);
        write_file(outside / L"secret.txt", "secret");
        write_file(data / L"app.db", "db");
        write_file(ws / L"note.txt", "note");
        rules = path_rules(nullptr, narrow(ws.wstring()), narrow(data.wstring()), {});
    }
    ~EscapeFixture() {
        for (const auto& j : junctions) ::RemoveDirectoryW(j.c_str());  // before remove_all, which must not walk them
    }
    // A mount-point reparse point (what mklink /J makes); junctions need no privilege. The target is its final path.
    bool junction(const std::filesystem::path& link, const std::filesystem::path& target) {
        auto final_target = canonicalize(narrow(target.wstring()));
        if (!final_target) return false;
        junctions.push_back(link);
        return test::make_junction(link, widen(*final_target));
    }
    std::vector<std::filesystem::path> junctions;
};

std::string s(const std::filesystem::path& p) { return narrow(p.wstring()); }

}  // namespace

TEST(PolicyPathsWin, WorkspaceIsReadableAndWritable) {
    EscapeFixture f;
    auto c = canonicalize(s(f.ws / L"note.txt"));
    ASSERT_TRUE(c) << c.error().message;
    auto r = open_for_read(s(f.ws / L"note.txt"), f.rules);
    EXPECT_TRUE(r) << (r ? "" : r.error().message) << " final=" << *c << " ws=" << f.rules.workspace;
    auto w = open_for_write(s(f.ws / L"new.txt"), WriteMode::create_new, f.rules);
    ASSERT_TRUE(w) << w.error().message;
    auto t = open_for_write(s(f.ws / L"note.txt"), WriteMode::truncate_or_create, f.rules);
    ASSERT_TRUE(t) << t.error().message;
    t->reset();
    EXPECT_EQ(std::filesystem::file_size(f.ws / L"note.txt"), 0u);
}

TEST(PolicyPathsWin, EscapesAreDenied) {
    EscapeFixture f;
    // Plain outside paths, relative traversal and the data root.
    EXPECT_FALSE(open_for_read(s(f.outside / L"secret.txt"), f.rules));
    EXPECT_FALSE(open_for_read(s(f.ws) + "\\..\\..\\outside\\secret.txt", f.rules));
    RuleVerdict v;
    EXPECT_FALSE(open_for_read(s(f.data / L"app.db"), f.rules, &v));
    EXPECT_EQ(v.rule, "data-root");
    EXPECT_FALSE(open_for_write(s(f.data / L"evil.txt"), WriteMode::create_new, f.rules));
    // Alternate data streams and device names.
    for (const std::string& p : std::vector<std::string>{s(f.ws / L"note.txt") + ":hidden", s(f.ws / L"note.txt") + "::$DATA", s(f.ws) + "\\CON",
                                                         "\\\\.\\C:", "\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\x", s(f.ws) + "\\nul.txt"}) {
        auto r = open_for_read(p, f.rules);
        ASSERT_FALSE(r) << p;
        EXPECT_EQ(r.error().code, "policy_denied") << p;
        EXPECT_FALSE(open_for_write(p, WriteMode::open_or_create, f.rules)) << p;
    }
    // An administrative share naming the workspace is a different final path and is not inside it.
    const std::string drive = s(f.ws).substr(0, 1);
    const std::string unc = "\\\\localhost\\" + drive + "$" + s(f.ws).substr(2) + "\\note.txt";
    EXPECT_FALSE(open_for_read(unc, f.rules)) << unc;
    EXPECT_FALSE(open_for_write("\\\\localhost\\" + drive + "$" + s(f.ws).substr(2) + "\\x.txt", WriteMode::create_new, f.rules));
}

TEST(PolicyPathsWin, JunctionsAreResolved) {
    EscapeFixture f;
    ASSERT_TRUE(f.junction(f.ws / L"j", f.outside)) << "mklink /J failed";
    const auto outside = canonicalize(s(f.outside));
    ASSERT_TRUE(outside);
    auto c = canonicalize(s(f.ws / L"j" / L"secret.txt"));
    ASSERT_TRUE(c) << c.error().message;
    EXPECT_TRUE(path_within(*c, *outside)) << *c;
    RuleVerdict v;
    EXPECT_FALSE(open_for_read(s(f.ws / L"j" / L"secret.txt"), f.rules, &v));
    EXPECT_FALSE(v.allowed);
    EXPECT_FALSE(open_for_write(s(f.ws / L"j" / L"planted.txt"), WriteMode::create_new, f.rules));
    EXPECT_FALSE(std::filesystem::exists(f.outside / L"planted.txt"));
    // A missing tail below a junction is resolved through the junction too.
    auto missing = canonicalize(s(f.ws / L"j" / L"no" / L"such.txt"));
    ASSERT_TRUE(missing);
    EXPECT_TRUE(path_within(*missing, *outside));
}

TEST(PolicyPathsWin, SymbolicLinksAreResolvedOrRefused) {
    EscapeFixture f;
    std::error_code ec;
    std::filesystem::create_symlink(f.outside / L"secret.txt", f.ws / L"link.txt", ec);
    if (ec) GTEST_SKIP() << "creating symbolic links is not permitted here: " << ec.message();
    EXPECT_FALSE(open_for_read(s(f.ws / L"link.txt"), f.rules));
    RuleVerdict v;
    auto w = open_for_write(s(f.ws / L"link.txt"), WriteMode::truncate_or_create, f.rules, &v);
    ASSERT_FALSE(w);
    EXPECT_EQ(v.rule, "link");
    EXPECT_EQ(std::filesystem::file_size(f.outside / L"secret.txt"), 6u);
}

TEST(PolicyPathsWin, HardLinksAreRefusedForWriting) {
    EscapeFixture f;
    std::error_code ec;
    std::filesystem::create_hard_link(f.outside / L"secret.txt", f.ws / L"hard.txt", ec);
    ASSERT_FALSE(ec) << ec.message();
    RuleVerdict v;
    auto w = open_for_write(s(f.ws / L"hard.txt"), WriteMode::truncate_or_create, f.rules, &v);
    ASSERT_FALSE(w);
    EXPECT_EQ(v.rule, "link");
    EXPECT_EQ(std::filesystem::file_size(f.outside / L"secret.txt"), 6u);
}

TEST(PolicyPathsWin, ShortNamesAreResolvedBeforeDenyRules) {
    EscapeFixture f;
    const auto dir = f.ws / L"VeryLongSecretFolderName";
    write_file(dir / L"key.pem", "k");
    wchar_t shortp[MAX_PATH];
    const DWORD n = ::GetShortPathNameW((dir / L"key.pem").c_str(), shortp, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || std::wstring(shortp).find(L"VeryLong") != std::wstring::npos)
        GTEST_SKIP() << "8.3 names are disabled on this volume";
    f.rules.deny = {"**\\VeryLongSecretFolderName\\**"};
    RuleVerdict v;
    EXPECT_FALSE(open_for_read(narrow(shortp), f.rules, &v));
    EXPECT_EQ(v.rule, "filesystem.deny[\"**\\VeryLongSecretFolderName\\**\"]");
}

TEST(PolicyPathsWin, PathRulesExpandVariables) {
    const EffectivePolicy p(PolicyBundle{"u", "", 1, 0, 4102444800, 1, {}, {{"filesystem", {{"read", {"%USERPROFILE%\\Documents\\**"}},
                                                                                            {"deny", {"%WORKBENCH_DATA%/secrets/**"}}}}}});
    const PathRules r = path_rules(&p, "C:\\D\\workspace", "C:\\D", {});
    ASSERT_EQ(r.read.size(), 1u);
    EXPECT_EQ(r.read[0].find('%'), std::string::npos);
    EXPECT_EQ(r.deny[0], "C:\\D\\secrets\\**");
}

// ---- Folder grants ----------------------------------------------------------------------------------------------------

TEST(PolicyGrants, OnlyWithinGrantable) {
    TestStore ts;
    FolderGrants g(ts.store->db());
    auto refused = g.grant("C:\\Windows\\System32", true, {"%USERPROFILE%\\**", "D:\\Shared\\**"});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "policy_denied");
    EXPECT_NE(refused.error().message.find("policy.filesystem.grantable"), std::string::npos);
    EXPECT_FALSE(g.grant("D:\\Shared\\x", false, {}));  // nothing is grantable by default
    ASSERT_TRUE(g.grant("D:\\Shared\\team", false, {"%USERPROFILE%\\**", "D:\\Shared\\**"}));
    ASSERT_TRUE(g.grant("D:\\Shared\\mine", true, {"D:\\Shared\\**"}));
    auto list = g.list();
    ASSERT_EQ(list.size(), 2u);
    ASSERT_TRUE(g.revoke("D:\\Shared\\team"));
    list = g.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].folder, "D:\\Shared\\mine");
    EXPECT_TRUE(list[0].write);
}

// improve-usability 2.3: a folder granted in Settings > Files can be read by the agent; a folder not granted cannot.
TEST(PolicyGrants, AgentReadsAGrantedFolder) {
    TestStore ts;
    FolderGrants g(ts.store->db());
    std::filesystem::create_directories(ts.dir.path / L"shared" / L"team");
    std::filesystem::create_directories(ts.dir.path / L"shared" / L"other");
    // As policy.grants.add does, the folder is canonicalized before it is granted.
    const std::string team = *canonicalize(ts.dir.str(L"shared\\team")), other = *canonicalize(ts.dir.str(L"shared\\other"));
    const std::string grantable = *canonicalize(ts.dir.str(L"shared")) + "\\**";
    ASSERT_TRUE(g.grant(team, false, {grantable}));
    auto policy = std::make_shared<const EffectivePolicy>(PolicyBundle{
        "u", "", 1, 0, 4102444800, 1, {"everyone"}, {{"filesystem", {{"grantable", {grantable}}}}}, {}});
    EnforcerEnv env;
    env.policy = [policy] { return policy; };
    env.workspace = ts.dir.str(L"ws");
    env.data_root = ts.dir.str(L"root");
    env.grants = [&g] { return g.list(); };
    PolicyEnforcer enf(env);
    runtime::ToolCallInfo call;
    call.tool = "files.read";
    call.risk = runtime::Risk::read;
    call.resources = {{runtime::ToolResource::Kind::read_path, team + "\\notes.txt"}};
    EXPECT_EQ(enf.evaluate(call).decision, Decision::allow) << enf.evaluate(call).reason;
    call.resources = {{runtime::ToolResource::Kind::read_path, other + "\\notes.txt"}};
    EXPECT_EQ(enf.evaluate(call).decision, Decision::deny);
    // Writing needs a read-write grant.
    call.tool = "files.write";
    call.risk = runtime::Risk::write_local;
    call.resources = {{runtime::ToolResource::Kind::write_path, team + "\\notes.txt"}};
    EXPECT_EQ(enf.evaluate(call).decision, Decision::deny);
}

// ---- Egress -----------------------------------------------------------------------------------------------------------

namespace {

// A loopback HTTP server answering every request with one fixed response; counts the connections it accepts.
class OneShotServer {
public:
    explicit OneShotServer(std::string response) : response_(std::move(response)) {
        WSADATA w;
        ::WSAStartup(MAKEWORD(2, 2), &w);
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        ::getsockname(sock_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        ::listen(sock_, 4);
        thread_ = std::thread([this] {
            for (;;) {
                SOCKET c = ::accept(sock_, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                ++accepted;
                std::string req;
                char buf[2048];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    const int n = ::recv(c, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    req.append(buf, n);
                }
                ::send(c, response_.data(), static_cast<int>(response_.size()), 0);
                ::shutdown(c, SD_SEND);
                ::closesocket(c);
            }
        });
    }
    ~OneShotServer() {
        ::closesocket(sock_);
        thread_.join();
        ::WSACleanup();
    }
    [[nodiscard]] std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    std::atomic<int> accepted{0};

private:
    std::string response_;
    SOCKET sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
};

std::shared_ptr<const EffectivePolicy> egress_policy(std::vector<std::string> allow, std::vector<std::string> deny) {
    return std::make_shared<const EffectivePolicy>(PolicyBundle{
        "u", "", 1, 0, 4102444800, 1, {}, {{"network", {{"egress_allow", std::move(allow)}, {"egress_deny", std::move(deny)}}}}});
}

}  // namespace

TEST(PolicyEgressWin, RedirectToADeniedHostIsNotFollowed) {
    if (!HttpClient::url_allowed("http://127.0.0.1:1/")) GTEST_SKIP() << "http loopback needs WORKBENCH_DEV_ENDPOINTS";
    OneShotServer srv("HTTP/1.1 302 Found\r\nLocation: http://exfil.pastebin.com/x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const auto policy = egress_policy({"127.0.0.1", "*.pastebin.com"}, {"*.pastebin.com"});
    EgressGuard guard([policy] { return policy; });
    HttpClient client;
    HttpRequest req;
    req.url = srv.url("/start");
    RuleVerdict denied;
    auto r = guarded_fetch(client, req, guard, 10, &denied);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "policy_denied");
    EXPECT_EQ(denied.rule, "network.egress_deny[\"*.pastebin.com\"]");
    EXPECT_EQ(srv.accepted.load(), 1);
}

TEST(PolicyEgressWin, DeniedFirstUrlIsNeverContacted) {
    if (!HttpClient::url_allowed("http://127.0.0.1:1/")) GTEST_SKIP() << "http loopback needs WORKBENCH_DEV_ENDPOINTS";
    OneShotServer srv("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
    HttpClient client;
    HttpRequest req;
    req.url = srv.url("/");
    const auto none = egress_policy({}, {});
    EXPECT_FALSE(guarded_fetch(client, req, EgressGuard([none] { return none; })));
    EXPECT_FALSE(guarded_fetch(client, req, EgressGuard([] { return std::shared_ptr<const EffectivePolicy>(); })));
    EXPECT_EQ(srv.accepted.load(), 0);
    const auto allow = egress_policy({"127.0.0.1"}, {});
    auto ok = guarded_fetch(client, req, EgressGuard([allow] { return allow; }));
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(ok->body, "ok");
    EXPECT_EQ(resolve_location("https://a.b/c/d?q", "/x"), "https://a.b/x");
    EXPECT_EQ(resolve_location("https://a.b/c/d", "e"), "https://a.b/c/e");
    EXPECT_EQ(resolve_location("https://a.b/c", "//evil.example/x"), "https://evil.example/x");
}

// ---- Enforcer and the approval log ------------------------------------------------------------------------------------

TEST(PolicyEnforcerWin, DecisionsAreLoggedWithRuleAndSource) {
    TestStore ts;
    DecisionLog log(ts.store->db());
    auto policy = std::make_shared<const EffectivePolicy>(PolicyBundle{
        "u", "", 9, 0, 4102444800, 1, {"developers"},
        {{"tools", {{"rules", {{"web.fetch", "deny"}}}, {"shell", {{"mode", "off"}}}}}, {"network", {{"egress_allow", {"*.corp.local"}}}}},
        {{"tools.rules[\"web.fetch\"]", "security-team"}}});
    EnforcerEnv env;
    env.policy = [policy] { return policy; };
    env.workspace = ts.dir.str(L"ws");
    env.data_root = ts.dir.str(L"root");
    env.grants = [] { return std::vector<FolderGrant>{}; };
    env.log = &log;
    PolicyEnforcer enf(env);

    runtime::ToolCallInfo call;
    call.run.run_id = "run-1";
    call.call_id = "call-1";
    call.tool = "web.fetch";
    call.risk = runtime::Risk::external;
    EXPECT_EQ(enf.before_tool_call(call).kind, runtime::ToolDecision::Kind::deny);
    auto rec = log.for_call("call-1");
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->outcome, "deny");
    EXPECT_EQ(rec->rule, "tools.rules[\"web.fetch\"]");
    EXPECT_EQ(rec->source, "security-team");
    EXPECT_EQ(rec->policy_version, 9);

    // A URL argument outside the egress rules denies an otherwise approvable call.
    call.call_id = "call-2";
    call.tool = "browser.open";
    call.resources = {{runtime::ToolResource::Kind::url, "https://example.com/"}};
    EXPECT_EQ(enf.before_tool_call(call).kind, runtime::ToolDecision::Kind::deny);
    EXPECT_NE(log.for_call("call-2")->rule.find("egress"), std::string::npos);
    call.call_id = "call-3";
    call.resources = {{runtime::ToolResource::Kind::url, "https://wiki.corp.local/"}};
    EXPECT_EQ(enf.before_tool_call(call).kind, runtime::ToolDecision::Kind::require_approval);
    enf.record_resolution("run-1", "call-3", "browser.open", "approve");
    bool resolution = false;
    for (const auto& r : log.recent())
        if (r.kind == "resolution" && r.call_id == "call-3" && r.outcome == "approve") resolution = true;
    EXPECT_TRUE(resolution);

    // A path argument outside the filesystem rules.
    call.call_id = "call-4";
    call.tool = "files.read";
    call.risk = runtime::Risk::read;
    call.resources = {{runtime::ToolResource::Kind::read_path, "C:\\Windows\\win.ini"}};
    EXPECT_EQ(enf.before_tool_call(call).kind, runtime::ToolDecision::Kind::deny);

    // Switched-off and denied tools are hidden; the shell is off.
    EXPECT_FALSE(enf.tool_permitted("shell.run", runtime::Risk::destructive));
    EXPECT_FALSE(enf.tool_permitted("web.fetch", runtime::Risk::external));
    EXPECT_TRUE(enf.tool_permitted("files.read", runtime::Risk::read));
    EXPECT_TRUE(enf.may_require_approval("browser.open", runtime::Risk::external));
    EXPECT_FALSE(enf.may_require_approval("files.read", runtime::Risk::read));
}
