// Fuzz target for the SSE framing parser: arbitrary bytes, split at arbitrary points, must not crash.
#include "base/sse.h"
#include "fuzz_main.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    wb::SseParser parser(1 << 20);
    std::vector<wb::SseEvent> out;
    const std::size_t step = 1 + data[0] % 17;  // the first byte picks the chunk size
    for (std::size_t i = 0; i < text.size(); i += step) {
        if (!parser.feed(text.substr(i, step), out)) break;
    }
    (void)parser.has_partial();
    return 0;
}
