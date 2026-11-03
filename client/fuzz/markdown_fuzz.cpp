// Fuzz target for Markdown rendering of model output (client/work-inbox Markdown Rendering; add-native-ui design D4):
// model output is untrusted. Any input must parse without a crash or hang, with every span inside its text.
#include "fuzz_main.h"
#include "ui/markdown.h"
#include "ui/text16.h"

using namespace wb::ui;

namespace {
void check_spans(const std::u16string& text, const std::vector<Span>& spans) {
    for (const Span& s : spans) {
        if (s.length == 0 || s.start > text.size() || s.length > text.size() - s.start) __builtin_trap();
        if ((s.style & kLink) == 0 && !s.url.empty()) __builtin_trap();
    }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const std::vector<Block> blocks = parse_markdown(text);
    size_t total = 0;
    for (const Block& b : blocks) {
        check_spans(b.text, b.spans);
        total += b.text.size() + b.marker.size() + b.language.size();
        for (const auto& row : b.rows)
            for (const Cell& c : row) {
                check_spans(c.text, c.spans);
                total += c.text.size();
            }
        if (b.kind == BlockKind::heading && (b.level < 1 || b.level > 6)) __builtin_trap();
    }
    // Output is bounded by the input: an image repeats its address after the alternative text, list markers and
    // numeric entities add a few units each.
    if (total > 4 * size + 64) __builtin_trap();
    // UTF-16 conversion round-trips whatever it produced.
    const std::u16string u = to_utf16(text);
    if (to_utf16(to_utf8(u)) != u) __builtin_trap();
    return 0;
}
