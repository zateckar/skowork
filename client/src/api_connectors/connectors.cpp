#include "api_connectors/connectors.h"

#include "base/json_schema.h"
#include "base/time.h"
#include "platform/log.h"
#include "policy/decide.h"

#include <algorithm>
#include <set>

namespace wb::connectors {

namespace {

constexpr const char* kLog = "connectors";

// A connector id: 1-64 characters of [A-Za-z0-9_.-], and not "." or "..", so that it is one path segment.
bool safe_id(const std::string& s) {
    if (s.empty() || s.size() > 64 || s == "." || s == "..") return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

// An operation name: the control plane's tool name, 1-64 characters of [A-Za-z0-9_-].
bool safe_operation(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// The answer of a control-plane request as JSON, or the error of its envelope (or of the status when there is none).
Result<json> json_answer(const Result<HttpResponse>& r) {
    if (!r) return r.error();
    const auto j = parse_json(r->body);
    if (r->status >= 200 && r->status < 300) {
        if (!j) return make_error("http", "the control plane sent no JSON (HTTP " + std::to_string(r->status) + ")");
        return *j;
    }
    std::string code = "http_" + std::to_string(r->status), message = "HTTP " + std::to_string(r->status);
    if (j)
        if (const json* e = jobj(*j, "error")) {
            if (!jstr(*e, "code").empty()) code = jstr(*e, "code");
            if (!jstr(*e, "message").empty()) message = jstr(*e, "message");
        }
    return make_error(code, message);
}

}  // namespace

std::vector<Connector> parse_connectors(const json* servers, std::vector<std::string>* problems) {
    std::vector<Connector> out;
    if (!servers || !servers->is_array()) return out;
    const auto problem = [&](const std::string& what) {
        if (problems) problems->push_back(what);
    };
    for (const auto& s : *servers) {
        if (!s.is_object() || jstr(s, "kind") != "api") continue;  // mcp entries are the MCP manager's
        Connector c;
        c.id = jstr(s, "id");
        if (!safe_id(c.id)) {
            problem("api connector: invalid id");
            continue;
        }
        if (std::any_of(out.begin(), out.end(), [&](const Connector& o) { return o.id == c.id; })) {
            problem(c.id + ": duplicate connector id");
            continue;
        }
        c.name = jstr(s, "name", c.id);
        c.auth = jstr(s, "auth", "none");
        c.state = jstr(s, "state", "not-applicable");
        c.fingerprint = s.dump();
        if (const json* ops = jobj(s, "operations"); ops && ops->is_array()) {
            std::set<std::string> seen;
            for (const auto& o : *ops) {
                Operation op;
                op.name = jstr(o, "name");
                if (!safe_operation(op.name) || !seen.insert(op.name).second) {
                    problem(c.id + ": operation name missing, invalid or repeated");
                    continue;
                }
                op.description = jstr(o, "description");
                op.risk = runtime::parse_risk(jstr(o, "risk")).value_or(runtime::Risk::external);
                if (const json* schema = jobj(o, "input_schema"); schema && schema->is_object()) op.input_schema = *schema;
                c.operations.push_back(std::move(op));
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::string tool_id(const std::string& connector, const std::string& operation) { return "api:" + connector + "/" + operation; }

std::optional<std::string> argument_problem(const json& schema, const json& args) {
    const auto errors = validate_json(schema, args, 1);
    if (errors.empty()) return std::nullopt;
    return "the arguments do not match the operation: " + errors.front();
}

runtime::ToolOutput call_output(int status, const std::string& body) {
    const auto j = parse_json(body);
    if (status >= 200 && status < 300) {
        if (!j || !j->is_object() || !j->contains("status") || !j->contains("body"))
            return runtime::ToolOutput::failure("The connector answered, but its answer is not in the expected form.");
        const std::string notice = jbool(*j, "truncated") ? "The response was truncated at 256 KB.\n" : "";
        return runtime::ToolOutput::success(notice + j->dump());
    }
    std::string code = "HTTP_" + std::to_string(status), message = "The connector call failed.";
    std::string upstream_text;
    long long upstream_status = 0;
    if (j && j->is_object())
        if (const json* e = jobj(*j, "error")) {
            if (!jstr(*e, "code").empty()) code = jstr(*e, "code");
            if (!jstr(*e, "message").empty()) message = jstr(*e, "message");
            upstream_text = jstr(*e, "text");
            upstream_status = jint(*e, "status");
        }
    std::string text = "The connector call failed (" + code + "): " + message;
    if (upstream_status) text += " The API answered " + std::to_string(upstream_status) + ".";
    if (!upstream_text.empty()) text += "\nThe API said: " + upstream_text;
    return runtime::ToolOutput::failure(text);
}

Result<json> list_connectors(const Request& request) {
    if (!request) return make_error("unavailable", "not registered with the control plane");
    return json_answer(request("GET", "api/v1/connectors", ""));
}

Result<json> set_token(const Request& request, const std::string& id, const std::string& token) {
    if (!request) return make_error("unavailable", "not registered with the control plane");
    if (!safe_id(id)) return make_error("invalid", "invalid connector id");
    if (token.empty()) return make_error("invalid", "the token is empty");
    // The body is the only place the token goes: no error text, log line or result carries it.
    return json_answer(request("PUT", "api/v1/connectors/" + id + "/token", json{{"token", token}}.dump()));
}

Result<json> remove_token(const Request& request, const std::string& id) {
    if (!request) return make_error("unavailable", "not registered with the control plane");
    if (!safe_id(id)) return make_error("invalid", "invalid connector id");
    return json_answer(request("DELETE", "api/v1/connectors/" + id + "/token", ""));
}

// ---- manager --------------------------------------------------------------------------------------------------------

namespace {

// One operation as a tool: its call goes through the control plane and its result is untrusted content.
runtime::ToolDef tool_for(const std::shared_ptr<const Request>& request, const Connector& c, const Operation& op) {
    runtime::ToolDef d;
    d.id = tool_id(c.id, op.name);
    d.description = "[" + c.name + ", set by its administrator] " + op.description;
    d.parameters = op.input_schema;
    d.risk = op.risk;
    d.provenance = runtime::Provenance{"api", c.id + "/" + op.name, "", false};
    const std::string connector = c.id, operation = op.name;
    const json schema = op.input_schema;
    d.run = [request, connector, operation, schema](runtime::ToolContext&, const json& args) -> runtime::ToolOutput {
        json a = args.is_object() ? args : json::object();
        a.erase("rationale");  // the approval rationale is not an argument of the operation
        if (auto problem = argument_problem(schema, a)) return runtime::ToolOutput::failure("The call was not made: " + *problem);
        const std::string path = "api/v1/connectors/" + connector + "/operations/" + operation + "/call";
        if (!*request) return runtime::ToolOutput::failure("The connectors are not available, so the call was not made.");
        auto r = (*request)("POST", path, json{{"arguments", a}}.dump());
        runtime::ToolOutput out = r ? call_output(r->status, r->body)
                                    : runtime::ToolOutput::failure("The connector could not be reached, so the call was not made (" + r.error().message + ").");
        out.provenance = runtime::Provenance{"api", connector + "/" + operation, iso8601_utc_now(), false};
        return out;
    };
    return d;
}

}  // namespace

ConnectorManager::ConnectorManager(ManagerEnv env)
    : env_(std::move(env)), request_(std::make_shared<const Request>(env_.request ? env_.request : Request{})) {}

ConnectorManager::~ConnectorManager() { shutdown(); }

void ConnectorManager::changed() {
    if (env_.on_status) env_.on_status();
}

void ConnectorManager::apply() {
    const std::lock_guard apply_lock(apply_mx_);
    const auto p = env_.policy ? env_.policy() : nullptr;
    std::vector<std::string> problems;
    const auto list = parse_connectors(p ? p->find("mcp.servers") : nullptr, &problems);
    for (const auto& pr : problems) log::warn(kLog, pr);

    std::map<std::string, std::vector<std::string>> before;
    {
        const std::lock_guard lock(mx_);
        for (const auto& [id, e] : entries_) before[id] = e.registered;
    }

    std::map<std::string, Entry> next;
    for (const auto& c : list) {
        Entry e;
        e.connector = c;
        std::vector<runtime::ToolDef> defs;
        size_t denied = 0;
        for (const auto& op : c.operations) {
            const std::string id = tool_id(c.id, op.name);
            if (p) {
                const auto m = policy::most_specific(p->find("tools.rules"), id);
                if (m && m->decision == policy::Decision::deny) {
                    ++denied;
                    continue;
                }
            }
            defs.push_back(tool_for(request_, c, op));
        }
        e.usable = defs.size();
        if (defs.empty()) {
            if (c.state == "needs-token" || c.state == "token-rejected") e.reason = c.state;
            else if (c.operations.empty()) e.reason = "none-selected";
            else if (denied) e.reason = "denied-by-policy";
        }
        for (auto& d : defs) {
            e.registered.push_back(d.id);
            if (env_.add_tool)
                if (Status st = env_.add_tool(std::move(d)); !st) log::error(kLog, "tool registration: " + st.error().message);
        }
        next[c.id] = std::move(e);
    }
    // Tools of a connector that is gone, or no longer offered, are withdrawn before this returns.
    for (const auto& [id, ids] : before) {
        const auto it = next.find(id);
        for (const auto& t : ids) {
            const bool kept = it != next.end() && std::find(it->second.registered.begin(), it->second.registered.end(), t) != it->second.registered.end();
            if (!kept && env_.remove_tool) env_.remove_tool(t);
        }
    }
    {
        const std::lock_guard lock(mx_);
        entries_ = std::move(next);
    }
    changed();
}

void ConnectorManager::shutdown() {
    const std::lock_guard apply_lock(apply_mx_);
    std::map<std::string, Entry> old;
    {
        const std::lock_guard lock(mx_);
        old.swap(entries_);
    }
    for (const auto& [id, e] : old)
        for (const auto& t : e.registered)
            if (env_.remove_tool) env_.remove_tool(t);
}

json ConnectorManager::status() const {
    json out = json::array();
    const std::lock_guard lock(mx_);
    for (const auto& [id, e] : entries_) {
        out.push_back({{"id", id},
                       {"name", e.connector.name},
                       {"kind", "api"},
                       {"auth", e.connector.auth},
                       {"state", e.connector.state},
                       {"operations", e.connector.operations.size()},
                       {"usable_tools", e.usable},
                       {"reason", e.reason.empty() ? json(nullptr) : json(e.reason)},
                       {"warning", e.usable == 0 ? json("no usable tools") : json(nullptr)}});
    }
    return out;
}

}  // namespace wb::connectors
