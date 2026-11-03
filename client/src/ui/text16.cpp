#include "ui/text16.h"

namespace wb::ui {

std::u16string to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        size_t len = 1;
        auto cont = [&](size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
        auto at = [&](size_t k) { return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3F); };
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xC2 && c < 0xE0 && cont(1)) {
            cp = (static_cast<char32_t>(c & 0x1F) << 6) | at(1);
            len = 2;
        } else if (c >= 0xE0 && c < 0xF0 && cont(1) && cont(2)) {
            const char32_t v = (static_cast<char32_t>(c & 0x0F) << 12) | (at(1) << 6) | at(2);
            if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) cp = v;  // no overlongs, no surrogates
            len = 3;
        } else if (c >= 0xF0 && c < 0xF5 && cont(1) && cont(2) && cont(3)) {
            const char32_t v = (static_cast<char32_t>(c & 0x07) << 18) | (at(1) << 12) | (at(2) << 6) | at(3);
            if (v >= 0x10000 && v <= 0x10FFFF) cp = v;
            len = 4;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string to_utf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // unpaired surrogate
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

}  // namespace wb::ui
