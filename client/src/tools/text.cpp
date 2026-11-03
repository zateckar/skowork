#include "tools/text.h"

#include "base/strings.h"

#include <array>
#include <cstdint>

namespace wb::tools {

namespace {

// Windows-1252 code points for 0x80..0x9F (0 = undefined, decoded as U+FFFD).
constexpr std::array<char32_t, 32> kCp1252 = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

void put_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Decodes one UTF-8 sequence at s[i]; returns its length, 0 when invalid.
size_t utf8_seq(std::string_view s, size_t i, char32_t& cp) {
    const auto b = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    if (b < 0x80) {
        cp = b;
        return 1;
    }
    if ((b & 0xE0) == 0xC0) n = 2, cp = b & 0x1F;
    else if ((b & 0xF0) == 0xE0) n = 3, cp = b & 0x0F;
    else if ((b & 0xF8) == 0xF0) n = 4, cp = b & 0x07;
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        const auto c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (c & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        char32_t cp = 0;
        const size_t n = utf8_seq(s, i, cp);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

std::string from_utf16(std::string_view b, bool big_endian) {
    std::string out;
    out.reserve(b.size());
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        const auto lo = static_cast<unsigned char>(b[i + (big_endian ? 1 : 0)]);
        const auto hi = static_cast<unsigned char>(b[i + (big_endian ? 0 : 1)]);
        char32_t c = static_cast<char32_t>(lo | (hi << 8));
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < b.size()) {
            const auto lo2 = static_cast<unsigned char>(b[i + 2 + (big_endian ? 1 : 0)]);
            const auto hi2 = static_cast<unsigned char>(b[i + 2 + (big_endian ? 0 : 1)]);
            const char32_t c2 = static_cast<char32_t>(lo2 | (hi2 << 8));
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i += 2;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        put_utf8(out, c);
    }
    return out;
}

void put_utf16(std::string& out, char32_t c, bool big_endian) {
    auto unit = [&](std::uint16_t u) {
        if (big_endian) {
            out += static_cast<char>(u >> 8);
            out += static_cast<char>(u & 0xFF);
        } else {
            out += static_cast<char>(u & 0xFF);
            out += static_cast<char>(u >> 8);
        }
    };
    if (c >= 0x10000) {
        c -= 0x10000;
        unit(static_cast<std::uint16_t>(0xD800 + (c >> 10)));
        unit(static_cast<std::uint16_t>(0xDC00 + (c & 0x3FF)));
    } else {
        unit(static_cast<std::uint16_t>(c));
    }
}

}  // namespace

bool looks_binary(std::string_view bytes) {
    if (bytes.size() >= 2 && ((bytes[0] == '\xFF' && bytes[1] == '\xFE') || (bytes[0] == '\xFE' && bytes[1] == '\xFF'))) return false;
    const std::string_view head = bytes.substr(0, 8192);
    size_t control = 0;
    for (const char ch : head) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == 0) return true;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != '\v' && c != 0x1B) ++control;
    }
    return !head.empty() && control * 10 > head.size();
}

std::optional<DecodedText> decode_text(std::string_view b) {
    if (b.size() >= 2 && b[0] == '\xFF' && b[1] == '\xFE') return DecodedText{from_utf16(b.substr(2), false), TextEncoding::utf16le};
    if (b.size() >= 2 && b[0] == '\xFE' && b[1] == '\xFF') return DecodedText{from_utf16(b.substr(2), true), TextEncoding::utf16be};
    if (looks_binary(b)) return std::nullopt;
    if (b.size() >= 3 && b.substr(0, 3) == "\xEF\xBB\xBF") {
        const std::string_view rest = b.substr(3);
        if (valid_utf8(rest)) return DecodedText{std::string(rest), TextEncoding::utf8_bom};
    }
    if (valid_utf8(b)) return DecodedText{std::string(b), TextEncoding::utf8};
    DecodedText d{{}, TextEncoding::ansi};
    d.text.reserve(b.size() + b.size() / 8);
    for (const char ch : b) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80 && c < 0xA0) put_utf8(d.text, kCp1252[c - 0x80] ? kCp1252[c - 0x80] : 0xFFFD);
        else put_utf8(d.text, c);
    }
    return d;
}

std::string encode_text(std::string_view utf8, TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::utf8: return std::string(utf8);
    case TextEncoding::utf8_bom: return "\xEF\xBB\xBF" + std::string(utf8);
    case TextEncoding::utf16le:
    case TextEncoding::utf16be: {
        const bool be = encoding == TextEncoding::utf16be;
        std::string out = be ? std::string("\xFE\xFF") : std::string("\xFF\xFE");
        for (size_t i = 0; i < utf8.size();) {
            char32_t cp = 0xFFFD;
            const size_t n = utf8_seq(utf8, i, cp);
            put_utf16(out, n ? cp : 0xFFFD, be);
            i += n ? n : 1;
        }
        return out;
    }
    case TextEncoding::ansi: {
        std::string out;
        for (size_t i = 0; i < utf8.size();) {
            char32_t cp = 0;
            const size_t n = utf8_seq(utf8, i, cp);
            i += n ? n : 1;
            if (!n) {
                out += '?';
            } else if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
                out += static_cast<char>(cp);
            } else {
                char mapped = '?';
                for (size_t k = 0; k < kCp1252.size(); ++k)
                    if (kCp1252[k] == cp) mapped = static_cast<char>(0x80 + k);
                out += mapped;
            }
        }
        return out;
    }
    }
    return std::string(utf8);
}

std::string document_kind(std::string_view path) {
    const size_t dot = path.find_last_of('.');
    const size_t sep = path.find_last_of("\\/");
    if (dot == std::string_view::npos || (sep != std::string_view::npos && dot < sep)) return {};
    const std::string ext = to_lower(path.substr(dot + 1));
    if (ext == "docx" || ext == "docm") return "docx";
    if (ext == "xlsx" || ext == "xlsm") return "xlsx";
    if (ext == "pptx" || ext == "pptm") return "pptx";
    if (ext == "pdf") return "pdf";
    return {};
}

size_t count_lines(std::string_view text) {
    if (text.empty()) return 0;
    size_t n = 0;
    for (const char c : text)
        if (c == '\n') ++n;
    return text.back() == '\n' ? n : n + 1;
}

LineSlice slice_lines(std::string_view text, size_t first, size_t limit, size_t max_bytes, bool numbered) {
    LineSlice s;
    if (first == 0) first = 1;
    s.first = first;
    s.last = first - 1;
    size_t line = 1, pos = 0;
    while (line < first && pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            pos = text.size();
            break;
        }
        pos = nl + 1;
        ++line;
    }
    if (line < first) return s;
    while (pos < text.size() && (limit == 0 || line < first + limit)) {
        size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
        std::string piece = numbered ? std::to_string(line) + "\t" : std::string();
        piece.append(text.substr(pos, end - pos));
        if (s.text.size() + piece.size() > max_bytes) {
            if (s.text.empty()) {
                // A single line longer than the limit: return its beginning.
                s.text = utf8_truncate(piece, max_bytes);
                s.last = line;
            }
            s.truncated = true;
            return s;
        }
        s.text += piece;
        s.last = line;
        pos = end;
        ++line;
    }
    return s;
}

size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size())) ++n;
    return n;
}

}  // namespace wb::tools
