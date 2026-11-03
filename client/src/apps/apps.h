// client/local-apps (add-local-apps): Local apps, small web applications the agent builds for the user alone. Each app
// is a folder with app.json, its server code, its static files and its SQLite database (design D1). The client runs
// an app on demand with the bundled Node.js runtime under the Node.js permission model and client/tool-sandbox, talks
// to it over frames on stdin and stdout (design D2), and serves it only to the user through a loopback listener with a
// single-use launch token and a session cookie (design D3). Pages call read-risk MCP tools the user confirmed through
// the same listener (design D4). The manager stops idle apps and keeps at most three running (design D5).
#pragma once

#include "base/json.h"
#include "base/result.h"
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
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace wb::sandbox {
class Sandbox;
}

namespace wb::apps {

// ---- Manifest and folders (D1) --------------------------------------------------------------------------------------

struct Manifest {
    std::string name, description;
    std::string entry = "server.js";
    std::vector<std::string> mcp_tools;  // mcp:<server>/<tool>
    [[nodiscard]] json to_json() const;
};

// Validates app.json: a name, an entry that is a .js, .mjs or .cjs file inside the folder, MCP tool ids.
Result<Manifest> parse_manifest(const json& j);
// The app id for a name: lowercase ASCII letters, digits and '-', at most 40 characters ("app" when nothing is left).
std::string slug(std::string_view name);
// The hash the user's confirmation of an MCP tool list is stored with (order and duplicates do not matter).
std::string tools_hash(const std::vector<std::string>& tools);
// Why the folder cannot be started because of packages (a package.json with dependencies, or node_modules), if so.
std::optional<std::string> dependency_problem(const std::filesystem::path& folder);

// ---- Frames (D2) ----------------------------------------------------------------------------------------------------

// u32le header length, u32le body length, the JSON header {t, id}, the body (see app-host.js).
struct Frame {
    std::string type;
    long long id = 0;
    std::string body;
};
std::string encode_frame(std::string_view type, long long id, std::string_view body = {});
class FrameReader {
public:
    void feed(std::string_view bytes) { buf_.append(bytes); }
    // The next complete frame; nullopt when more bytes are needed. A malformed frame sets bad().
    std::optional<Frame> next();
    [[nodiscard]] bool bad() const { return bad_; }

private:
    std::string buf_;
    bool bad_ = false;
};

// ---- Manager --------------------------------------------------------------------------------------------------------

struct AppsEnv {
    std::filesystem::path root;         // the apps folder; one subfolder per app
    std::filesystem::path state_dir;    // confirmations, last use and logs (out of the apps' reach)
    std::filesystem::path runtime_dir;  // node.exe and app-host.js (the optional app runtime package)
    sandbox::Sandbox* sandbox = nullptr;
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::function<long long()> now_ms;  // default: wall clock
    // The Risk class of an MCP tool in the catalog, nullopt when the tool is not available.
    std::function<std::optional<runtime::Risk>(const std::string& tool_id)> tool_risk;
    // Calls an MCP tool as the user with origin app:<id> under client/policy-enforcement; the result as JSON.
    std::function<Result<json>(const std::string& app_id, const std::string& tool_id, const json& args)> call_tool;
    // Whether a Run is still queued, running or interrupted (its write access to an app folder lasts that long).
    std::function<bool(const std::string& run_id)> run_active;
    std::function<void()> changed;  // the list or a running state changed
    long long memory_mb = 512;
    size_t max_running = 3;
    std::chrono::milliseconds idle = std::chrono::minutes(15);
    std::chrono::milliseconds start_timeout = std::chrono::seconds(10);
    size_t log_bytes = 1024 * 1024;
};

struct AppInfo {
    std::string id;
    Manifest manifest;
    std::string folder;  // final path
    long long size = 0;       // bytes in the folder
    long long last_used = 0;  // unix ms of the last request or start, 0 when never
    bool running = false;
    bool needs_confirmation = false;  // the MCP tool list is not confirmed (or changed since)
    std::string problem;              // why the folder cannot be started (a broken manifest, packages)
    [[nodiscard]] json to_json() const;
};

// One connection to a running app: request bytes in, response bytes out.
class AppStream {
public:
    Status send(std::string_view bytes);
    void end_input();
    // Waits for response bytes; false at the end of the response or after the timeout (then timed_out()).
    bool read(std::string& out, std::chrono::milliseconds timeout);
    [[nodiscard]] bool timed_out() const { return timed_out_; }

private:
    friend class AppManager;
    std::function<Status(std::string_view type, std::string_view body)> write_;  // frames to the app
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<std::string> chunks_;
    bool ended_ = false, timed_out_ = false;
    void push(std::string bytes);
    void finish();
};

class AppManager {
public:
    explicit AppManager(AppsEnv env);
    ~AppManager();
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;

    // Why apps cannot run now (policy.apps.mode off, the runtime package missing), or nullopt.
    [[nodiscard]] std::optional<std::string> unavailable() const;
    [[nodiscard]] std::string mcp_mode() const;  // policy.apps.mcp: off | read

    std::vector<AppInfo> list();
    std::optional<AppInfo> get(const std::string& id);
    // Creates the folder with app.json, a starter server.js and public/index.html; grants the Run write access.
    Result<AppInfo> create(const Manifest& m, const std::string& run_id);
    // Changes the manifest of an existing app (fields given) and grants the Run write access to its folder.
    Result<AppInfo> update(const std::string& id, const json& changes, const std::string& run_id);
    // App folders a Run may read and write with the files.* tools; "" gives the folders of every active Run.
    std::vector<std::string> folders_for(const std::string& run_id);
    Status confirm(const std::string& id);  // the user confirmed the app's MCP tool list
    Status start(const std::string& id);
    Status stop(const std::string& id);
    Status remove(const std::string& id);  // stops the app and deletes its folder
    // The last `max_bytes` of the app's log.
    std::string logs(const std::string& id, size_t max_bytes = 64 * 1024);
    // A new connection to the app, started when needed; counts as a request for the idle timer.
    Result<std::shared_ptr<AppStream>> connect(const std::string& id);
    // The MCP bridge (D4): the manifest list, the user's confirmation, policy.apps.mcp and the read Risk class.
    Result<json> call_mcp(const std::string& id, const std::string& server, const std::string& tool, const json& args);
    // Stops apps idle for longer than env.idle. Called periodically.
    void tick();
    void shutdown();

    [[nodiscard]] size_t running_count();

private:
    struct Running;
    std::filesystem::path folder_of(const std::string& id) const;
    Result<AppInfo> info_locked(const std::string& id);
    Status start_locked(std::unique_lock<std::mutex>& lock, const std::string& id);
    void stop_locked(const std::string& id, const std::string& why);
    void reap_locked();
    json state_locked();
    void save_state_locked(const json& s);
    void append_log(const std::string& id, std::string_view text);
    long long now() const;
    void notify();

    AppsEnv env_;
    std::mutex mx_;
    std::map<std::string, std::shared_ptr<Running>> running_;
    std::mutex log_mx_;                                         // logs_ (the reader threads append without mx_)
    std::map<std::string, std::string> logs_;                   // app id -> log (the last log_bytes)
    std::map<std::string, std::set<std::string>> grants_;       // run id -> app ids
    long long next_stream_ = 1;
};

// ---- Owner-only serving (D3) ----------------------------------------------------------------------------------------

class AppServer {
public:
    explicit AppServer(AppManager& apps);
    ~AppServer();
    AppServer(const AppServer&) = delete;
    AppServer& operator=(const AppServer&) = delete;

    // Starts the listener when needed and returns http://127.0.0.1:<port>/<id>/?launch=<single-use token>.
    Result<std::string> launch_url(const std::string& app_id);
    [[nodiscard]] int port() const;
    void stop();
    // Ends the sessions of an app (deleted).
    void forget(const std::string& app_id);

    static constexpr int kMaxConnections = 32;

private:
    Status start_locked();
    void accept_loop(std::uintptr_t s);
    void serve(std::uintptr_t c);

    AppManager& apps_;
    mutable std::mutex mx_;
    std::uintptr_t listen_ = ~std::uintptr_t{0};
    int port_ = 0;
    std::thread acceptor_;
    std::map<std::string, std::pair<std::string, long long>> launch_;  // token -> app id, expiry (unix ms)
    std::map<std::string, std::string> sessions_;                       // session -> app id
    int connections_ = 0;
    std::condition_variable idle_;
    std::vector<std::uintptr_t> open_;
};

// ---- Tools ----------------------------------------------------------------------------------------------------------

struct AppToolsEnv {
    AppManager* apps = nullptr;
    AppServer* server = nullptr;
    std::function<void(const std::string& url)> open_url;  // the user's default browser
};
std::vector<runtime::ToolDef> app_tools(AppToolsEnv env);

}  // namespace wb::apps
