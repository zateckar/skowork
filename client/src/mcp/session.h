// MCP transports and sessions of client/mcp-client (add-core-tools-and-mcp design D3, D8): stdio to a sandboxed local
// server, Streamable HTTP to a remote server, and the initialize handshake, tools/list and tools/call on top of either.
#pragma once

#include "mcp/protocol.h"
#include "platform/http.h"
#include "policy/egress.h"
#include "sandbox/sandbox.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wb::mcp {

// Error codes of requests: "closed" (the connection or process ended), "timeout", "cancelled", "rpc" (a JSON-RPC
// error from the server), "protocol" (an unusable reply), "session_expired" (HTTP 404 for the session),
// "unauthorized", "policy_denied", and the HttpClient codes.
class Transport {
public:
    virtual ~Transport() = default;
    // Sends a request and waits for its response, at most `timeout`, polling `cancelled`.
    virtual Result<json> request(const std::string& method, const json& params, std::chrono::milliseconds timeout,
                                 const std::function<bool()>& cancelled) = 0;
    virtual Status notify(const std::string& method, const json& params) = 0;
    [[nodiscard]] virtual bool alive() const = 0;
    // What went wrong last, for diagnostics (the stderr tail of a local server).
    [[nodiscard]] virtual std::string diagnostics() const { return {}; }
    virtual void close() = 0;
    // Remote transports forget their session after "session_expired"; local ones never expire.
    virtual void reset_session() {}
};

// Newline-delimited JSON over the stdin and stdout of a sandboxed process. Answers the server's ping; refuses every
// other server request with -32601 (no sampling, elicitation or roots; design non-goals).
class StdioTransport final : public Transport {
public:
    explicit StdioTransport(std::unique_ptr<sandbox::SandboxedProcess> process);
    ~StdioTransport() override;

    Result<json> request(const std::string& method, const json& params, std::chrono::milliseconds timeout,
                         const std::function<bool()>& cancelled) override;
    Status notify(const std::string& method, const json& params) override;
    [[nodiscard]] bool alive() const override;
    [[nodiscard]] std::string diagnostics() const override;
    void close() override;
    // Whether the process ended by itself (a crash), as opposed to close().
    [[nodiscard]] bool crashed() const;

private:
    void reader();
    Status send(const json& j);

    std::unique_ptr<sandbox::SandboxedProcess> proc_;
    std::thread reader_;
    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::map<long long, Message> responses_;
    long long next_id_ = 1;
    bool eof_ = false;
    bool closing_ = false;
    std::string protocol_problem_;
};

struct HttpTransportOptions {
    std::string url;
    HttpClient* client = nullptr;
    const policy::EgressGuard* guard = nullptr;
    // The bearer token for the server (the user's access token); `force` asks for a fresh one after a 401. Unset: no
    // Authorization header (auth "none").
    std::function<Result<std::string>(bool force)> token;
};

// Streamable HTTP (MCP 2025-06-18): every message is a POST; the reply is JSON or an SSE stream that carries the
// response. The session id from initialize is sent with every later request and the session is deleted on close. No
// redirects are followed and every request passes the egress guard first.
class HttpTransport final : public Transport {
public:
    explicit HttpTransport(HttpTransportOptions o) : o_(std::move(o)) {}
    ~HttpTransport() override { close(); }

    Result<json> request(const std::string& method, const json& params, std::chrono::milliseconds timeout,
                         const std::function<bool()>& cancelled) override;
    Status notify(const std::string& method, const json& params) override;
    [[nodiscard]] bool alive() const override { return !closed_; }
    void close() override;
    void reset_session() override;
    void set_protocol_version(std::string v);

private:
    Result<HttpResponse> post(const json& message, std::chrono::milliseconds timeout, const std::function<bool()>& cancelled,
                              long long expect_id, Message* reply);

    HttpTransportOptions o_;
    std::mutex mx_;
    std::string session_id_, protocol_version_;
    long long next_id_ = 1;
    bool closed_ = false;
};

// A connected MCP session: the handshake, then tools/list and tools/call.
class Session {
public:
    // Runs initialize and notifications/initialized. `protocol_version` comes from the catalog entry.
    static Result<std::unique_ptr<Session>> open(std::unique_ptr<Transport> t, const std::string& protocol_version,
                                                 const std::string& client_version, std::chrono::milliseconds timeout);

    // Every page of tools/list (at most kMaxToolPages).
    Result<std::vector<ToolInfo>> list_tools(std::chrono::milliseconds timeout);
    // tools/call. A remote session that expired is opened again once and the call repeated.
    Result<json> call_tool(const std::string& name, const json& arguments, std::chrono::milliseconds timeout,
                           const std::function<bool()>& cancelled);

    [[nodiscard]] const ServerInfo& info() const { return info_; }
    Transport& transport() { return *t_; }

private:
    Session(std::unique_ptr<Transport> t, std::string pv, std::string cv) : t_(std::move(t)), pv_(std::move(pv)), cv_(std::move(cv)) {}
    Status handshake(std::chrono::milliseconds timeout);

    std::unique_ptr<Transport> t_;
    std::string pv_, cv_;
    ServerInfo info_;
};

}  // namespace wb::mcp
