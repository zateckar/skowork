// client/api-connectors (add-api-connectors design D5, D11): the API connectors of the Policy bundle offered to the agent as
// tools api:<connector>/<operation>, their calls through the control plane, and the user's personal access token calls.
// The client never contacts an API host, and never stores, logs or shows a token: the token is sent once, in the body of
// one request, and the field that held it is cleared (TokenDraft).
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "platform/http.h"
#include "policy/core.h"
#include "runtime/tools.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::connectors {

// One request to the control plane: the method, a path relative to its base URL, and the JSON body ("" for none).
using Request = std::function<Result<HttpResponse>(const std::string& method, const std::string& path, const std::string& body)>;

struct Operation {
    std::string name, description;
    runtime::Risk risk = runtime::Risk::external;
    json input_schema = {{"type", "object"}, {"properties", json::object()}};
};

struct Connector {
    std::string id, name, auth;
    std::string state;  // ready | needs-token | token-rejected | not-applicable (control-plane/api-connectors)
    std::vector<Operation> operations;  // the operations the control plane offers this user
    std::string fingerprint;            // the entry's JSON, for change detection
};

// The entries of kind api in policy.mcp.servers. Entries of other kinds are skipped silently; an invalid api entry or
// operation is skipped with its reason in `problems`.
std::vector<Connector> parse_connectors(const json* servers, std::vector<std::string>* problems = nullptr);

// api:<connector>/<operation>.
std::string tool_id(const std::string& connector, const std::string& operation);

// The first violation of the arguments against an operation's input schema (a missing or unknown parameter, a wrong
// type), or nullopt. Checked before a request is made.
std::optional<std::string> argument_problem(const json& schema, const json& args);

// A control-plane answer to a call as a tool result: the 2xx body, or the error envelope's code, message and upstream
// text. Failures are returned as they are; no call is retried.
runtime::ToolOutput call_output(int status, const std::string& body);

// The connector listing of the caller (GET api/v1/connectors) and the personal access token (PUT and DELETE
// api/v1/connectors/<id>/token). The answers never carry a token; errors carry the control plane's code and message.
Result<json> list_connectors(const Request& request);
Result<json> set_token(const Request& request, const std::string& id, const std::string& token);
Result<json> remove_token(const Request& request, const std::string& id);

struct ManagerEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    Request request;
    std::function<Status(runtime::ToolDef)> add_tool;
    std::function<bool(const std::string& id)> remove_tool;
    // Called (on any thread) whenever the status changes.
    std::function<void()> on_status;
};

// Keeps the API connectors of the policy's mcp.servers as tools: added, changed and withdrawn with each policy change.
class ConnectorManager {
public:
    explicit ConnectorManager(ManagerEnv env);
    ~ConnectorManager();
    ConnectorManager(const ConnectorManager&) = delete;
    ConnectorManager& operator=(const ConnectorManager&) = delete;

    void apply();
    void shutdown();
    // [{id, name, kind: "api", auth, state, operations, usable_tools, reason, warning}]. reason is one of needs-token,
    // token-rejected, none-selected, denied-by-policy, or null.
    [[nodiscard]] json status() const;

private:
    struct Entry {
        Connector connector;
        std::vector<std::string> registered;  // tool ids in the agent runtime
        size_t usable = 0;
        std::string reason;
    };
    void changed();

    ManagerEnv env_;
    std::shared_ptr<const Request> request_;  // shared with the tools, so that a call outlives the manager safely
    std::mutex apply_mx_;                      // serializes apply and shutdown
    mutable std::mutex mx_;                    // guards entries_
    std::map<std::string, Entry> entries_;
};

}  // namespace wb::connectors
