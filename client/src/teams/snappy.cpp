// Raw Snappy block format decoder (client/teams; add-local-outlook-and-teams design D4).
#include "teams/snappy.h"

#include <cstdint>

#include "teams/bytes.h"

namespace wb::teams {

Result<std::string> snappy_decompress(std::string_view in, std::size_t max_output) {
    Cursor c(in);
    std::uint32_t declared = 0;
    if (!c.varint32(declared)) return make_error("snappy", "bad length header");
    if (declared > max_output) return make_error("too_large", "snappy output exceeds the limit");
    // A copy element of 3 bytes yields at most 64: anything claiming more than ~22x its input is not Snappy.
    if (declared / 22 > in.size()) return make_error("snappy", "declared length larger than the input can produce");
    std::string out;
    out.reserve(declared);
    while (!c.empty()) {
        std::uint8_t tag = 0;
        c.u8(tag);
        const unsigned kind = tag & 3u;
        if (kind == 0) {  // literal
            std::size_t len = (tag >> 2) + 1u;
            if ((tag >> 2) >= 60) {
                const unsigned extra = (tag >> 2) - 59u;  // 1..4 bytes
                std::uint32_t v = 0;
                for (unsigned i = 0; i < extra; ++i) {
                    std::uint8_t b = 0;
                    if (!c.u8(b)) return make_error("snappy", "truncated literal length");
                    v |= static_cast<std::uint32_t>(b) << (8 * i);
                }
                len = static_cast<std::size_t>(v) + 1;
            }
            std::string_view lit;
            if (!c.bytes(len, lit)) return make_error("snappy", "truncated literal");
            if (out.size() + len > declared) return make_error("snappy", "output longer than declared");
            out.append(lit);
            continue;
        }
        std::size_t len = 0;
        std::size_t offset = 0;
        if (kind == 1) {
            std::uint8_t b = 0;
            if (!c.u8(b)) return make_error("snappy", "truncated copy");
            len = 4 + ((tag >> 2) & 7u);
            offset = (static_cast<std::size_t>(tag >> 5) << 8) | b;
        } else if (kind == 2) {
            std::uint8_t b0 = 0, b1 = 0;
            if (!c.u8(b0) || !c.u8(b1)) return make_error("snappy", "truncated copy");
            len = (tag >> 2) + 1u;
            offset = static_cast<std::size_t>(b0) | (static_cast<std::size_t>(b1) << 8);
        } else {
            std::uint32_t v = 0;
            if (!c.fixed32(v)) return make_error("snappy", "truncated copy");
            len = (tag >> 2) + 1u;
            offset = v;
        }
        if (offset == 0 || offset > out.size()) return make_error("snappy", "copy offset out of range");
        if (out.size() + len > declared) return make_error("snappy", "output longer than declared");
        // Overlapping copies are allowed (offset < len repeats the pattern), so copy byte by byte.
        std::size_t from = out.size() - offset;
        for (std::size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
    if (out.size() != declared) return make_error("snappy", "output shorter than declared");
    return out;
}

}  // namespace wb::teams
