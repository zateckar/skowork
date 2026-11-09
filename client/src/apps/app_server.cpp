// The loopback listener of client/local-apps (add-local-apps design D3, D4): 127.0.0.1 on a random port, /<app-id>/...
// mapped to the app's framed connection. A single-use launch token becomes an HttpOnly, SameSite=Strict session cookie
// scoped to the app's path; requests without it, and requests naming another host, are refused. Pages reach the MCP
// bridge at POST /<app-id>/_workbench/mcp/<server>/<tool>.
#include "apps/apps.h"

#include "base/strings.h"
#include "base/time.h"
#include "platform/crypto.h"
#include "platform/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace wb::apps {

namespace {

constexpr const char* kLog = "apps";
constexpr size_t kMaxHead = 32 * 1024;
constexpr size_t kMaxBody = 16 * 1024 * 1024;
constexpr size_t kMaxBridgeBody = 1024 * 1024;
constexpr DWORD kHeadTimeoutMs = 30000;
constexpr long long kLaunchTtlMs = 2 * 60 * 1000;
constexpr auto kResponseIdle = std::chrono::minutes(5);

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

bool send_all(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int k = ::send(s, data.data(), static_cast<int>(std::min<size_t>(data.size(), 1 << 20)), 0);
        if (k <= 0) return false;
        data.remove_prefix(static_cast<size_t>(k));
    }
    return true;
}

void reply(SOCKET s, int code, const char* text, const std::string& body, const std::string& extra = {}, const char* type = "text/plain; charset=utf-8") {
    const std::string msg = "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\n" + extra + "Content-Type: " + type +
                            "\r\nContent-Length: " + std::to_string(body.size()) +
                            "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n" + body;
    send_all(s, msg);
}

void reply_json(SOCKET s, int code, const char* text, const json& body) { reply(s, code, text, body.dump(), {}, "application/json"); }

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

bool read_body(SOCKET s, std::string& body, size_t length) {
    char tmp[64 * 1024];
    while (body.size() < length) {
        const int n = ::recv(s, tmp, static_cast<int>(std::min(sizeof tmp, length - body.size())), 0);
        if (n <= 0) return false;
        body.append(tmp, static_cast<size_t>(n));
    }
    body.resize(length);
    return true;
}

struct Request {
    std::string method, target, version;
    std::vector<std::pair<std::string, std::string>> headers;
    [[nodiscard]] std::string header(std::string_view name) const {
        for (const auto& [k, v] : headers)
            if (iequals(k, name)) return v;
        return {};
    }
    [[nodiscard]] bool has(std::string_view name) const {
        for (const auto& [k, _] : headers)
            if (iequals(k, name)) return true;
        return false;
    }
};

std::optional<Request> parse_head(const std::string& head) {
    Request r;
    size_t at = 0;
    bool first = true;
    while (at < head.size()) {
        const size_t e = head.find("\r\n", at);
        if (e == std::string::npos) break;
        const std::string line = head.substr(at, e - at);
        at = e + 2;
        if (first) {
            const auto parts = split(line, ' ');
            if (parts.size() != 3 || !starts_with(parts[2], "HTTP/1.")) return std::nullopt;
            r.method = parts[0];
            r.target = parts[1];
            r.version = parts[2];
            first = false;
            continue;
        }
        const size_t c = line.find(':');
        if (c == std::string::npos || c == 0) return std::nullopt;
        r.headers.emplace_back(trim(line.substr(0, c)), trim(line.substr(c + 1)));
    }
    if (first || r.target.empty() || r.target[0] != '/') return std::nullopt;
    return r;
}

// The value of a cookie in a Cookie header.
std::string cookie_value(const std::string& header, const std::string& name) {
    for (const auto& part : split(header, ';')) {
        const std::string p = trim(part);
        const size_t eq = p.find('=');
        if (eq != std::string::npos && p.substr(0, eq) == name) return p.substr(eq + 1);
    }
    return {};
}

// The Cookie header without the client's own session cookies (the app never sees them).
std::string strip_session_cookies(const std::string& header) {
    std::string out;
    for (const auto& part : split(header, ';')) {
        const std::string p = trim(part);
        if (p.empty() || starts_with(p, "workbench_app_")) continue;
        out += (out.empty() ? "" : "; ") + p;
    }
    return out;
}

std::string query_param(std::string_view query, std::string_view name) {
    for (const auto& kv : split(query, '&')) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == name) return kv.substr(eq + 1);
    }
    return {};
}

std::string page(const char* title, const char* text) {
    return std::string("<!doctype html><meta charset=\"utf-8\"><title>") + title + "</title><p>" + text + "</p>";
}

}  // namespace

AppServer::AppServer(AppManager& apps) : apps_(apps) {}

AppServer::~AppServer() { stop(); }

int AppServer::port() const {
    const std::lock_guard lock(mx_);
    return port_;
}

Status AppServer::start_locked() {
    if (as_socket(listen_) != INVALID_SOCKET) return {};
    if (!wsa_ready()) return make_error("apps", "Winsock is unavailable");
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return make_error("apps", "cannot create the app listener socket");
    BOOL excl = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 64) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        ::closesocket(s);
        return make_error("apps", "cannot listen on loopback for Local apps");
    }
    listen_ = static_cast<std::uintptr_t>(s);
    port_ = ntohs(a.sin_port);
    acceptor_ = std::thread([this, l = listen_] { accept_loop(l); });
    log::info(kLog, "listening on 127.0.0.1:" + std::to_string(port_));
    return {};
}

Result<std::string> AppServer::launch_url(const std::string& app_id) {
    const std::lock_guard lock(mx_);
    if (Status s = start_locked(); !s) return s.error();
    const long long now = unix_millis_now();
    for (auto it = launch_.begin(); it != launch_.end();) it = it->second.second < now ? launch_.erase(it) : std::next(it);
    const std::string token = random_urlsafe(24);
    launch_[token] = {app_id, now + kLaunchTtlMs};
    return "http://127.0.0.1:" + std::to_string(port_) + "/" + app_id + "/?launch=" + token;
}

void AppServer::forget(const std::string& app_id) {
    const std::lock_guard lock(mx_);
    for (auto it = sessions_.begin(); it != sessions_.end();) it = it->second == app_id ? sessions_.erase(it) : std::next(it);
    for (auto it = launch_.begin(); it != launch_.end();) it = it->second.first == app_id ? launch_.erase(it) : std::next(it);
}

void AppServer::stop() {
    std::uintptr_t l;
    std::thread acceptor;
    {
        const std::lock_guard lock(mx_);
        l = listen_;
        listen_ = ~std::uintptr_t{0};
        port_ = 0;
        acceptor = std::move(acceptor_);
        for (auto s : open_) ::shutdown(as_socket(s), SD_BOTH);
    }
    if (as_socket(l) != INVALID_SOCKET) ::closesocket(as_socket(l));
    if (acceptor.joinable()) acceptor.join();
    std::unique_lock lock(mx_);
    idle_.wait(lock, [this] { return connections_ == 0; });
}

void AppServer::accept_loop(std::uintptr_t l) {
    for (;;) {
        SOCKET c = ::accept(as_socket(l), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        {
            const std::lock_guard lock(mx_);
            if (connections_ >= kMaxConnections) {
                reply(c, 503, "Service Unavailable", "busy\n");
                ::closesocket(c);
                continue;
            }
            ++connections_;
            open_.push_back(static_cast<std::uintptr_t>(c));
        }
        std::thread([this, c] { serve(static_cast<std::uintptr_t>(c)); }).detach();
    }
}

void AppServer::serve(std::uintptr_t raw) {
    const SOCKET c = as_socket(raw);
    const auto finish = [&] {
        {
            const std::lock_guard lock(mx_);
            std::erase(open_, raw);
            ::closesocket(c);
            --connections_;
        }
        idle_.notify_all();
    };
    DWORD timeout = kHeadTimeoutMs;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    std::string head, rest;
    if (!read_head(c, head, rest)) {
        reply(c, 400, "Bad Request", "bad request\n");
        return finish();
    }
    const auto req = parse_head(head);
    if (!req) {
        reply(c, 400, "Bad Request", "bad request\n");
        return finish();
    }
    int port = 0;
    {
        const std::lock_guard lock(mx_);
        port = port_;
    }
    // Only the loopback names of this listener (DNS rebinding).
    const std::string host = to_lower(req->header("Host"));
    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    if (host != "127.0.0.1:" + std::to_string(port) && host != "localhost:" + std::to_string(port)) {
        reply(c, 403, "Forbidden", "This address only serves the user's Local apps.\n");
        return finish();
    }
    // /<app-id>/<rest>[?query]
    const size_t q = req->target.find('?');
    const std::string path = req->target.substr(0, q);
    const std::string query = q == std::string::npos ? std::string{} : req->target.substr(q + 1);
    const size_t slash = path.find('/', 1);
    const std::string app_id = path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
    if (app_id.empty() || slug(app_id) != app_id) {
        reply(c, 404, "Not Found", "not found\n");
        return finish();
    }
    if (slash == std::string::npos) {
        reply(c, 308, "Permanent Redirect", "", "Location: /" + app_id + "/" + (query.empty() ? "" : "?" + query) + "\r\n");
        return finish();
    }
    const std::string cookie_name = "workbench_app_" + app_id;
    const std::string launch = query_param(query, "launch");
    if (!launch.empty()) {
        std::string session;
        {
            const std::lock_guard lock(mx_);
            const auto it = launch_.find(launch);
            if (it != launch_.end()) {
                const bool valid = it->second.first == app_id && it->second.second >= unix_millis_now();
                launch_.erase(it);  // single use, whatever the outcome
                if (valid) {
                    session = random_urlsafe(32);
                    sessions_[session] = app_id;
                }
            }
        }
        if (session.empty()) {
            reply(c, 403, "Forbidden", page("Link expired", "This link has been used or has expired. Open the app again from SkoWork."), {},
                  "text/html; charset=utf-8");
            return finish();
        }
        reply(c, 303, "See Other", "",
              "Location: /" + app_id + "/\r\nSet-Cookie: " + cookie_name + "=" + session + "; Path=/" + app_id + "/; HttpOnly; SameSite=Strict\r\n");
        return finish();
    }
    bool allowed = false;
    if (const std::string session = cookie_value(req->header("Cookie"), cookie_name); !session.empty()) {
        const std::lock_guard lock(mx_);
        const auto it = sessions_.find(session);
        allowed = it != sessions_.end() && constant_time_equals(it->second, app_id);
    }
    if (!allowed) {
        reply(c, 403, "Forbidden", page("Not allowed", "Open this app from SkoWork."), {}, "text/html; charset=utf-8");
        return finish();
    }
    if (req->has("Transfer-Encoding") || req->has("Upgrade")) {
        reply(c, 411, "Length Required", "requests need a Content-Length and cannot upgrade\n");
        return finish();
    }
    const std::string cl = req->header("Content-Length");
    const unsigned long long length = cl.empty() ? 0ull : std::strtoull(cl.c_str(), nullptr, 10);
    const std::string rel = path.substr(slash);  // "/..."

    // The MCP bridge (D4).
    if (starts_with(rel, "/_workbench/")) {
        const std::string o = req->header("Origin");
        if (req->method != "POST" || !starts_with(rel, "/_workbench/mcp/") || (!o.empty() && o != origin) ||
            !starts_with(to_lower(req->header("Content-Type")), "application/json")) {
            reply_json(c, 403, "Forbidden", {{"error", {{"code", "forbidden"}, {"message", "POST JSON from the app's own pages only"}}}});
            return finish();
        }
        const auto parts = split(rel.substr(sizeof("/_workbench/mcp/") - 1), '/');
        if (parts.size() != 2 || parts[0].empty() || parts[1].empty() || length > kMaxBridgeBody) {
            reply_json(c, 400, "Bad Request", {{"error", {{"code", "bad_request"}, {"message", "use /_workbench/mcp/<server>/<tool>"}}}});
            return finish();
        }
        std::string body = rest;
        if (!read_body(c, body, length)) return finish();
        auto args = body.empty() ? std::optional<json>(json::object()) : parse_json(body);
        if (!args || !args->is_object()) {
            reply_json(c, 400, "Bad Request", {{"error", {{"code", "bad_request"}, {"message", "the body must be a JSON object of arguments"}}}});
            return finish();
        }
        auto r = apps_.call_mcp(app_id, parts[0], parts[1], *args);
        if (!r) {
            const std::string& code = r.error().code;
            const int status = code == "tool_error" ? 502 : code == "unavailable" ? 503 : code == "not_found" ? 404 : 403;
            reply_json(c, status, status == 502 ? "Bad Gateway" : status == 503 ? "Service Unavailable" : status == 404 ? "Not Found" : "Forbidden",
                       {{"error", {{"code", code}, {"message", r.error().message}}}});
        } else {
            reply_json(c, 200, "OK", {{"result", *r}});
        }
        return finish();
    }

    // Everything else goes to the app: one request per connection, the /<app-id> prefix removed.
    if (length > kMaxBody) {
        reply(c, 413, "Payload Too Large", "too large\n");
        return finish();
    }
    std::string body = rest;
    if (!read_body(c, body, static_cast<size_t>(length))) return finish();
    auto stream = apps_.connect(app_id);
    if (!stream) {
        log::info(kLog, app_id + ": " + stream.error().message);
        reply(c, 503, "Service Unavailable",
              page("The app is not running", "SkoWork could not start this app. See the app's log in SkoWork's Apps view."), {},
              "text/html; charset=utf-8");
        return finish();
    }
    std::string out = req->method + " " + rel + (query.empty() ? "" : "?" + query) + " " + req->version + "\r\n";
    for (const auto& [k, v] : req->headers) {
        if (iequals(k, "Connection") || iequals(k, "Keep-Alive") || iequals(k, "Proxy-Connection") || iequals(k, "Proxy-Authorization")) continue;
        if (iequals(k, "Cookie")) {
            const std::string kept = strip_session_cookies(v);
            if (!kept.empty()) out += "Cookie: " + kept + "\r\n";
            continue;
        }
        out += k + ": " + v + "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    out += body;
    if (!(*stream)->send(out)) {
        reply(c, 502, "Bad Gateway", "the app stopped\n");
        return finish();
    }
    // No end of input here: Node.js's HTTP server would abort the request at once. The request carries its length and
    // Connection: close, so the app ends the connection after its response.
    std::string chunk;
    bool any = false, browser_left = false;
    while ((*stream)->read(chunk, kResponseIdle)) {
        any = true;
        if (!send_all(c, chunk)) {
            browser_left = true;
            break;
        }
    }
    if (browser_left || (*stream)->timed_out()) (*stream)->end_input();  // the app drops the connection
    if (!any) reply(c, 502, "Bad Gateway", "the app sent no response\n");
    finish();
}

}  // namespace wb::apps
