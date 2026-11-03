// A minimal PNG writer for tests: 8-bit RGB with stored (uncompressed) deflate blocks, so no compression library is
// needed to produce a 4K screenshot.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wb::testing {

inline std::uint32_t crc32_of(const std::uint8_t* data, size_t n, std::uint32_t crc = 0xFFFFFFFFu) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

inline void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

inline void chunk(std::vector<std::uint8_t>& png, const char type[5], const std::vector<std::uint8_t>& body) {
    put32(png, static_cast<std::uint32_t>(body.size()));
    std::vector<std::uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    png.insert(png.end(), typed.begin(), typed.end());
    put32(png, crc32_of(typed.data(), typed.size()) ^ 0xFFFFFFFFu);
}

// A width x height image with a diagonal gradient.
inline std::vector<std::uint8_t> make_png(int width, int height) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 3 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < width; ++x) {
            raw.push_back(static_cast<std::uint8_t>((x * 255) / width));
            raw.push_back(static_cast<std::uint8_t>((y * 255) / height));
            raw.push_back(static_cast<std::uint8_t>(((x + y) * 255) / (width + height)));
        }
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    put32(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(width));
    put32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

}  // namespace wb::testing
