// Fuzz target for evaluation export lines (client/evaluation; add-evaluation-export task 4.4): any subject or body text,
// including invalid UTF-8, NUL and line breaks, gives exactly one line that parses as JSON.
#include "fuzz_main.h"
#include "eval/jsonl.h"

#include <string>

using namespace wb;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    const std::string half = bytes.substr(0, size / 2);
    const std::string line = eval::jsonl_line(json{{"subject", half}, {"body", bytes}, {"to", json::array({bytes})}});
    if (line.find('\n') != std::string::npos) __builtin_trap();
    if (!parse_json(line)) __builtin_trap();
    return 0;
}
