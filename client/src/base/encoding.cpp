#include "base/encoding.h"

#include <array>

namespace wb {
namespace {

constexpr char kStd[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kUrl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode(const std::uint8_t* p, size_t n, const char* alphabet, bool pad) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const unsigned v = (unsigned{p[i]} << 16) | (unsigned{p[i + 1]} << 8) | p[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i < n) {
        unsigned v = unsigned{p[i]} << 16;
        if (i + 1 < n) v |= unsigned{p[i + 1]} << 8;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        if (i + 1 < n) out += alphabet[(v >> 6) & 63];
        else if (pad) out += '=';
        if (pad) out += '=';
    }
    return out;
}

std::optional<bytes> decode(std::string_view s, bool url) {
    std::array<int, 256> map{};
    map.fill(-1);
    const char* alphabet = url ? kUrl : kStd;
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(alphabet[i])] = i;
    while (!s.empty() && s.back() == '=') s.remove_suffix(1);
    if (s.size() % 4 == 1) return std::nullopt;
    bytes out;
    out.reserve(s.size() * 3 / 4);
    unsigned acc = 0;
    int bits = 0;
    for (const char c : s) {
        const int v = map[static_cast<unsigned char>(c)];
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

std::string base64_encode(const std::uint8_t* data, size_t n) { return encode(data, n, kStd, true); }
std::optional<bytes> base64_decode(std::string_view s) { return decode(s, false); }
std::string base64url_encode(const std::uint8_t* data, size_t n) { return encode(data, n, kUrl, false); }
std::optional<bytes> base64url_decode(std::string_view s) { return decode(s, true); }

std::string hex_encode(const std::uint8_t* data, size_t n) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 0xF]);
    }
    return s;
}

}  // namespace wb
