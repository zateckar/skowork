// UTF-8 <-> UTF-16 for the UI (add-native-ui): DirectWrite indexes text in UTF-16 code units, so the portable UI
// core keeps text as std::u16string. Invalid UTF-8 becomes U+FFFD.
#pragma once

#include <string>
#include <string_view>

namespace wb::ui {

std::u16string to_utf16(std::string_view utf8);
std::string to_utf8(std::u16string_view utf16);

}  // namespace wb::ui
