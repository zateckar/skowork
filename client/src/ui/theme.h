// Look of the native window (add-native-ui design D7, improve-desktop-look D1): light and dark follow the Windows app
// mode with the brand colours Emerald and Electric Green as the accent; a high-contrast theme uses the system colours
// only and turns off custom colours and shadows. Colours are 0xRRGGBB.
#pragma once

#include <windows.h>

#include <cstdint>

namespace wb::ui {

struct Theme {
    bool dark = false;
    bool high_contrast = false;
    bool shadows = true;
    uint32_t background = 0;      // window
    uint32_t surface = 0;         // panes, cards
    uint32_t text = 0;
    uint32_t text_secondary = 0;
    uint32_t border = 0;          // dividers and card edges
    uint32_t control_border = 0;  // the boundary of buttons, fields and checkboxes (3:1 against the surface)
    uint32_t accent = 0;          // primary buttons, selection marker
    uint32_t accent_text = 0;     // text on accent
    uint32_t selection = 0;       // selected list row
    uint32_t selection_text = 0;
    uint32_t link = 0;
    uint32_t focus = 0;           // keyboard focus ring
    uint32_t error = 0;
    uint32_t code_background = 0;
    uint32_t surface_alt = 0;  // navigation pane, group headers, hover
    uint32_t accent_surface = 0;  // a surface tinted with the accent (the user's own messages)
    uint32_t warning = 0;      // Action indicators, warnings
    uint32_t success = 0;
    uint32_t info = 0;
    // Banner backgrounds by severity (client/work-inbox Error Banners With Remedy, status notices).
    uint32_t error_background = 0, warning_background = 0, info_background = 0;
};

// The system colours a high-contrast theme takes (GetSysColor, as 0xRRGGBB).
struct SystemColors {
    uint32_t window, window_text, highlight, highlight_text, hot_light, gray_text, button_face, button_text;
};

// Pure: the theme for an app mode, a high-contrast flag and the system colours.
Theme make_theme(bool dark, bool high_contrast, const SystemColors& sys);

// The theme Windows asks for now (app mode, high contrast).
Theme current_theme();

// True for the messages after which current_theme() may differ (app mode, high contrast, system colours).
bool is_theme_change(UINT msg, WPARAM wp, LPARAM lp);

// Dark or light title bar to match the theme.
void apply_title_bar(HWND hwnd, const Theme& theme);

}  // namespace wb::ui
