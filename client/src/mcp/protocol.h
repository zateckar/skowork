// The portable core of client/mcp-client (add-core-tools-and-mcp design D3, D8): JSON-RPC 2.0 messages, the parts of
// the MCP protocol the client uses (initialize, tools/list, tools/call), newline framing for the stdio transport, the
// catalog's tool filters and the conversion of call results to text. No Win32 headers, so the fuzz job builds it.
// Every input here comes from an MCP server and is untrusted: nothing throws, and sizes are bounded.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::mcp {

// The protocol version the client speaks when the catalog entry names none.
inline constexpr const char* kDefaultProtocolVersion = "2025-06-18";
// The largest JSON-RPC message accepted from a server (a line on stdio, a body or an SSE event over HTTP).
inline constexpr size_t kMaxMessageBytes = 16u * 1024 * 1024;

// ---- JSON-RPC -------------------------------------------------------------------------------------------------------

json make_request(long long id, const std::string& method, const json& params = nullptr);
json make_notification(const std::string& method, const json& params = nullptr);
json make_result(const json& id, const json& result);
json make_error_response(const json& id, int code, const std::string& message);

struct RpcError {
    long long code = 0;
    std::string message;
};

struct Message {
    enum class Kind { request, notification, response, invalid };
    Kind kind = Kind::invalid;
    json id;              // request and response (number or string)
    std::string method;   // request and notification
    json params;          // request and notification
    json result;          // response
    std::optional<RpcError> error;  // response
};

// Classifies one JSON value. A batch (array) is returned as Kind::invalid: MCP 2025-06-18 removed batching.
Message classify(const json& j);
// Parses and classifies text; malformed JSON gives Kind::invalid.
Message parse_message(std::string_view text);

// Newline-delimited JSON (stdio transport). Feeds bytes in any split; complete lines (without the newline, CR
// stripped, empty lines skipped) are appended to `out`. Returns false once a line grows beyond `max_line`; the
// framer then refuses further input.
class LineFramer {
public:
    explicit LineFramer(size_t max_line = kMaxMessageBytes) : max_(max_line) {}
    bool feed(std::string_view chunk, std::vector<std::string>& out);
    [[nodiscard]] bool failed() const { return failed_; }

private:
    size_t max_;
    std::string line_;
    bool failed_ = false;
};

// ---- MCP ------------------------------------------------------------------------------------------------------------

json initialize_params(const std::string& protocol_version, const std::string& client_version);

struct ServerInfo {
    std::string protocol_version, name, version, instructions;
    bool has_tools = false;
};
Result<ServerInfo> parse_initialize(const json& result);

struct ToolInfo {
    std::string name, title, description;
    json input_schema = {{"type", "object"}, {"properties", json::object()}};
    bool read_only_hint = false;  // the server's own annotation: shown, never used to lower the risk class
};
// One page of tools/list. Tools with a missing or unusable name are skipped; `next_cursor` is empty on the last page.
Result<std::vector<ToolInfo>> parse_tools_page(const json& result, std::string& next_cursor);
// The tools/list cursor loop stops after this many pages.
inline constexpr int kMaxToolPages = 50;

// A tools/call result as text for the model: text blocks as is, images and audio as placeholders, embedded
// resources by their text (or a placeholder for blobs), resource links as "[resource: name <uri>]", and
// structuredContent as JSON when there is no text block. `item` receives the first resource URI, if any.
struct CallText {
    std::string text;
    bool is_error = false;
    std::string item;
};
CallText call_result_text(const json& result);

// The item identifier for provenance (mcp:server/tool#item): a resource URI from the result, else an id-like
// argument (id, key, issue, issueKey, page_id, pageId, uri, url, path), else empty.
std::string item_identifier(const json& args, const CallText& result);

// ---- Catalog --------------------------------------------------------------------------------------------------------

// The catalog's tool name match: `*` matches any run of characters, everything else is literal and case-sensitive
// (the same rule as control-plane/mcp-catalog).
bool tool_pattern_match(std::string_view pattern, std::string_view name);
// Include and exclude lists of a catalog entry: an exclude match always wins; an empty include list includes all.
// `why` receives the matching exclude pattern or "not included".
bool tool_selected(const std::string& name, const std::vector<std::string>& include, const std::vector<std::string>& exclude,
                   std::string* why = nullptr);

// A server id or tool name that may appear in mcp:<server>/<tool>: 1-64 characters of [A-Za-z0-9_.-].
bool valid_name(std::string_view s);

// A catalog entry (policy.mcp.servers[], control-plane/mcp-catalog Entry Delivered In The Policy Bundle).
struct ServerEntry {
    std::string id, name, kind;  // kind: remote | local
    std::string spec_version = kDefaultProtocolVersion;
    std::string url, auth, audience;   // remote; auth: audience-token | none
    std::string package;               // local: SHA-256 hex of the zip
    std::vector<std::string> args;     // local: args[0] is the executable, relative to the package root
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> include_tools, exclude_tools;
    json risk = json::object();  // tool name -> read | write-local | external | destructive
    std::string fingerprint;     // the entry's JSON, for change detection
};
Result<ServerEntry> parse_entry(const json& j);
// Entries that fail to parse are skipped with their reason in `problems`.
std::vector<ServerEntry> parse_entries(const json* servers, std::vector<std::string>* problems = nullptr);

// args[0] of a local entry as a path relative to the package root: no drive, no root, no "..", no ':'.
bool safe_relative_path(std::string_view p);

}  // namespace wb::mcp
