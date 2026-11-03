// Fuzz target for the MCP protocol core (client/mcp-client; add-core-tools-and-mcp design D3): everything an MCP server
// sends is untrusted. Framing, message classification, tools/list pages, initialize and call results must give an
// answer for any input, never a crash or unbounded output.
#include "fuzz_main.h"
#include "mcp/protocol.h"

using namespace wb;
using namespace wb::mcp;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    // stdio framing in two arbitrary chunks; every line is bounded.
    LineFramer framer(4096);
    std::vector<std::string> lines;
    const size_t cut = size ? data[0] % (size + 1) : 0;
    framer.feed(text.substr(0, cut), lines);
    framer.feed(text.substr(cut), lines);
    for (const auto& l : lines)
        if (l.size() > 4096 || l.find('\n') != std::string::npos) __builtin_trap();

    const Message m = parse_message(text);
    if (m.kind == Message::Kind::response && !m.error && !m.id.is_number() && !m.id.is_string()) __builtin_trap();
    if (m.error && m.error->message.size() > 2000) __builtin_trap();

    const auto j = parse_json(text);
    const json value = j ? *j : json(nullptr);
    std::string cursor;
    if (auto tools = parse_tools_page(value, cursor))
        for (const auto& t : *tools) {
            if (!valid_name(t.name)) __builtin_trap();
            if (t.description.size() > 4000) __builtin_trap();
            if (!t.input_schema.is_object() || t.input_schema["type"] != "object") __builtin_trap();
        }
    (void)parse_initialize(value);
    const CallText c = call_result_text(value);
    if (c.text.empty()) __builtin_trap();
    if (item_identifier(value, c).size() > 512) __builtin_trap();
    (void)parse_entry(value);
    const json* arr = value.is_array() ? &value : nullptr;
    (void)parse_entries(arr);

    // The catalog's wildcard matcher with the input split into pattern and name.
    const size_t half = size / 2;
    (void)tool_pattern_match(text.substr(0, half), text.substr(half));
    (void)safe_relative_path(text);
    return 0;
}
