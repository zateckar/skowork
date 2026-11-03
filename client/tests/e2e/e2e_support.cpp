#include "e2e_support.h"

#include "base/jwt.h"
#include "base/strings.h"
#include "base/url.h"
#include "platform/crypto.h"
#include "platform/http.h"
#include "platform/log.h"
#include "platform/win.h"
#include "storage/data_root.h"
#include "storage/database.h"
#include "storage/keystore.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace wb::e2e {

namespace {

// The repository .env (found by walking up from the test executable). Only the OIDC_* names are read.
std::string dotenv(const std::string& name) {
    for (auto dir = exe_dir(); !dir.empty(); dir = dir.parent_path()) {
        std::ifstream f(dir / L".env");
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                line = std::string(trim(line));
                if (line.rfind(name + "=", 0) != 0) continue;
                std::string v = line.substr(name.size() + 1);
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                return v;
            }
            return {};
        }
        if (dir == dir.parent_path()) break;
    }
    return {};
}

std::string setting(const char* name, const std::string& fallback = {}) {
    if (const char* v = std::getenv(name); v && *v) return v;
    if (std::string v = dotenv(name); !v.empty()) return v;
    return fallback;
}

Result<HttpResponse> get(const std::string& url) {
    HttpRequest r;
    r.method = "GET";
    r.url = url;
    r.connect_timeout = std::chrono::milliseconds(3000);
    r.idle_timeout = std::chrono::milliseconds(5000);
    return http().send(r);
}

std::pair<int, json> post_form(const std::string& url, const std::vector<std::pair<std::string, std::string>>& form) {
    HttpRequest r;
    r.method = "POST";
    r.url = url;
    r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    r.body = form_encode(form);
    auto resp = http().send(r);
    if (!resp) return {0, json{{"transport_error", resp.error().message}}};
    auto j = parse_json(resp->body);
    return {resp->status, j ? *j : json(resp->body)};
}

}  // namespace

std::string& skip_reason() {
    static std::string r;
    return r;
}

const std::optional<Env>& env() {
    static const std::optional<Env> e = []() -> std::optional<Env> {
        // WORKBENCH_E2E_LOG=<dir>: client diagnostic logs (verbose) for the test process.
        if (const std::string dir = setting("WORKBENCH_E2E_LOG"); !dir.empty()) {
            log::Options o;
            o.dir = widen(dir);
            log::init(o);
            log::set_verbose_until(std::chrono::system_clock::now() + std::chrono::hours(2));
        }
        Env x;
        auto fail = [](std::string why) -> std::optional<Env> {
            skip_reason() = std::move(why);
            return std::nullopt;
        };
        std::string discovery_url = setting("OIDC_DISCOVERY_URL");
        x.client_id = setting("OIDC_CLIENT_ID");
        x.redirect_uri = setting("OIDC_REDIRECT_URI", "http://localhost:4200/auth/callback");
        x.scope = setting("OIDC_SCOPE", "openid profile email");
        x.cp = setting("WORKBENCH_E2E_CP", "http://localhost:8080");
        if (discovery_url.empty() || x.client_id.empty()) return fail("OIDC_DISCOVERY_URL / OIDC_CLIENT_ID not set (.env)");
        (void)cp_proxy();
        auto d = get(discovery_url);
        if (!d) return fail("identity provider discovery: " + d.error().message);
        if (d->status != 200) return fail("identity provider discovery: HTTP " + std::to_string(d->status));
        auto doc = parse_json(d->body);
        if (!doc) return fail("identity provider discovery is not JSON");
        x.discovery = *doc;
        x.issuer = jstr(*doc, "issuer");
        auto ready = get(x.cp + "/readyz");
        if (!ready || ready->status != 200) return fail("control plane " + x.cp + " not ready (dev/scripts/dev-stack.ps1)");
        return x;
    }();
    return e;
}

bool slow_tests_enabled() { return !setting("WORKBENCH_E2E_SLOW").empty(); }

IdentityConfig desktop_config() {
    IdentityConfig c;
    c.issuer = env()->issuer;
    c.client_id = env()->client_id;
    c.redirect_uri = env()->redirect_uri;
    c.scope = env()->scope;
    return c;
}

TempStore::TempStore() {
    root_ = std::filesystem::temp_directory_path() / L"workbench-test" / widen("e2e-" + random_urlsafe(6));
    auto r = DataRoot::open(root_);
    if (!r) throw std::runtime_error("data root: " + r.error().message);
    KeyStore keys(r->keys());
    (void)keys.load();
    auto key = keys.get_or_create("app.db");
    MigrationOutcome o;
    auto db = open_and_migrate(r->file("app.db"), *key, app_migrations(), o);
    if (!db) throw std::runtime_error("app.db: " + db.error().message);
    store_ = std::make_unique<AppStore>(std::move(*db));
}

TempStore::~TempStore() {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
}

Status sign_in(TokenManager& tokens, std::chrono::seconds timeout) {
    static std::once_flag hint;
    std::call_once(hint, [] {
        std::printf("\n>>> Workbench e2e: complete the sign-in in your browser if it asks (later sign-ins are automatic).\n\n");
        std::fflush(stdout);
    });
    if (Status s = tokens.begin_sign_in(); !s) return s;
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        const AuthState st = tokens.state();
        if (st == AuthState::signed_in) return {};
        if (st == AuthState::sign_in_required) return make_error("sign_in", tokens.last_error());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return make_error("timeout", "the browser sign-in did not complete");
}

int SharedSession::subscribe(std::function<void()> fn) {
    const std::lock_guard lock(mx_);
    subs_[++next_] = std::move(fn);
    return next_;
}

void SharedSession::unsubscribe(int id) {
    const std::lock_guard lock(mx_);
    subs_.erase(id);
}

void SharedSession::notify() {
    const std::lock_guard lock(mx_);
    for (auto& [id, fn] : subs_) fn();
}

Result<SharedSession*> shared_session() {
    static std::mutex mx;
    static SharedSession* session = nullptr;  // kept for the process lifetime
    const std::lock_guard lock(mx);
    if (!session) {
        session = new SharedSession();
        session->tm.on_new_access_token([](const std::string&, long long) { session->notify(); });
        session->tm.on_state_changed([](AuthState) { session->notify(); });
    }
    if (session->tm.state() != AuthState::signed_in) {
        if (Status s = sign_in(session->tm); !s) return s.error();
    }
    return session;
}

Result<std::string> admin_token() {
    auto s = shared_session();
    if (!s) return s.error();
    return (*s)->tm.access_token();
}

std::string user_name_of(const std::string& access_token) {
    auto jwt = parse_jwt(access_token);
    return jwt ? jstr(jwt->payload, "preferred_username") : std::string{};
}

std::pair<int, json> cp_call(const std::string& method, const std::string& path, const std::string& token, const json& body) {
    HttpRequest r;
    r.method = method;
    r.url = env()->cp + path;
    r.headers = {{"Authorization", "Bearer " + token}, {"Accept", "application/json"}};
    if (!body.is_null()) {
        r.headers.emplace_back("Content-Type", "application/json");
        r.body = body.dump();
    }
    auto resp = http().send(r);
    if (!resp) return {0, json{{"transport_error", resp.error().message}}};
    auto j = parse_json(resp->body);
    return {resp->status, j ? *j : json(resp->body)};
}

std::pair<int, json> idp_token(const std::vector<std::pair<std::string, std::string>>& form) {
    return post_form(jstr(env()->discovery, "token_endpoint"), form);
}

std::pair<int, json> idp_revoke(const std::string& refresh_token) {
    return post_form(jstr(env()->discovery, "revocation_endpoint"),
                     {{"token", refresh_token}, {"token_type_hint", "refresh_token"}, {"client_id", env()->client_id}});
}
// --- TcpProxy ----------------------------------------------------------------------------------------------

namespace {
struct WsaInit {
    WsaInit() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
};
}  // namespace

TcpProxy* cp_proxy() {
    static TcpProxy* p = []() -> TcpProxy* {
        const std::string upstream = setting("WORKBENCH_E2E_CP_UPSTREAM");
        if (upstream.empty()) return nullptr;
        const auto cp = parse_url(setting("WORKBENCH_E2E_CP", "http://localhost:8080"));
        return new TcpProxy(cp->port, std::atoi(upstream.c_str()));
    }();
    return p;
}

TcpProxy::TcpProxy(int listen_port, int target_port) : target_port_(target_port) {
    static WsaInit init;
    SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(listen_port));
    ::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(l, 16);
    int len = sizeof a;
    ::getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    listener_ = l;
    accept_thread_ = std::thread([this] { accept_loop(); });
}

TcpProxy::~TcpProxy() {
    stopping_ = true;
    set_down(true);
    ::closesocket(static_cast<SOCKET>(listener_));
    if (accept_thread_.joinable()) accept_thread_.join();
    std::vector<std::thread> pumps;
    {
        const std::lock_guard lock(mx_);
        pumps.swap(pumps_);
    }
    for (auto& t : pumps)
        if (t.joinable()) t.join();
}

void TcpProxy::set_down(bool down) {
    down_ = down;
    if (!down) return;
    const std::lock_guard lock(mx_);
    for (auto s : sockets_) ::shutdown(static_cast<SOCKET>(s), SD_BOTH);
}

void TcpProxy::accept_loop() {
    while (!stopping_) {
        SOCKET c = ::accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        if (down_) {
            ::closesocket(c);
            continue;
        }
        SOCKET u = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(target_port_));
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::connect(u, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            ::closesocket(c);
            ::closesocket(u);
            continue;
        }
        const std::lock_guard lock(mx_);
        sockets_.push_back(c);
        sockets_.push_back(u);
        auto pump = [](SOCKET from, SOCKET to) {
            char buf[16384];
            for (;;) {
                const int n = ::recv(from, buf, sizeof buf, 0);
                if (n <= 0) break;
                if (::send(to, buf, n, 0) != n) break;
            }
            ::shutdown(to, SD_BOTH);
            ::shutdown(from, SD_BOTH);
        };
        pumps_.emplace_back([this, pump, c, u] {
            std::thread other([pump, c, u] { pump(u, c); });
            pump(c, u);
            other.join();
            const std::lock_guard l(mx_);
            std::erase(sockets_, static_cast<std::uintptr_t>(c));
            std::erase(sockets_, static_cast<std::uintptr_t>(u));
            ::closesocket(c);
            ::closesocket(u);
        });
    }
}

}  // namespace wb::e2e
