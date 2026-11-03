// Fuzz target for the in-house HTML-to-text extractor (client/core-tools Web Fetch Tool; add-core-tools-and-mcp design
// D4): web pages are untrusted input. Any input must give an answer, never a crash, hang or unbounded output.
#include "fuzz_main.h"
#include "tools/html.h"

using namespace wb::tools;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    constexpr std::string_view base = "https://example.com/dir/page.html?q=1";
    const HtmlText h = html_to_text(text, base);
    // Output is bounded by the input: entities expand to at most 4 bytes per input byte, markup adds a few bytes per tag,
    // and each of at most 64 links adds its resolved target.
    if (h.text.size() > 8 * size + 64 * (base.size() + 8) + 16) __builtin_trap();
    if (h.title.size() > 4 * size + 4) __builtin_trap();
    // Never more than one empty line in a row, and no trailing blank lines.
    if (h.text.find("\n\n\n") != std::string::npos) __builtin_trap();
    if (!h.text.empty() && (h.text.back() != '\n' || (h.text.size() > 1 && h.text[h.text.size() - 2] == '\n'))) __builtin_trap();
    // Entity decoding never grows beyond 4 bytes per input byte.
    if (decode_entities(text).size() > 4 * size) __builtin_trap();
    return 0;
}
