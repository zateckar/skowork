#include <winsock2.h>
#include <ws2tcpip.h>

#include "identity/loopback_listener.h"

#include "base/url.h"

#include <cstring>

namespace wb {
namespace {

constexpr char kRefusedPage[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Skowork</title></head>"
    "<body style=\"font-family:Segoe UI,sans-serif;margin:3em\"><h1>Sign-in not completed</h1>"
    "<p>This response does not belong to a pending Skowork sign-in.</p></body></html>";

constexpr char kPage[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Skowork</title></head>"
    "<body style=\"font-family:Segoe UI,sans-serif;margin:3em\"><h1>Signed in to Skowork</h1>"
    "<p>You can close this tab and return to Skowork.</p></body></html>";

constexpr SOCKET kInvalid = INVALID_SOCKET;

SOCKET as_socket(std::uintptr_t s) { return static_cast<SOCKET>(s); }

void close_socket(std::uintptr_t& s) {
    if (as_socket(s) != kInvalid) ::closesocket(as_socket(s));
    s = static_cast<std::uintptr_t>(kInvalid);
}

struct WsaInit {
    bool ok = false;
    WsaInit() {
        WSADATA d;
        ok = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WsaInit() {
        if (ok) ::WSACleanup();
    }
};

WsaInit& wsa() {
    static WsaInit w;
    return w;
}

// Binds one loopback address with exclusive use, so that a port held by another process is detected.
Result<SOCKET> bind_loopback(int family, int port) {
    SOCKET s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) return make_error("socket", "cannot create a socket");
    BOOL on = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
    int rc;
    if (family == AF_INET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        rc = ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    } else {
        DWORD v6only = 1;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(static_cast<u_short>(port));
        a.sin6_addr = in6addr_loopback;
        rc = ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    }
    if (rc != 0) {
        const int err = ::WSAGetLastError();
        ::closesocket(s);
        if (err == WSAEADDRINUSE || err == WSAEACCES)
            return make_error("port_in_use", "port " + std::to_string(port) + " is in use by another program");
        return make_error("socket", "cannot listen on the loopback interface (" + std::to_string(err) + ")");
    }
    if (::listen(s, 8) != 0) {
        ::closesocket(s);
        return make_error("socket", "cannot listen on the loopback interface");
    }
    u_long nonblocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonblocking);
    return s;
}

// Reads the request head (up to 8 KiB, 2 s) and returns the request target of a GET, or "" when malformed.
std::string read_target(SOCKET c) {
    u_long blocking = 0;
    ::ioctlsocket(c, FIONBIO, &blocking);
    DWORD timeout_ms = 2000;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
    std::string head;
    char buf[1024];
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        const int n = ::recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        head.append(buf, static_cast<size_t>(n));
    }
    const auto eol = head.find("\r\n");
    if (eol == std::string::npos || head.rfind("GET ", 0) != 0) return {};
    const std::string line = head.substr(4, eol - 4);
    const auto sp = line.find(' ');
    return sp == std::string::npos ? std::string{} : line.substr(0, sp);
}

void respond(SOCKET c, int status, const char* reason, const std::string& body) {
    const std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                          "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                          "Referrer-Policy: no-referrer\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
    ::send(c, r.data(), static_cast<int>(r.size()), 0);
    ::shutdown(c, SD_SEND);
}

}  // namespace

bool LoopbackListener::is_loopback_redirect(const std::string& redirect_uri) {
    const auto u = parse_url(redirect_uri);
    return u && u->scheme == "http" && u->is_loopback() && u->port > 0 && !u->path.empty();
}

Result<std::unique_ptr<LoopbackListener>> LoopbackListener::start(const std::string& redirect_uri, Callbacks cb,
                                                                   std::chrono::milliseconds timeout) {
    if (!is_loopback_redirect(redirect_uri)) return make_error("redirect", "the redirect URI is not an http loopback URI");
    if (!wsa().ok) return make_error("socket", "Windows Sockets is unavailable");
    const auto u = parse_url(redirect_uri);
    std::unique_ptr<LoopbackListener> l(new LoopbackListener());
    l->port_ = u->port;
    l->host_ = u->host;
    l->path_ = u->path;
    l->cb_ = std::move(cb);
    auto v4 = bind_loopback(AF_INET, u->port);
    if (!v4) return v4.error();
    l->sockets_[0] = static_cast<std::uintptr_t>(*v4);
    // "localhost" may resolve to ::1 first; listen there too when the machine has IPv6.
    if (auto v6 = bind_loopback(AF_INET6, u->port)) {
        l->sockets_[1] = static_cast<std::uintptr_t>(*v6);
    } else if (v6.error().code == "port_in_use") {
        close_socket(l->sockets_[0]);
        return v6.error();
    }
    l->listening_ = true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    LoopbackListener* self = l.get();
    l->thread_ = std::thread([self, deadline] { self->run(deadline); });
    return l;
}

LoopbackListener::~LoopbackListener() {
    cancelled_ = true;
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
        else thread_.join();
    }
    for (auto& s : sockets_) close_socket(s);
}

void LoopbackListener::run(std::chrono::steady_clock::time_point deadline) {
    std::string delivered;
    bool timed_out = false;
    while (!cancelled_ && delivered.empty()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        fd_set set;
        FD_ZERO(&set);
        for (auto s : sockets_)
            if (as_socket(s) != kInvalid) FD_SET(as_socket(s), &set);
        timeval tv{0, 200 * 1000};  // wake regularly to notice cancel() and the deadline
        if (::select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
        for (auto s : sockets_) {
            if (as_socket(s) == kInvalid || !FD_ISSET(as_socket(s), &set)) continue;
            SOCKET c = ::accept(as_socket(s), nullptr, nullptr);
            if (c == kInvalid) continue;
            const std::string target = read_target(c);
            const auto q = target.find('?');
            const std::string path = target.substr(0, q);
            if (!target.empty() && path == path_ && !cancelled_) {
                const std::string uri = "http://" + (host_.find(':') != std::string::npos ? "[" + host_ + "]" : host_) +
                                        ":" + std::to_string(port_) + target;
                if (!cb_.redirect || cb_.redirect(uri)) {
                    respond(c, 200, "OK", kPage);
                    delivered = uri;
                } else {
                    respond(c, 400, "Bad Request", kRefusedPage);
                }
            } else {
                respond(c, 404, "Not Found", "<!doctype html><title>Not found</title>");
            }
            ::closesocket(c);
            if (!delivered.empty()) break;
        }
    }
    for (auto& s : sockets_) close_socket(s);
    listening_ = false;
    if (cancelled_ || !delivered.empty()) return;
    if (timed_out && cb_.timed_out) cb_.timed_out();
}

}  // namespace wb
