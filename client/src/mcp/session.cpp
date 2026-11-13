#include "mcp/session.h"

#include "base/sse.h"
#include "platform/log.h"

#include <algorithm>

namespace wb::mcp {

namespace {

constexpr const char* kLog = "mcp";

Error rpc_error(const RpcError& e) { return make_error("rpc", "the MCP server returned error " + std::to_string(e.code) + ": " + e.message); }

long long id_number(const json& id) {
    if (id.is_number_integer() || id.is_number_unsigned()) return id.get<long long>();
    return -1;
}

// Calls `fn` (which cancels a request) when `cancelled` turns true while the watcher lives.
class CancelWatcher {
public:
    CancelWatcher(const std::function<bool()>& cancelled, std::function<void()> fn) {
        if (!cancelled) return;
        t_ = std::thread([this, cancelled, fn = std::move(fn)] {
            std::unique_lock lock(mx_);
            while (!done_) {
                if (cancelled()) {
                    fn();
                    return;
                }
                cv_.wait_for(lock, std::chrono::milliseconds(100));
            }
        });
    }
    ~CancelWatcher() {
        {
            const std::lock_guard lock(mx_);
            done_ = true;
        }
        cv_.notify_all();
        if (t_.joinable()) t_.join();
    }

private:
    std::mutex mx_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread t_;
};

}  // namespace

// ---- stdio ----------------------------------------------------------------------------------------------------------

StdioTransport::StdioTransport(std::unique_ptr<sandbox::SandboxedProcess> process) : proc_(std::move(process)) {
    reader_ = std::thread([this] { reader(); });
}

StdioTransport::~StdioTransport() { close(); }

void StdioTransport::reader() {
    LineFramer framer;
    std::vector<std::string> lines;
    for (;;) {
        const std::string chunk = proc_->read_some();
        if (chunk.empty()) break;
        lines.clear();
        if (!framer.feed(chunk, lines)) {
            {
                const std::lock_guard lock(mx_);
                protocol_problem_ = "the server sent a message larger than " + std::to_string(kMaxMessageBytes / (1024 * 1024)) + " MB";
            }
            log::warn(kLog, "stdio server sent an oversized message; stopping it");
            proc_->terminate();
            break;
        }
        for (const auto& line : lines) {
            Message m = parse_message(line);
            switch (m.kind) {
                case Message::Kind::response: {
                    const long long id = id_number(m.id);
                    if (id < 0) break;
                    {
                        const std::lock_guard lock(mx_);
                        if (id < next_id_) responses_[id] = std::move(m);
                    }
                    cv_.notify_all();
                    break;
                }
                case Message::Kind::request:
                    if (m.method == "ping") (void)send(make_result(m.id, json::object()));
                    else (void)send(make_error_response(m.id, -32601, "method not supported by Skowork"));
                    break;
                case Message::Kind::notification: break;  // logging and list_changed are not used
                case Message::Kind::invalid: {
                    const std::lock_guard lock(mx_);
                    protocol_problem_ = "the server wrote something that is not JSON-RPC to stdout";
                    break;
                }
            }
        }
    }
    {
        const std::lock_guard lock(mx_);
        eof_ = true;
    }
    cv_.notify_all();
}

Status StdioTransport::send(const json& j) {
    return proc_->write(j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
}

Result<json> StdioTransport::request(const std::string& method, const json& params, std::chrono::milliseconds timeout,
                                     const std::function<bool()>& cancelled) {
    long long id = 0;
    {
        const std::lock_guard lock(mx_);
        if (eof_ || closing_) return make_error("closed", "the MCP server is not running");
        id = next_id_++;
    }
    if (Status s = send(make_request(id, method, params)); !s) return make_error("closed", "the MCP server is not running");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mx_);
    for (;;) {
        if (const auto it = responses_.find(id); it != responses_.end()) {
            Message m = std::move(it->second);
            responses_.erase(it);
            if (m.error) return rpc_error(*m.error);
            return std::move(m.result);
        }
        if (eof_) return make_error("closed", "the MCP server ended");
        const bool is_cancelled = cancelled && cancelled();
        if (is_cancelled || std::chrono::steady_clock::now() >= deadline) {
            lock.unlock();
            (void)send(make_notification("notifications/cancelled",
                                         {{"requestId", id}, {"reason", is_cancelled ? "cancelled by the user" : "timed out"}}));
            if (is_cancelled) return make_error("cancelled", "the call was cancelled");
            return make_error("timeout", "the MCP server did not answer " + method + " in time");
        }
        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
}

Status StdioTransport::notify(const std::string& method, const json& params) { return send(make_notification(method, params)); }

bool StdioTransport::alive() const {
    const std::lock_guard lock(mx_);
    return !eof_ && !closing_;
}

bool StdioTransport::crashed() const {
    const std::lock_guard lock(mx_);
    return eof_ && !closing_;
}

std::string StdioTransport::diagnostics() const {
    std::string d;
    bool ended = false;
    {
        const std::lock_guard lock(mx_);
        d = protocol_problem_;
        ended = eof_;
    }
    if (ended) proc_->wait_ended(std::chrono::milliseconds(2000));  // the whole stderr tail of a crash
    if (const auto l = proc_->limit(); l != sandbox::LimitHit::none) d += std::string(d.empty() ? "" : "; ") + "stopped at the " + sandbox::to_string(l) + " limit";
    const int code = proc_->exit_code();
    if (code >= 0) d += std::string(d.empty() ? "" : "; ") + "exit code " + std::to_string(code);
    std::string tail = proc_->stderr_tail();
    if (tail.size() > 1000) tail = tail.substr(tail.size() - 1000);
    if (!tail.empty()) d += std::string(d.empty() ? "" : "; ") + "stderr: " + tail;
    return d;
}

void StdioTransport::close() {
    {
        const std::lock_guard lock(mx_);
        closing_ = true;
    }
    proc_->terminate();
    if (reader_.joinable()) reader_.join();
    cv_.notify_all();
}

// ---- Streamable HTTP ------------------------------------------------------------------------------------------------

void HttpTransport::set_protocol_version(std::string v) {
    const std::lock_guard lock(mx_);
    protocol_version_ = std::move(v);
}

void HttpTransport::reset_session() {
    const std::lock_guard lock(mx_);
    session_id_.clear();
}

Result<HttpResponse> HttpTransport::post(const json& message, std::chrono::milliseconds timeout, const std::function<bool()>& cancelled,
                                         long long expect_id, Message* reply) {
    if (!o_.client) return make_error("closed", "no HTTP client");
    if (o_.guard) {
        const policy::RuleVerdict v = o_.guard->check_url(o_.url);
        if (!v.allowed) return make_error("policy_denied", "the MCP server's address is blocked by the network policy (" + v.rule + ")");
    }
    std::string session, version;
    {
        const std::lock_guard lock(mx_);
        if (closed_) return make_error("closed", "the MCP connection is closed");
        session = session_id_;
        version = protocol_version_;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        HttpRequest req;
        req.method = "POST";
        req.url = o_.url;
        req.body = message.dump(-1, ' ', false, json::error_handler_t::replace);
        req.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
        if (!session.empty()) req.headers.emplace_back("Mcp-Session-Id", session);
        if (!version.empty()) req.headers.emplace_back("MCP-Protocol-Version", version);
        if (o_.token) {
            auto t = o_.token(attempt > 0);
            if (!t) return t.error();
            req.headers.emplace_back("Authorization", "Bearer " + *t);
        }
        req.follow_redirects = false;
        req.connect_timeout = std::min<std::chrono::milliseconds>(timeout, std::chrono::milliseconds(10000));
        req.idle_timeout = timeout;
        req.max_body = kMaxMessageBytes;
        req.cancel = std::make_shared<CancelToken>();
        Result<HttpResponse> r = [&] {
            const CancelWatcher watch(cancelled, [c = req.cancel] { c->cancel(); });
            return o_.client->send(req);
        }();
        if (!r) {
            if (r.error().code == "cancelled") return make_error("cancelled", "the call was cancelled");
            return r;
        }
        if (r->status == 401 && o_.token && attempt == 0) continue;
        if (r->status == 401 || r->status == 403) return make_error("unauthorized", "the MCP server refused the token (HTTP " + std::to_string(r->status) + ")");
        if (r->status == 404 && !session.empty()) {
            reset_session();
            return make_error("session_expired", "the MCP session expired");
        }
        if (r->status >= 300 && r->status < 400) return make_error("protocol", "the MCP server answered with a redirect, which is not followed");
        if (r->status < 200 || r->status >= 300) return make_error("http", "the MCP server answered HTTP " + std::to_string(r->status));
        if (const std::string sid = r->header("mcp-session-id"); !sid.empty()) {
            const std::lock_guard lock(mx_);
            session_id_ = sid.substr(0, 512);
        }
        if (!reply) return r;
        const std::string type = r->header("content-type");
        if (type.find("text/event-stream") != std::string::npos) {
            SseParser parser(kMaxMessageBytes);
            std::vector<SseEvent> events;
            parser.feed(r->body, events);
            parser.feed("\n\n", events);
            for (const auto& e : events) {
                Message m = parse_message(e.data);
                if (m.kind == Message::Kind::response && id_number(m.id) == expect_id) {
                    *reply = std::move(m);
                    return r;
                }
            }
            return make_error("protocol", "the MCP server's event stream ended without the response");
        }
        Message m = parse_message(r->body);
        if (m.kind != Message::Kind::response || id_number(m.id) != expect_id)
            return make_error("protocol", "the MCP server's reply is not the JSON-RPC response");
        *reply = std::move(m);
        return r;
    }
    return make_error("unauthorized", "the MCP server refused the token");
}

Result<json> HttpTransport::request(const std::string& method, const json& params, std::chrono::milliseconds timeout,
                                    const std::function<bool()>& cancelled) {
    long long id = 0;
    {
        const std::lock_guard lock(mx_);
        id = next_id_++;
    }
    Message reply;
    auto r = post(make_request(id, method, params), timeout, cancelled, id, &reply);
    if (!r) return r.error();
    if (reply.error) return rpc_error(*reply.error);
    return std::move(reply.result);
}

Status HttpTransport::notify(const std::string& method, const json& params) {
    auto r = post(make_notification(method, params), std::chrono::milliseconds(15000), {}, -1, nullptr);
    if (!r) return r.error();
    return {};
}

void HttpTransport::close() {
    std::string session;
    {
        const std::lock_guard lock(mx_);
        if (closed_) return;
        closed_ = true;
        session = std::move(session_id_);
    }
    if (session.empty() || !o_.client) return;
    if (o_.guard && !o_.guard->check_url(o_.url).allowed) return;
    HttpRequest req;
    req.method = "DELETE";
    req.url = o_.url;
    req.headers = {{"Mcp-Session-Id", session}};
    if (o_.token)
        if (auto t = o_.token(false)) req.headers.emplace_back("Authorization", "Bearer " + *t);
    req.follow_redirects = false;
    req.connect_timeout = std::chrono::milliseconds(3000);
    req.idle_timeout = std::chrono::milliseconds(3000);
    (void)o_.client->send(req);
}

// ---- Session --------------------------------------------------------------------------------------------------------

Result<std::unique_ptr<Session>> Session::open(std::unique_ptr<Transport> t, const std::string& protocol_version,
                                               const std::string& client_version, std::chrono::milliseconds timeout) {
    std::unique_ptr<Session> s(new Session(std::move(t), protocol_version, client_version));
    if (Status st = s->handshake(timeout); !st) return st.error();
    return s;
}

Status Session::handshake(std::chrono::milliseconds timeout) {
    auto r = t_->request("initialize", initialize_params(pv_, cv_), timeout, {});
    if (!r) return r.error();
    auto info = parse_initialize(*r);
    if (!info) return info.error();
    info_ = std::move(*info);
    if (auto* http = dynamic_cast<HttpTransport*>(t_.get())) http->set_protocol_version(info_.protocol_version);
    return t_->notify("notifications/initialized", nullptr);
}

Result<std::vector<ToolInfo>> Session::list_tools(std::chrono::milliseconds timeout) {
    std::vector<ToolInfo> all;
    std::string cursor;
    for (int page = 0; page < kMaxToolPages; ++page) {
        auto r = t_->request("tools/list", cursor.empty() ? json::object() : json{{"cursor", cursor}}, timeout, {});
        if (!r) return r.error();
        auto tools = parse_tools_page(*r, cursor);
        if (!tools) return tools.error();
        for (auto& t : *tools)
            if (std::none_of(all.begin(), all.end(), [&](const ToolInfo& o) { return o.name == t.name; })) all.push_back(std::move(t));
        if (cursor.empty()) return all;
    }
    log::warn(kLog, "tools/list: stopped after " + std::to_string(kMaxToolPages) + " pages");
    return all;
}

Result<json> Session::call_tool(const std::string& name, const json& arguments, std::chrono::milliseconds timeout,
                                const std::function<bool()>& cancelled) {
    const json params = {{"name", name}, {"arguments", arguments.is_object() ? arguments : json::object()}};
    auto r = t_->request("tools/call", params, timeout, cancelled);
    if (!r && r.error().code == "session_expired") {
        if (Status s = handshake(timeout); !s) return s.error();
        r = t_->request("tools/call", params, timeout, cancelled);
    }
    return r;
}

}  // namespace wb::mcp
