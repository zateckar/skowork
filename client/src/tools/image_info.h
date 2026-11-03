// The type and pixel size of an image from its header (add-image-generation design D4): PNG, JPEG and WebP only.
// Portable and bounds-checked, because the bytes come from outside the client; nothing is decoded.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wb::tools {

struct ImageInfo {
    std::string mime;  // image/png, image/jpeg or image/webp
    std::string extension;  // png, jpg or webp
    int width = 0;
    int height = 0;
};

// The image's type and size, or nothing when the bytes are not a PNG, JPEG or WebP image with a readable size.
std::optional<ImageInfo> image_info(std::string_view bytes);

}  // namespace wb::tools
