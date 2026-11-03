// A small stdio MCP server for the client/mcp-client tests (packaged into a zip by the tests). Newline-delimited
// JSON-RPC on stdin/stdout. Tools: echo, get_page, list_items (claims read-only), delete_issue, sleep, crash, env.
// tools/list is paged (two pages) to exercise the cursor loop.
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {

json tool(const char* name, const char* description, json props = json::object(), bool read_only = false) {
    json t = {{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"}, {"properties", std::move(props)}}}};
    if (read_only) t["annotations"] = {{"readOnlyHint", true}};
    return t;
}

json text(const std::string& s) { return {{"content", json::array({{{"type", "text"}, {"text", s}}})}}; }

void reply(const json& j) {
    std::cout << j.dump() << "\n";
    std::cout.flush();
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::cerr << "mcp test server starting\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        json m = json::parse(line, nullptr, false);
        if (m.is_discarded() || !m.is_object() || !m.contains("method")) continue;
        if (!m.contains("id")) continue;  // notifications
        const json id = m["id"];
        const std::string method = m["method"].get<std::string>();
        const json params = m.value("params", json::object());
        if (method == "initialize") {
            reply({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", params.value("protocolVersion", "2025-06-18")},
                                                               {"capabilities", {{"tools", json::object()}}},
                                                               {"serverInfo", {{"name", "workbench-test-server"}, {"version", "1.0"}}}}}});
        } else if (method == "tools/list") {
            const std::string cursor = params.value("cursor", "");
            json tools = json::array();
            json result;
            if (cursor.empty()) {
                tools.push_back(tool("echo", "Echoes the text.", {{"text", {{"type", "string"}}}}));
                tools.push_back(tool("get_page", "Returns a page by id.", {{"page_id", {{"type", "string"}}}}));
                tools.push_back(tool("list_items", "Lists items.", json::object(), true));
                result = {{"tools", tools}, {"nextCursor", "page2"}};
            } else {
                tools.push_back(tool("delete_issue", "Deletes an issue.", {{"key", {{"type", "string"}}}}));
                tools.push_back(tool("sleep", "Sleeps.", {{"ms", {{"type", "integer"}}}}));
                tools.push_back(tool("crash", "Exits at once."));
                tools.push_back(tool("env", "Reports the environment."));
                tools.push_back(tool("alloc", "Allocates memory.", {{"mb", {{"type", "integer"}}}}));
                result = {{"tools", tools}};
            }
            reply({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        } else if (method == "tools/call") {
            const std::string name = params.value("name", "");
            const json args = params.value("arguments", json::object());
            json result;
            if (name == "echo") {
                result = text("echo: " + args.value("text", ""));
                if (args.contains("rationale")) result = text("unexpected rationale argument");
            } else if (name == "get_page") {
                const std::string pid = args.value("page_id", "");
                result = {{"content", json::array({{{"type", "resource"},
                                                    {"resource", {{"uri", "page://" + pid}, {"mimeType", "text/plain"}, {"text", "Page " + pid + " body"}}}}})}};
            } else if (name == "list_items") {
                result = text("a, b, c");
            } else if (name == "delete_issue") {
                result = text("deleted " + args.value("key", ""));
            } else if (name == "sleep") {
                std::this_thread::sleep_for(std::chrono::milliseconds(args.value("ms", 1000)));
                result = text("slept");
            } else if (name == "alloc") {
                const size_t mb = args.value("mb", 1);
                auto* p = new char[mb * 1024 * 1024];
                for (size_t i = 0; i < mb * 1024 * 1024; i += 4096) p[i] = 1;
                result = text("allocated " + std::to_string(mb) + " MB" + (p[0] ? "" : "?"));
                delete[] p;
            } else if (name == "crash") {
                std::cerr << "crashing on purpose\n";
                std::cerr.flush();
                std::_Exit(3);
            } else if (name == "env") {
                result = text("MODE=" + env("MODE") + " PROXY=" + (env("HTTPS_PROXY").empty() ? "no" : "yes") +
                              " SECRET=" + (env("WORKBENCH_TEST_SECRET").empty() ? "no" : "yes"));
            } else {
                reply({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "unknown tool " + name}}}});
                continue;
            }
            reply({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        } else if (method == "ping") {
            reply({{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}});
        } else {
            reply({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "method not found"}}}});
        }
    }
    return 0;
}
