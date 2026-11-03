#include <windows.h>

#include "ui/bundles.h"
#include "ui/text16.h"

namespace wb::ui {

std::string system_language() {
    ULONG count = 0, size = 0;
    if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) return "en";
    std::wstring buf(size, L'\0');
    if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) return "en";
    // The first entry of the double-NUL-terminated list.
    const std::wstring first(buf.c_str());
    return to_utf8(std::u16string(first.begin(), first.end()));
}

}  // namespace wb::ui
