// Drawing services of the native window (add-native-ui design D2, D7): text formats, the text-layout cache, theme
// colours, brushes, and the paint traversal of the element tree. Text layouts are device-independent and cached by
// text, style and width; brushes are device resources cached on the renderer.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

#include "platform/com.h"
#include "ui/device.h"
#include "ui/layout.h"
#include "ui/strings.h"
#include "ui/theme.h"

namespace wb::ui {

class TextField;
class MarkdownView;

enum class TextStyle { body, body_strong, caption, subtitle, title, heading1, heading2, heading3, code, icon, caption_icon, count_ };

// Theme colours by purpose, so elements follow theme changes without rebuilding.
enum class Paint {
    none, background, surface, text, text_secondary, border, accent, accent_text, selection, selection_text, link, focus,
    error, code_background, surface_alt, warning, success, info, error_background, warning_background, info_background,
    control_border, accent_surface
};
uint32_t paint_color(const Theme& t, Paint p);

// Text layouts keyed by style, text and width, least recently used first out.
class TextCache {
public:
    explicit TextCache(IDWriteFactory* dw, size_t capacity = 1024) : dw_(dw), capacity_(capacity) {}
    // A layout of the text in the format, wrapped at max_w (or not wrapped). Owned by the cache.
    IDWriteTextLayout* get(IDWriteTextFormat* format, int style_key, std::u16string_view text, float max_w, bool wrap);
    void clear();
    size_t size() const { return map_.size(); }

private:
    struct Entry {
        std::u16string key;
        com_ptr<IDWriteTextLayout> layout;
    };
    IDWriteFactory* dw_;
    size_t capacity_;
    std::list<Entry> lru_;
    std::unordered_map<std::u16string, std::list<Entry>::iterator> map_;
};

// Shared services for elements: fonts, theme, strings, and the window's actions.
class Ui {
public:
    static Result<std::unique_ptr<Ui>> create(IDWriteFactory* dw, std::string_view lang);

    IDWriteFactory* dwrite() const { return dw_; }
    IDWriteTextFormat* format(TextStyle s) const { return formats_[static_cast<size_t>(s)].get(); }
    const std::wstring& code_family() const { return code_family_; }
    const std::wstring& text_family() const { return text_family_; }
    TextCache& texts() { return texts_; }
    // The size of a text in a style; wrapped at max_w when wrap is true.
    Size measure_text(std::u16string_view text, TextStyle s, float max_w, bool wrap = true);
    IDWriteTextLayout* layout(std::u16string_view text, TextStyle s, float max_w, bool wrap = true);

    Theme theme;
    Strings strings;
    uint32_t color(Paint p) const { return paint_color(theme, p); }

    // Actions the window provides. open_link shows the target and asks before opening it in the browser.
    std::function<void(const std::u16string& url)> open_link;
    std::function<void(const std::u16string& text)> copy_text;
    std::function<void(const std::u16string& text)> announce;

    // The window that hosts child-window elements (text fields) and its scale; null off screen (tests).
    HWND host = nullptr;
    float scale = 1.f;
    // Handles a key as if the window had received it (keys a text field passes on: Tab, Escape, shortcuts).
    std::function<bool(const KeyEvent&)> dispatch_key;
    // The text fields of this window, for the window's placement and command routing.
    std::vector<TextField*> fields;
    // The message view that holds the window's text selection (improve-desktop-look D5), or null.
    MarkdownView* selection = nullptr;

private:
    explicit Ui(IDWriteFactory* dw) : dw_(dw), texts_(dw) {}
    IDWriteFactory* dw_;
    std::array<com_ptr<IDWriteTextFormat>, static_cast<size_t>(TextStyle::count_)> formats_;
    std::wstring code_family_, text_family_;
    TextCache texts_;
};

// One frame's drawing surface.
class Canvas {
public:
    Canvas(Renderer& r, Ui& ui);
    Renderer& renderer() { return r_; }
    Ui& ui() { return ui_; }
    ID2D1DeviceContext* dc() const { return r_.dc(); }
    uint64_t generation() const { return r_.generation(); }
    uint64_t device_id() const { return r_.device_id(); }

    ID2D1SolidColorBrush* brush(uint32_t rgb, float alpha = 1.f);
    ID2D1SolidColorBrush* brush(Paint p, float alpha = 1.f) { return brush(ui_.color(p), alpha); }
    void fill(const Rect& r, Paint p, float alpha = 1.f);
    void round(const Rect& r, float radius, Paint fill, Paint stroke = Paint::none, float alpha = 1.f);
    void line(float x1, float y1, float x2, float y2, Paint p, float width = 1.f);
    // A circular arc clockwise from the top covering `fraction` of the circle (a whole circle at 1).
    void arc(float cx, float cy, float r, float width, Paint p, double fraction);
    void text(IDWriteTextLayout* layout, float x, float y, Paint p);
    void focus_ring(const Rect& r, float radius = 4.f);
    void push_clip(const Rect& r);
    void pop_clip();

private:
    Renderer& r_;
    Ui& ui_;
};

// Paints the tree into the regions (DIPs) the renderer asked for: each node's draw(), its children (clipped where
// the node clips or scrolls), draw_over(), and the focus ring of the focused node.
void paint(Tree& tree, Canvas& canvas, const std::vector<Rect>& regions);

}  // namespace wb::ui
