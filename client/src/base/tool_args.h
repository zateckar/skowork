// Parsing of tool-call arguments emitted by a model (client/model-access Tool Call Validation And Repair;
// add-agent-runtime design D7). The input is untrusted text: nothing here throws, and work is bounded by the input
// size.
#pragma once

#include "base/json.h"

#include <optional>
#include <string>
#include <string_view>

namespace wb {

struct ParsedArguments {
    std::optional<json> value;  // always a JSON object when set
    bool repaired = false;      // the text was not valid JSON and a repair made it valid
    std::string error;          // set when value is empty
};

// Largest argument text and deepest nesting that are accepted.
constexpr size_t kMaxToolArgumentBytes = 4u * 1024 * 1024;
constexpr int kMaxToolArgumentDepth = 64;

// Parses tool arguments. Valid JSON objects pass unchanged. The repair covers only unambiguous defects:
//  - a code fence around the JSON (```json ... ```);
//  - trailing commas;
//  - single-quoted strings and unquoted object keys;
//  - raw newlines, tabs and other control characters inside strings, and invalid escapes such as \';
//  - Python literals True, False and None;
//  - arguments that arrive as a JSON string holding the object (double encoding).
// Empty text and `null` mean "no arguments" and give {}. Anything else (a truncated or ambiguous text) fails.
ParsedArguments parse_tool_arguments(std::string_view text);

// The text transformation alone: returns the JSON text after the repairs above, or nullopt when the input has
// nothing to repair or cannot be repaired. Exposed for tests.
std::optional<std::string> repair_json_text(std::string_view text);

}  // namespace wb
