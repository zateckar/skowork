// Fuzz target for the knowledge core (client/knowledge-graph; add-knowledge-graph task 5.4): decoding, the chunkers, quoted
// history stripping, policy globs and the extraction parser must give an answer for any input, never a crash; chunks are
// valid UTF-8 within the item limit, and parsed extractions only point at chunks that exist.
#include "fuzz_main.h"
#include "knowledge/core.h"

using namespace wb;
using namespace wb::knowledge;

namespace {

// Text the host sends on as JSON must be valid UTF-8 (nlohmann::json throws on dump otherwise).
bool utf8_valid(const std::string& s) {
    try {
        (void)json(s).dump();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    const std::string text = decode_text(bytes);
    if (!utf8_valid(text)) __builtin_trap();
    (void)looks_binary(bytes);
    (void)file_kind(bytes);
    (void)normalize_name(text);
    (void)estimate_tokens(text);

    for (const char* kind : {"md", "code", "mail", "txt"}) {
        const auto chunks = chunk_text(kind, text);
        size_t total = 0;
        for (const auto& c : chunks) {
            if (!utf8_valid(c.text) || !utf8_valid(c.heading)) __builtin_trap();
            total += c.text.size();
        }
        if (total > 4 * kMaxItemText + 4096) __builtin_trap();
        (void)chunks_from_json(chunks_json(chunks));
    }
    (void)strip_quoted(text);

    // The first line is a glob pattern, the rest a path.
    const size_t nl = text.find('\n');
    if (nl != std::string::npos) (void)glob_match(std::string_view(text).substr(0, nl), std::string_view(text).substr(nl + 1));

    const int chunk_count = static_cast<int>(size % 9);
    if (const auto x = parse_extraction(text, chunk_count)) {
        for (const auto& e : x->entities)
            for (const int c : e.chunks)
                if (c < 0 || c >= chunk_count) __builtin_trap();
        for (const auto& r : x->relations)
            if (r.confidence < 0.0 || r.confidence > 1.0 || r.chunk < -1 || r.chunk >= chunk_count) __builtin_trap();
    }
    return 0;
}
