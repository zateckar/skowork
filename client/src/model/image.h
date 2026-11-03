// Image downscaling for vision requests (client/model-access Image Input): longest side 1568 px and at most 5 MB
// per image by default. Uses the Windows Imaging Component, so no image library is vendored.
#pragma once

#include "base/result.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wb::model {

struct ImageLimits {
    int max_side = 1568;
    size_t max_bytes = 5u * 1024 * 1024;
};

struct PreparedImage {
    std::string mime;  // image/png or image/jpeg
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    bool resized = false;  // false: the original bytes were already within the limits and pass through
};

// The size that fits `max_side` on the longest side, preserving the aspect ratio (never enlarges; at least 1 px).
std::pair<int, int> fit_within(int width, int height, int max_side);

// Decodes any format WIC knows (PNG, JPEG, BMP, GIF, TIFF, WebP when the codec is installed), downscales when needed
// and re-encodes as PNG, or as JPEG when the PNG would exceed max_bytes. Error codes: "decode", "encode", "too_large".
Result<PreparedImage> prepare_image(const std::vector<std::uint8_t>& data, const ImageLimits& limits = {});

}  // namespace wb::model
