#include "ui/theme.h"

#include <dwmapi.h>

#include <cwchar>

namespace wb::ui {
namespace {

uint32_t rgb_of(COLORREF c) { return (static_cast<uint32_t>(GetRValue(c)) << 16) | (static_cast<uint32_t>(GetGValue(c)) << 8) | GetBValue(c); }

// The brand colours (improve-desktop-look D1).
constexpr uint32_t kEmerald = 0x0E3A2F, kElectricGreen = 0x78FAAE;

}  // namespace

Theme make_theme(bool dark, bool high_contrast, const SystemColors& sys) {
    Theme t;
    t.dark = dark;
    t.high_contrast = high_contrast;
    if (high_contrast) {
        t.shadows = false;
        t.background = t.surface = t.code_background = sys.window;
        t.text = t.error = sys.window_text;
        t.text_secondary = sys.gray_text;
        t.border = t.control_border = sys.window_text;
        t.accent = t.selection = sys.highlight;
        t.accent_text = t.selection_text = sys.highlight_text;
        t.link = sys.hot_light;
        t.focus = sys.window_text;
        t.surface_alt = t.accent_surface = sys.window;
        t.warning = t.success = t.info = sys.window_text;
        t.error_background = t.warning_background = t.info_background = sys.window;
        return t;
    }
    // Neutrals with a slight green tint, so they sit with the accent. The contrast levels (text 4.5:1, control
    // boundaries and focus 3:1) are checked by the UiTheme tests.
    if (dark) {
        t.background = 0x131A17;
        t.surface = 0x1B2420;
        t.surface_alt = 0x17201C;
        t.accent_surface = 0x1E2E27;
        t.code_background = 0x0F1513;
        t.text = 0xF2F5F4;
        t.text_secondary = 0xB3C0BB;
        t.border = 0x2C3833;
        t.control_border = 0x6E7D77;
        t.accent = kElectricGreen;
        t.accent_text = kEmerald;
        t.selection = 0x1F4536;
        t.selection_text = 0xFFFFFF;
        t.link = kElectricGreen;
        t.focus = kElectricGreen;
        t.error = 0xFF9AA2;
        t.warning = 0xF5C94B;
        t.success = kElectricGreen;
        t.info = 0x8CC8FF;
        t.error_background = 0x3D2224;
        t.warning_background = 0x3A3018;
        t.info_background = 0x1B2E3D;
    } else {
        t.background = 0xF1F5F3;
        t.surface = 0xFFFFFF;
        t.surface_alt = 0xE6EEEA;
        t.accent_surface = 0xE6F7EE;
        t.code_background = 0xF1F5F3;
        t.text = 0x0E1F19;
        t.text_secondary = 0x46564F;
        t.border = 0xD5DFDA;
        t.control_border = 0x7A8A84;
        t.accent = kEmerald;
        t.accent_text = 0xFFFFFF;
        t.selection = 0xD3F9E4;  // an Electric Green tint
        t.selection_text = 0x0E1F19;
        t.link = 0x0A6B47;
        t.focus = kEmerald;
        t.error = 0xB3261E;
        t.warning = 0x8A5300;
        t.success = 0x0A6B47;
        t.info = 0x005A9E;
        t.error_background = 0xFDE8E8;
        t.warning_background = 0xFFF3D6;
        t.info_background = 0xE3F0FB;
    }
    return t;
}

Theme current_theme() {
    DWORD light = 1, size = sizeof light;
    ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof hc;
    const bool high = ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    const SystemColors sys{rgb_of(::GetSysColor(COLOR_WINDOW)),     rgb_of(::GetSysColor(COLOR_WINDOWTEXT)),
                           rgb_of(::GetSysColor(COLOR_HIGHLIGHT)),  rgb_of(::GetSysColor(COLOR_HIGHLIGHTTEXT)),
                           rgb_of(::GetSysColor(COLOR_HOTLIGHT)),   rgb_of(::GetSysColor(COLOR_GRAYTEXT)),
                           rgb_of(::GetSysColor(COLOR_BTNFACE)),    rgb_of(::GetSysColor(COLOR_BTNTEXT))};
    return make_theme(light == 0, high, sys);
}
bool is_theme_change(UINT msg, WPARAM, LPARAM lp) {
    if (msg == WM_SYSCOLORCHANGE || msg == WM_THEMECHANGED) return true;
    if (msg == WM_SETTINGCHANGE && lp) {
        const auto* area = reinterpret_cast<const wchar_t*>(lp);
        return std::wcscmp(area, L"ImmersiveColorSet") == 0 || std::wcscmp(area, L"WindowsThemeElement") == 0;
    }
    return false;
}

void apply_title_bar(HWND hwnd, const Theme& theme) {
    const BOOL dark = theme.dark && !theme.high_contrast;
    constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 11)
    ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof dark);
}

}  // namespace wb::ui
