// Screenshots for computer use (client/computer-use Screenshot Perception; add-computer-use design D2): a window through
// Windows.Graphics.Capture (PrintWindow with full content when that is unavailable), a screen region from the
// composed desktop. Pixels stay in memory; only the encoded image goes on.
#pragma once

#include "base/result.h"
#include "computer/uia.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wb::computer {

struct Bitmap {
    int width = 0, height = 0;
    std::vector<std::uint8_t> bgra;  // top-down, 4 bytes a pixel
    Box box;                         // the screen area it shows, physical pixels
    std::string method;              // graphics-capture | print-window | screen
};

Result<Bitmap> capture_window(HWND hwnd);
Result<Bitmap> capture_region(const Box& box);
// PNG through the Windows Imaging Component.
Result<std::vector<std::uint8_t>> encode_png(const Bitmap& b);

}  // namespace wb::computer
