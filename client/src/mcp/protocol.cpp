#include "mcp/protocol.h"

#include <algorithm>

namespace wb::mcp {

// ---- JSON-RPC -------------------------------------------------------------------------------------------------------

json make_request(long long id, const std::string& method, const json& params) {
    json j = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) j["params"] = params;
    return j;
}

json make_notification(const std::string& method, const json& params) {
    json j = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) j["params"] = params;
    return j;
}

json make_result(const json& id, const json& result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; }

json make_error_response(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Message classify(const json& j) {
    Message m;
    if (!j.is_object()) return m;
    const auto id = j.find("id");
    const bool has_id = id != j.end() && (id->is_number_integer() || id->is_number_unsigned() || id->is_string());
    const auto method = j.find("method");
    if (method != j.end()) {
        if (!method->is_string()) return m;
        m.method = method->get<std::string>();
        const auto p = j.find("params");
        if (p != j.end()) m.params = *p;
        if (has_id) {
            m.kind = Message::Kind::request;
            m.id = *id;
        } else if (id == j.end()) {
            m.kind = Message::Kind::notification;
        }
        return m;
    }
    if (!has_id) return m;
    m.id = *id;
    const auto r = j.find("result");
    const auto e = j.find("error");
    if (e != j.end() && e->is_object()) {
        RpcError err;
        err.code = jint(*e, "code", -32603);
        err.message = jstr(*e, "message", "error");
        if (err.message.size() > 2000) err.message.resize(2000);
        m.error = std::move(err);
        m.kind = Message::Kind::response;
    } else if (r != j.end()) {
        m.result = *r;
        m.kind = Message::Kind::response;
    }
    return m;
}

Message parse_message(std::string_view text) {
    if (text.size() > kMaxMessageBytes) return {};
    auto j = parse_json(text);
    if (!j) return {};
    return classify(*j);
}

bool LineFramer::feed(std::string_view chunk, std::vector<std::string>& out) {
    if (failed_) return false;
    for (const char c : chunk) {
        if (c == '\n') {
            if (!line_.empty() && line_.back() == '\r') line_.pop_back();
            if (!line_.empty()) out.push_back(std::move(line_));
            line_.clear();
            continue;
        }
        if (line_.size() >= max_) {
            failed_ = true;
            line_.clear();
            return false;
        }
        line_.push_back(c);
    }
    return true;
}

// ---- MCP ------------------------------------------------------------------------------------------------------------

json initialize_params(const std::string& protocol_version, const std::string& client_version) {
    return {{"protocolVersion", protocol_version.empty() ? std::string(kDefaultProtocolVersion) : protocol_version},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "Workbench"}, {"version", client_version}}}};
}

Result<ServerInfo> parse_initialize(const json& result) {
    if (!result.is_object()) return make_error("protocol", "initialize returned no object");
    ServerInfo s;
    s.protocol_version = jstr(result, "protocolVersion");
    if (s.protocol_version.empty()) return make_error("protocol", "initialize returned no protocolVersion");
    if (const json* info = jobj(result, "serverInfo")) {
        s.name = jstr(*info, "name");
        s.version = jstr(*info, "version");
    }
    s.instructions = jstr(result, "instructions");
    if (s.instructions.size() > 8000) s.instructions.resize(8000);
    if (const json* caps = jobj(result, "capabilities")) s.has_tools = caps->contains("tools");
    return s;
}

Result<std::vector<ToolInfo>> parse_tools_page(const json& result, std::string& next_cursor) {
    next_cursor.clear();
    if (!result.is_object()) return make_error("protocol", "tools/list returned no object");
    const auto tools = result.find("tools");
    if (tools == result.end() || !tools->is_array()) return make_error("protocol", "tools/list returned no tools array");
    std::vector<ToolInfo> out;
    for (const auto& t : *tools) {
        if (!t.is_object()) continue;
        ToolInfo info;
        info.name = jstr(t, "name");
        if (!valid_name(info.name)) continue;
        info.title = jstr(t, "title");
        info.description = jstr(t, "description");
        if (info.description.size() > 4000) info.description.resize(4000);
        const auto schema = t.find("inputSchema");
        if (schema != t.end() && schema->is_object()) {
            info.input_schema = *schema;
            info.input_schema["type"] = "object";
        }
        if (const json* ann = jobj(t, "annotations")) info.read_only_hint = jbool(*ann, "readOnlyHint");
        out.push_back(std::move(info));
    }
    next_cursor = jstr(result, "nextCursor");
    return out;
}

namespace {

void append_block(std::string& text, const std::string& block) {
    if (block.empty()) return;
    if (!text.empty()) text += "\n\n";
    text += block;
}

}  // namespace

CallText call_result_text(const json& result) {
    CallText c;
    if (!result.is_object()) {
        c.is_error = true;
        c.text = "The MCP server returned a malformed result.";
        return c;
    }
    c.is_error = jbool(result, "isError");
    bool any_text = false;
    if (const json* content = jobj(result, "content"); content && content->is_array()) {
        for (const auto& b : *content) {
            const std::string type = jstr(b, "type");
            if (type == "text") {
                append_block(c.text, jstr(b, "text"));
                any_text = true;
            } else if (type == "image" || type == "audio") {
                append_block(c.text, "[" + type + ": " + jstr(b, "mimeType", "unknown type") + ", not shown]");
            } else if (type == "resource") {
                const json* r = jobj(b, "resource");
                const std::string uri = r ? jstr(*r, "uri") : std::string{};
                if (c.item.empty()) c.item = uri;
                if (r && r->contains("text")) {
                    append_block(c.text, (uri.empty() ? std::string{} : "[resource " + uri + "]\n") + jstr(*r, "text"));
                    any_text = true;
                } else {
                    append_block(c.text, "[binary resource " + uri + ", not shown]");
                }
            } else if (type == "resource_link") {
                const std::string uri = jstr(b, "uri");
                if (c.item.empty()) c.item = uri;
                append_block(c.text, "[resource: " + jstr(b, "name", uri) + " <" + uri + ">]");
            } else if (!type.empty()) {
                append_block(c.text, "[" + type + " content, not shown]");
            }
        }
    }
    if (!any_text) {
        const auto sc = result.find("structuredContent");
        if (sc != result.end() && !sc->is_null()) append_block(c.text, sc->dump(2, ' ', false, json::error_handler_t::replace));
    }
    if (c.text.empty()) c.text = c.is_error ? "The MCP tool failed without a message." : "(no content)";
    return c;
}

std::string item_identifier(const json& args, const CallText& result) {
    if (!result.item.empty()) return result.item.substr(0, 512);
    if (!args.is_object()) return {};
    for (const char* k : {"id", "key", "issue", "issueKey", "issue_key", "page_id", "pageId", "uri", "url", "path"}) {
        const auto it = args.find(k);
        if (it == args.end()) continue;
        if (it->is_string() && !it->get<std::string>().empty()) return it->get<std::string>().substr(0, 512);
        if (it->is_number_integer() || it->is_number_unsigned()) return it->dump();
    }
    return {};
}

// ---- Catalog --------------------------------------------------------------------------------------------------------

bool tool_pattern_match(std::string_view p, std::string_view s) {
    // Iterative wildcard match with backtracking to the last '*' (linear in practice, no recursion).
    size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (pi < p.size() && p[pi] == s[si]) {
            ++pi;
            ++si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool tool_selected(const std::string& name, const std::vector<std::string>& include, const std::vector<std::string>& exclude,
                   std::string* why) {
    for (const auto& p : exclude)
        if (tool_pattern_match(p, name)) {
            if (why) *why = "excluded by the catalog (" + p + ")";
            return false;
        }
    if (include.empty()) return true;
    for (const auto& p : include)
        if (tool_pattern_match(p, name)) return true;
    if (why) *why = "not in the catalog's include list";
    return false;
}

bool valid_name(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

bool safe_relative_path(std::string_view p) {
    if (p.empty() || p.size() > 260) return false;
    if (p.front() == '\\' || p.front() == '/' || p.find(':') != std::string_view::npos) return false;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find_first_of("\\/", start);
        const std::string_view seg = p.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (seg == ".." || seg == ".") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    for (const char c : p)
        if (static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

namespace {

std::vector<std::string> string_list(const json& j, const char* key) {
    std::vector<std::string> out;
    const json* a = jobj(j, key);
    if (!a || !a->is_array()) return out;
    for (const auto& v : *a)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

}  // namespace

Result<ServerEntry> parse_entry(const json& j) {
    if (!j.is_object()) return make_error("invalid_entry", "not an object");
    ServerEntry e;
    e.id = jstr(j, "id");
    if (!valid_name(e.id)) return make_error("invalid_entry", "invalid server id");
    e.name = jstr(j, "name", e.id);
    e.kind = jstr(j, "kind");
    e.spec_version = jstr(j, "spec_version", kDefaultProtocolVersion);
    e.include_tools = string_list(j, "include_tools");
    e.exclude_tools = string_list(j, "exclude_tools");
    if (const json* r = jobj(j, "risk"); r && r->is_object()) e.risk = *r;
    if (e.kind == "remote") {
        e.url = jstr(j, "url");
        e.auth = jstr(j, "auth", "none");
        if (e.url.empty()) return make_error("invalid_entry", e.id + ": a remote server has no url");
        if (e.auth != "user-token" && e.auth != "none") return make_error("invalid_entry", e.id + ": unknown authentication " + e.auth);
    } else if (e.kind == "local") {
        e.package = jstr(j, "package");
        if (e.package.size() != 64 || !std::all_of(e.package.begin(), e.package.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            return make_error("invalid_entry", e.id + ": a local server needs the package's SHA-256 (64 lower-case hex digits)");
        e.args = string_list(j, "args");
        if (e.args.empty() || !safe_relative_path(e.args.front()))
            return make_error("invalid_entry", e.id + ": args[0] must be a relative path inside the package");
        if (const json* env = jobj(j, "env"); env && env->is_object())
            for (const auto& [k, v] : env->items())
                if (v.is_string()) e.env.emplace_back(k, v.get<std::string>());
    } else {
        return make_error("invalid_entry", e.id + ": unknown kind '" + e.kind + "'");
    }
    e.fingerprint = j.dump();
    return e;
}

std::vector<ServerEntry> parse_entries(const json* servers, std::vector<std::string>* problems) {
    std::vector<ServerEntry> out;
    if (!servers || !servers->is_array()) return out;
    for (const auto& s : *servers) {
        if (s.is_object() && jstr(s, "kind") == "api") continue;  // API connectors are client/api-connectors'
        auto e = parse_entry(s);
        if (!e) {
            if (problems) problems->push_back(e.error().message);
            continue;
        }
        if (std::any_of(out.begin(), out.end(), [&](const ServerEntry& o) { return o.id == e->id; })) {
            if (problems) problems->push_back(e->id + ": duplicate server id");
            continue;
        }
        out.push_back(std::move(*e));
    }
    return out;
}

}  // namespace wb::mcp
