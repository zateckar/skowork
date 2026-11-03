// Fuzz target for the tool-call argument parser and the schema validator (client/model-access Tool Call Validation
// And Repair): any text must give an object or an error, never a crash, hang or unbounded memory use.
#include "base/json_schema.h"
#include "base/tool_args.h"
#include "fuzz_main.h"

namespace {
const wb::json kSchema = wb::json::parse(R"({"type":"object","properties":{"path":{"type":"string","minLength":1},
  "n":{"type":"integer","minimum":0,"maximum":100},"tags":{"type":"array","items":{"type":"string"}}},
  "required":["path"],"additionalProperties":false})");
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto parsed = wb::parse_tool_arguments(text);
    if (parsed.value) {
        if (!parsed.value->is_object()) __builtin_trap();
        (void)wb::validate_json(kSchema, *parsed.value);
        (void)wb::validate_json(*parsed.value, *parsed.value);  // arbitrary documents as schemas must be safe too
    }
    (void)wb::repair_json_text(text);
    return 0;
}
