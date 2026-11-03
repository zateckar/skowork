#include "tools/image_info.h"

namespace wb::tools {

namespace {

unsigned byte_at(std::string_view b, size_t i) { return static_cast<unsigned char>(b[i]); }
unsigned be16(std::string_view b, size_t i) { return (byte_at(b, i) << 8) | byte_at(b, i + 1); }
unsigned long be32(std::string_view b, size_t i) {
    return (static_cast<unsigned long>(be16(b, i)) << 16) | be16(b, i + 2);
}
unsigned le16(std::string_view b, size_t i) { return byte_at(b, i) | (byte_at(b, i + 1) << 8); }
unsigned long le24(std::string_view b, size_t i) {
    return le16(b, i) | (static_cast<unsigned long>(byte_at(b, i + 2)) << 16);
}

std::optional<ImageInfo> sized(const char* mime, const char* ext, unsigned long w, unsigned long h) {
    if (w == 0 || h == 0 || w > 65535 || h > 65535) return std::nullopt;
    return ImageInfo{mime, ext, static_cast<int>(w), static_cast<int>(h)};
}

std::optional<ImageInfo> png(std::string_view b) {
    // Signature, then the IHDR chunk first: length, "IHDR", width, height (big-endian).
    if (b.size() < 24 || b.substr(0, 8) != std::string_view("\x89PNG\r\n\x1a\n", 8) || b.substr(12, 4) != "IHDR")
        return std::nullopt;
    return sized("image/png", "png", be32(b, 16), be32(b, 20));
}

std::optional<ImageInfo> jpeg(std::string_view b) {
    if (b.size() < 4 || byte_at(b, 0) != 0xFF || byte_at(b, 1) != 0xD8) return std::nullopt;
    size_t i = 2;
    while (i + 4 <= b.size()) {
        if (byte_at(b, i) != 0xFF) return std::nullopt;
        const unsigned marker = byte_at(b, i + 1);
        if (marker == 0xFF) {  // fill byte
            ++i;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {  // no length
            i += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) return std::nullopt;  // end of image or scan data before a frame header
        const unsigned len = be16(b, i + 2);
        if (len < 2) return std::nullopt;
        // Start of frame: every SOFn except DHT (C4), JPG (C8) and DAC (CC).
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 9 > b.size() || len < 7) return std::nullopt;
            return sized("image/jpeg", "jpg", be16(b, i + 7), be16(b, i + 5));
        }
        i += 2 + len;
    }
    return std::nullopt;
}

std::optional<ImageInfo> webp(std::string_view b) {
    if (b.size() < 30 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WEBP") return std::nullopt;
    const std::string_view chunk = b.substr(12, 4);
    if (chunk == "VP8 ") {
        // Lossy: a key frame's start code, then 14-bit width and height.
        if (byte_at(b, 23) != 0x9D || byte_at(b, 24) != 0x01 || byte_at(b, 25) != 0x2A) return std::nullopt;
        return sized("image/webp", "webp", le16(b, 26) & 0x3FFF, le16(b, 28) & 0x3FFF);
    }
    if (chunk == "VP8L") {
        // Lossless: a signature byte, then 14-bit width - 1 and height - 1.
        if (byte_at(b, 20) != 0x2F) return std::nullopt;
        const unsigned long bits = static_cast<unsigned long>(byte_at(b, 21)) | (static_cast<unsigned long>(byte_at(b, 22)) << 8) |
                                   (static_cast<unsigned long>(byte_at(b, 23)) << 16) | (static_cast<unsigned long>(byte_at(b, 24)) << 24);
        return sized("image/webp", "webp", (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1);
    }
    if (chunk == "VP8X") return sized("image/webp", "webp", le24(b, 24) + 1, le24(b, 27) + 1);  // extended: 24-bit sizes - 1
    return std::nullopt;
}

}  // namespace

std::optional<ImageInfo> image_info(std::string_view bytes) {
    if (auto i = png(bytes)) return i;
    if (auto i = jpeg(bytes)) return i;
    return webp(bytes);
}

}  // namespace wb::tools
