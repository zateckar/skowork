// McpManager (client/mcp-client; add-core-tools-and-mcp design D8): the catalog servers of the effective policy, their
// tools in the agent runtime as mcp:<server>/<tool>, pinned local packages, lazy start, idle stop, the crash limit,
// health for Connections and the status bar, and live catalog updates.
#pragma once

#include "mcp/session.h"
#include "policy/core.h"
#include "runtime/tools.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wb::mcp {

struct McpEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    sandbox::Sandbox* sandbox = nullptr;
    HttpClient* http = nullptr;
    const policy::EgressGuard* guard = nullptr;
    std::filesystem::path data_dir;  // <data root>\mcp
    std::string client_version;
    // The package bytes for a SHA-256 from the control plane (GET /api/v1/mcp/packages/{sha256}).
    std::function<Result<std::string>(const std::string& sha256)> fetch_package;
    // The user's identity-provider access token (client/mcp-client Remote Server User Token); orce: a fresh one,
    // after a server answered 401.
    std::function<Result<std::string>(bool force)> user_token;
    std::function<void(const std::string& kind, const json& detail)> security_event;
    // The agent runtime's registry.
    std::function<Status(runtime::ToolDef)> add_tool;
    std::function<bool(const std::string& id)> remove_tool;
    // Called (on any thread) whenever the status of a server changes.
    std::function<void()> on_status;
    // Timeouts; tests shorten them.
    std::chrono::milliseconds start_timeout{30000};
    std::chrono::milliseconds call_timeout{std::chrono::minutes(5)};
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
};

class McpManager {
public:
    explicit McpManager(McpEnv env);
    ~McpManager();
    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    // Reads policy.mcp.servers and applies the difference (Catalog Updates Applied Live): removed servers are stopped
    // and their tools withdrawn before this returns; new or changed servers get their tools from the cached tool list
    // or a background discovery.
    void apply();
    // Stops servers idle for longer than policy.mcp.idle_timeout_minutes. Called periodically.
    void tick();
    // Stops everything and withdraws the tools.
    void shutdown();

    // [{id, name, kind, state, tools, usable_tools, warning, error}] (Health And Diagnostics). state is connected,
    // idle, starting, failed or unavailable.
    [[nodiscard]] json status() const;
    // Catalog-Only Servers: nullopt when the server is in the catalog and usable, else why not (for a skill's dependency).
    [[nodiscard]] std::optional<std::string> server_unavailable(const std::string& server_id) const;
    // For the policy enforcer: why an mcp: tool cannot be called now, or nullopt.
    [[nodiscard]] std::optional<std::string> tool_unavailable(const std::string& tool_id) const;
    // Waits until no background discovery is pending (tests).
    bool wait_idle(std::chrono::milliseconds timeout);

private:
    struct Server;
    using ServerPtr = std::shared_ptr<Server>;

    runtime::ToolOutput call(const ServerPtr& s, const std::string& tool, runtime::ToolContext& ctx, const json& args);
    // Starts (or reuses) the server's session; caller holds s->call_mx.
    Status ensure_session(const ServerPtr& s);
    Status discover(const ServerPtr& s);
    void register_tools(const ServerPtr& s, const std::vector<ToolInfo>& tools);
    void withdraw_tools(const ServerPtr& s);
    void stop_session(const ServerPtr& s);
    void crashed(const ServerPtr& s, const std::string& why);
    Result<std::unique_ptr<Transport>> open_local(const ServerPtr& s);
    Result<std::unique_ptr<Transport>> open_remote(const ServerPtr& s);
    // The package bytes, checked against the catalog hash (fetched again once when the stored copy does not match).
    Result<std::string> verified_package(const ServerEntry& e);
    void post(std::function<void()> fn);
    void worker();
    void changed();

    McpEnv env_;
    mutable std::mutex mx_;
    std::map<std::string, ServerPtr> servers_;

    std::mutex work_mx_;
    std::condition_variable work_cv_;
    std::deque<std::function<void()>> work_;
    int busy_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace wb::mcp
