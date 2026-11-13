#include "sandbox/proxy.h"

#include "base/encoding.h"
#include "base/strings.h"
#include "platform/crypto.h"
#include "platform/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace wb::sandbox {

namespace {

constexpr const char* kLog = "egress-proxy";
constexpr size_t kMaxHead = 16 * 1024;
constexpr DWORD kHeadTimeoutMs = 30000;
constexpr long kIdleTimeoutS = 300;
constexpr const char* kUser = "skowork";

struct Wsa {
    bool ok = false;
    Wsa() {
        WSADATA w;
        ok = ::WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }
    ~Wsa() {
        if (ok) ::WSACleanup();
    }
};

bool wsa_ready() {
    static Wsa w;
    return w.ok;
}

SOCKET as_socket(std::uintptr_t s) { return static_cast<SOCKET>(s); }

bool send_all(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        const int k = ::send(s, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
        if (k <= 0) return false;
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

void reply(SOCKET s, int code, const char* text, const std::string& extra = {}) {
    const std::string body = std::to_string(code) + " " + text + "\n";
    const std::string msg = "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\n" + extra + "Content-Type: text/plain\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send_all(s, msg.data(), msg.size());
}

// Reads the request head; `rest` receives any bytes after it.
bool read_head(SOCKET s, std::string& head, std::string& rest) {
    std::string buf;
    char tmp[4096];
    for (;;) {
        const size_t end = buf.find("\r\n\r\n");
        if (end != std::string::npos) {
            head = buf.substr(0, end + 2);
            rest = buf.substr(end + 4);
            return true;
        }
        if (buf.size() > kMaxHead) return false;
        const int n = ::recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
}

// Loopback, unspecified, link-local (including cloud metadata addresses) and multicast destinations are never
// reachable through the proxy, whatever the host name resolves to.
bool forbidden_address(const sockaddr* a) {
    if (a->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(a);
        const auto b = reinterpret_cast<const unsigned char*>(&v4->sin_addr);
        return b[0] == 127 || b[0] == 0 || (b[0] == 169 && b[1] == 254) || b[0] >= 224;
    }
    if (a->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(a);
        const auto* b = v6->sin6_addr.s6_addr;
        static const unsigned char loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static const unsigned char any[16] = {};
        if (!std::memcmp(b, loop, 16) || !std::memcmp(b, any, 16)) return true;
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;  // fe80::/10
        if (b[0] == 0xFF) return true;                           // multicast
        // IPv4-mapped addresses are checked as IPv4.
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (!std::memcmp(b, mapped, 12)) {
            sockaddr_in v4{};
            v4.sin_family = AF_INET;
            std::memcpy(&v4.sin_addr, b + 12, 4);
            return forbidden_address(reinterpret_cast<const sockaddr*>(&v4));
        }
        return false;
    }
    return true;
}

// Resolves once and connects to the first permitted address; the connection stays on that address (pinned).
SOCKET connect_pinned(const std::string& host, int port, bool loopback_ok, std::string& why) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        why = "the host name does not resolve";
        return INVALID_SOCKET;
    }
    SOCKET out = INVALID_SOCKET;
    bool only_forbidden = true;
    for (addrinfo* a = res; a && out == INVALID_SOCKET; a = a->ai_next) {
        const bool loopback = (a->ai_family == AF_INET && reinterpret_cast<const unsigned char*>(
                                                             &reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr)[0] == 127);
        if (forbidden_address(a->ai_addr) && !(loopback_ok && loopback)) continue;
        only_forbidden = false;
        SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) out = s;
        else ::closesocket(s);
    }
    ::freeaddrinfo(res);
    if (out == INVALID_SOCKET) why = only_forbidden ? "the host resolves to a local address" : "the connection failed";
    return out;
}

// Copies bytes both ways until either side closes or the connection is idle for kIdleTimeoutS.
void splice(SOCKET a, SOCKET b) {
    char buf[64 * 1024];
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(a, &r);
        FD_SET(b, &r);
        timeval tv{kIdleTimeoutS, 0};
        const int n = ::select(0, &r, nullptr, nullptr, &tv);
        if (n <= 0) return;
        for (SOCKET from : {a, b}) {
            if (!FD_ISSET(from, &r)) continue;
            const int k = ::recv(from, buf, sizeof buf, 0);
            if (k <= 0) return;
            if (!send_all(from == a ? b : a, buf, static_cast<size_t>(k))) return;
        }
    }
}

std::string header_value(const std::vector<std::string>& lines, std::string_view name) {
    for (size_t i = 1; i < lines.size(); ++i) {
        const size_t c = lines[i].find(':');
        if (c != std::string::npos && iequals(trim(lines[i].substr(0, c)), name)) return trim(lines[i].substr(c + 1));
    }
    return {};
}

std::string normalise_host(std::string host) {
    host = to_lower(host);
    while (!host.empty() && host.back() == '.') host.pop_back();
    return host;
}

}  // namespace

bool split_authority(const std::string& authority, int default_port, std::string& host, int& port) {
    port = default_port;
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    std::string port_text;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port_text = authority.substr(close + 2);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') != colon) return false;  // bare IPv6 without brackets
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port_text = authority.substr(colon + 1);
    }
    if (!port_text.empty()) {
        if (port_text.size() > 5 || port_text.find_first_not_of("0123456789") != std::string::npos) return false;
        port = std::atoi(port_text.c_str());
    }
    return !host.empty() && port > 0 && port < 65536;
}

// ---- Sessions -------------------------------------------------------------------------------------------------------

EgressProxy::EgressProxy(HostCheck check, DenialSink on_denied, bool loopback_upstream)
    : check_(std::move(check)), on_denied_(std::move(on_denied)), loopback_upstream_(loopback_upstream) {}

EgressProxy::~EgressProxy() { stop(); }

std::string EgressProxy::Session::proxy_url() const {
    return std::string("http://") + kUser + ":" + credential_ + "@127.0.0.1:" + std::to_string(port_);
}

EgressProxy::Session::~Session() { proxy_.close_session(id_); }

Result<std::unique_ptr<EgressProxy::Session>> EgressProxy::open(Owner owner) {
    const std::lock_guard lock(mx_);
    if (as_socket(listen_) == INVALID_SOCKET)
        if (Status s = start_locked(); !s) return s.error();
    const long long id = next_id_++;
    std::string cred = random_urlsafe(16);
    sessions_[id] = Entry{cred, std::move(owner)};
    return std::unique_ptr<Session>(new Session(*this, id, port_, std::move(cred)));
}

void EgressProxy::close_session(long long id) {
    bool last = false;
    {
        const std::lock_guard lock(mx_);
        sessions_.erase(id);
        last = sessions_.empty();
    }
    if (last) stop();  // the proxy runs only while sandboxed processes with network access exist
}

bool EgressProxy::running() const {
    const std::lock_guard lock(mx_);
    return as_socket(listen_) != INVALID_SOCKET;
}

Status EgressProxy::start_locked() {
    if (!wsa_ready()) return make_error("sandbox", "Winsock is unavailable");
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return make_error("sandbox", "cannot create the proxy socket");
    BOOL excl = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 64) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        ::closesocket(s);
        return make_error("sandbox", "cannot listen on loopback for the egress proxy");
    }
    listen_ = static_cast<std::uintptr_t>(s);
    port_ = ntohs(a.sin_port);
    acceptor_ = std::thread([this, l = listen_] { accept_loop(l); });
    log::info(kLog, "listening on 127.0.0.1:" + std::to_string(port_));
    return {};
}

void EgressProxy::stop() {
    std::uintptr_t l;
    std::thread acceptor;
    {
        const std::lock_guard lock(mx_);
        l = listen_;
        listen_ = ~std::uintptr_t{0};
        port_ = 0;
        acceptor = std::move(acceptor_);
        for (auto s : open_sockets_) ::shutdown(as_socket(s), SD_BOTH);
    }
    if (as_socket(l) != INVALID_SOCKET) ::closesocket(as_socket(l));
    if (acceptor.joinable()) acceptor.join();
    std::unique_lock lock(mx_);
    idle_.wait(lock, [this] { return connections_.load() == 0; });
    if (as_socket(l) != INVALID_SOCKET) log::info(kLog, "stopped");
}

void EgressProxy::accept_loop(std::uintptr_t l) {
    for (;;) {
        SOCKET c = ::accept(as_socket(l), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;  // closed by stop()
        {
            const std::lock_guard lock(mx_);
            if (connections_.load() >= kMaxConnections) {
                reply(c, 503, "Service Unavailable");
                ::closesocket(c);
                continue;
            }
            ++connections_;
            open_sockets_.push_back(static_cast<std::uintptr_t>(c));
        }
        std::thread([this, c] { serve(static_cast<std::uintptr_t>(c)); }).detach();
    }
}

std::optional<EgressProxy::Owner> EgressProxy::authenticate(const std::string& value) {
    if (value.size() < 6 || !iequals(value.substr(0, 6), "Basic ")) return std::nullopt;
    const auto decoded = base64_decode(trim(value.substr(6)));
    if (!decoded) return std::nullopt;
    const std::string pair = to_string(*decoded);
    const size_t colon = pair.find(':');
    if (colon == std::string::npos) return std::nullopt;
    const std::string password = pair.substr(colon + 1);
    const std::lock_guard lock(mx_);
    std::optional<Owner> found;
    for (const auto& [id, e] : sessions_)
        if (constant_time_equals(password, e.credential) && !found) found = e.owner;  // every entry is compared
    return found;
}

// ---- One client connection --------------------------------------------------------------------------------------------

void EgressProxy::serve(std::uintptr_t client_raw) {
    const SOCKET client = as_socket(client_raw);
    SOCKET upstream = INVALID_SOCKET;
    const auto register_socket = [this](SOCKET s) {
        const std::lock_guard lock(mx_);
        open_sockets_.push_back(static_cast<std::uintptr_t>(s));
    };
    const auto finish = [&] {
        {
            const std::lock_guard lock(mx_);
            for (SOCKET s : {client, upstream}) {
                if (s == INVALID_SOCKET) continue;
                std::erase(open_sockets_, static_cast<std::uintptr_t>(s));
                ::closesocket(s);
            }
            --connections_;
        }
        idle_.notify_all();
    };

    DWORD timeout = kHeadTimeoutMs;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    std::string head, rest;
    if (!read_head(client, head, rest)) {
        reply(client, 400, "Bad Request");
        return finish();
    }
    std::vector<std::string> lines;
    for (size_t at = 0; at < head.size();) {
        const size_t e = head.find("\r\n", at);
        lines.push_back(head.substr(at, e - at));
        at = e + 2;
    }
    const auto parts = split(lines.empty() ? std::string{} : lines[0], ' ');
    if (parts.size() != 3 || !starts_with(parts[2], "HTTP/1.")) {
        reply(client, 400, "Bad Request");
        return finish();
    }
    const auto owner = authenticate(header_value(lines, "Proxy-Authorization"));
    if (!owner) {
        reply(client, 407, "Proxy Authentication Required", "Proxy-Authenticate: Basic realm=\"Skowork\"\r\n");
        return finish();
    }

    // The destination: CONNECT host:port, or an absolute http:// URL.
    const bool tunnel = parts[0] == "CONNECT";
    std::string host, path = "/";
    int port = 0;
    if (tunnel) {
        if (!split_authority(parts[1], 443, host, port)) {
            reply(client, 400, "Bad Request");
            return finish();
        }
    } else {
        const std::string& target = parts[1];
        if (target.size() < 8 || !iequals(target.substr(0, 7), "http://")) {
            reply(client, 400, "Bad Request");  // https:// goes through CONNECT; origin-form is not a proxy request
            return finish();
        }
        const size_t end = target.find_first_of("/?#", 7);
        if (!split_authority(target.substr(7, end == std::string::npos ? std::string::npos : end - 7), 80, host, port)) {
            reply(client, 400, "Bad Request");
            return finish();
        }
        if (end != std::string::npos) path = target[end] == '/' ? target.substr(end) : "/" + target.substr(end);
    }
    host = normalise_host(host);

    // The egress rules, then a pinned connection to a permitted address.
    policy::RuleVerdict v = owner->no_network ? policy::RuleVerdict{false, "no-network", "the process runs in no-network mode"}
                            : check_                ? check_(host)
                                                    : policy::RuleVerdict{false, "fail-closed", "no valid policy is in force"};
    std::string why;
    if (v.allowed) {
        upstream = connect_pinned(host, port, loopback_upstream_, why);
        if (upstream == INVALID_SOCKET && why == "the host resolves to a local address")
            v = {false, "local-address", "the host " + host + " resolves to a local address"};
    }
    if (!v.allowed) {
        if (on_denied_) on_denied_(*owner, host, v);
        log::info(kLog, "denied " + host + " (" + v.rule + ") for " + owner->tool);
        reply(client, 403, "Forbidden");
        return finish();
    }
    if (upstream == INVALID_SOCKET) {
        reply(client, 502, "Bad Gateway");
        return finish();
    }
    register_socket(upstream);
    timeout = 0;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);

    if (tunnel) {
        const char ok[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (send_all(client, ok, sizeof ok - 1) && (rest.empty() || send_all(upstream, rest.data(), rest.size()))) splice(client, upstream);
        return finish();
    }
    // Forward: origin-form request line, hop-by-hop and proxy headers removed, one destination per connection.
    std::string req = parts[0] + " " + path + " " + parts[2] + "\r\n";
    for (size_t i = 1; i < lines.size(); ++i) {
        const size_t c = lines[i].find(':');
        const std::string name = c == std::string::npos ? std::string{} : trim(lines[i].substr(0, c));
        if (iequals(name, "Proxy-Authorization") || iequals(name, "Proxy-Connection") || iequals(name, "Connection") ||
            iequals(name, "Keep-Alive") || lines[i].empty())
            continue;
        req += lines[i] + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    if (send_all(upstream, req.data(), req.size()) && (rest.empty() || send_all(upstream, rest.data(), rest.size()))) splice(client, upstream);
    finish();
}

}  // namespace wb::sandbox
