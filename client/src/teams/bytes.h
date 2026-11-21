// Bounds-checked byte reading and text conversion shared by the Teams cache decoders (client/teams;
// add-local-outlook-and-teams design D4). Every read reports failure instead of reading past the end, so malformed or
// truncated cache files never cause undefined behaviour.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace wb::teams {

struct Cursor {
    std::string_view s;
    std::size_t pos = 0;

    explicit Cursor(std::string_view data) : s(data) {}

    [[nodiscard]] std::size_t remaining() const { return pos <= s.size() ? s.size() - pos : 0; }
    [[nodiscard]] bool empty() const { return remaining() == 0; }
    [[nodiscard]] std::string_view rest() const { return s.substr(pos < s.size() ? pos : s.size()); }

    bool u8(std::uint8_t& out) {
        if (remaining() < 1) return false;
        out = static_cast<std::uint8_t>(s[pos++]);
        return true;
    }
    bool peek(std::uint8_t& out) const {
        if (remaining() < 1) return false;
        out = static_cast<std::uint8_t>(s[pos]);
        return true;
    }
    bool skip(std::size_t n) {
        if (remaining() < n) return false;
        pos += n;
        return true;
    }
    bool bytes(std::size_t n, std::string_view& out) {
        if (remaining() < n) return false;
        out = s.substr(pos, n);
        pos += n;
        return true;
    }
    bool fixed32(std::uint32_t& out) {
        if (remaining() < 4) return false;
        out = 0;
        for (int i = 3; i >= 0; --i) out = (out << 8) | static_cast<std::uint8_t>(s[pos + static_cast<std::size_t>(i)]);
        pos += 4;
        return true;
    }
    bool fixed64(std::uint64_t& out) {
        if (remaining() < 8) return false;
        out = 0;
        for (int i = 7; i >= 0; --i) out = (out << 8) | static_cast<std::uint8_t>(s[pos + static_cast<std::size_t>(i)]);
        pos += 8;
        return true;
    }
    bool f64(double& out) {
        std::uint64_t bits = 0;
        if (!fixed64(bits)) return false;
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    bool varint64(std::uint64_t& out) {
        out = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            std::uint8_t b = 0;
            if (!u8(b)) return false;
            out |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return true;
        }
        return false;  // more than 10 bytes
    }
    bool varint32(std::uint32_t& out) {
        out = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            std::uint8_t b = 0;
            if (!u8(b)) return false;
            out |= static_cast<std::uint32_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return true;
        }
        return false;
    }
    // A varint32-length-prefixed slice (LevelDB encoding).
    bool slice(std::string_view& out) {
        std::uint32_t n = 0;
        return varint32(n) && bytes(n, out);
    }
};

inline void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

// UTF-16 code units (big- or little-endian) to UTF-8; lone surrogates become U+FFFD, an odd trailing byte is dropped.
inline std::string utf16_to_utf8(std::string_view bytes, bool big_endian) {
    std::string out;
    out.reserve(bytes.size());
    const std::size_t n = bytes.size() / 2;
    auto unit = [&](std::size_t i) -> std::uint32_t {
        const auto a = static_cast<std::uint8_t>(bytes[2 * i]);
        const auto b = static_cast<std::uint8_t>(bytes[2 * i + 1]);
        return big_endian ? (static_cast<std::uint32_t>(a) << 8 | b) : (static_cast<std::uint32_t>(b) << 8 | a);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t u = unit(i);
        if (u >= 0xd800 && u <= 0xdbff && i + 1 < n) {
            const std::uint32_t v = unit(i + 1);
            if (v >= 0xdc00 && v <= 0xdfff) {
                append_utf8(out, 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00));
                ++i;
                continue;
            }
        }
        append_utf8(out, u);
    }
    return out;
}

inline std::string latin1_to_utf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const char c : bytes) append_utf8(out, static_cast<std::uint8_t>(c));
    return out;
}

// Replaces invalid UTF-8 sequences with U+FFFD so the text is always safe to put into JSON and dump.
inline std::string sanitize_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto c = static_cast<std::uint8_t>(in[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xe0) == 0xc0) { len = 2; cp = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { len = 3; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { len = 4; cp = c & 0x07; }
        bool ok = len != 0 && i + len <= in.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            const auto d = static_cast<std::uint8_t>(in[i + k]);
            if ((d & 0xc0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3f);
        }
        if (ok) {
            const std::uint32_t min = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
            if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) ok = false;
        }
        if (ok) {
            out.append(in.substr(i, len));
            i += len;
        } else {
            append_utf8(out, 0xfffd);
            ++i;
        }
    }
    return out;
}

}  // namespace wb::teams
