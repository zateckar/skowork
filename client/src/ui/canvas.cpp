#include "ui/canvas.h"

#include <cmath>
#include <cstring>
#include <map>

#include "ui/bundles.h"

namespace wb::ui {

uint32_t paint_color(const Theme& t, Paint p) {
    switch (p) {
    case Paint::none: return 0;
    case Paint::background: return t.background;
    case Paint::surface: return t.surface;
    case Paint::text: return t.text;
    case Paint::text_secondary: return t.text_secondary;
    case Paint::border: return t.border;
    case Paint::accent: return t.accent;
    case Paint::accent_text: return t.accent_text;
    case Paint::selection: return t.selection;
    case Paint::selection_text: return t.selection_text;
    case Paint::link: return t.link;
    case Paint::focus: return t.focus;
    case Paint::error: return t.error;
    case Paint::code_background: return t.code_background;
    case Paint::surface_alt: return t.surface_alt;
    case Paint::warning: return t.warning;
    case Paint::success: return t.success;
    case Paint::info: return t.info;
    case Paint::error_background: return t.error_background;
    case Paint::warning_background: return t.warning_background;
    case Paint::info_background: return t.info_background;
    case Paint::control_border: return t.control_border;
    case Paint::accent_surface: return t.accent_surface;
    }
    return t.text;
}

// --- TextCache ----------------------------------------------------------------------------------------------

IDWriteTextLayout* TextCache::get(IDWriteTextFormat* format, int style_key, std::u16string_view text, float max_w, bool wrap) {
    uint32_t wbits = 0;
    std::memcpy(&wbits, &max_w, sizeof wbits);
    std::u16string key;
    key.reserve(text.size() + 4);
    key.push_back(static_cast<char16_t>(style_key | (wrap ? 0x100 : 0)));
    key.push_back(static_cast<char16_t>(wbits & 0xFFFF));
    key.push_back(static_cast<char16_t>(wbits >> 16));
    key.append(text);
    if (const auto it = map_.find(key); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->layout.get();
    }
    com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dw_->CreateTextLayout(reinterpret_cast<const wchar_t*>(text.data()), static_cast<UINT32>(text.size()), format,
                                     max_w, 1e6f, layout.put())))
        return nullptr;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        // Single-line text that does not fit its width ends with an ellipsis.
        com_ptr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(format, sign.put()))) {
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&t, sign.get());
        }
    }
    lru_.push_front({key, std::move(layout)});
    map_[lru_.front().key] = lru_.begin();
    while (lru_.size() > capacity_) {
        map_.erase(lru_.back().key);
        lru_.pop_back();
    }
    return lru_.front().layout.get();
}

void TextCache::clear() {
    map_.clear();
    lru_.clear();
}

// --- Ui -------------------------------------------------------------------------------------------------------

namespace {

bool has_family(IDWriteFactory* dw, const wchar_t* name) {
    com_ptr<IDWriteFontCollection> c;
    if (FAILED(dw->GetSystemFontCollection(c.put()))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(c->FindFamilyName(name, &index, &exists)) && exists;
}

const wchar_t* first_family(IDWriteFactory* dw, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
        if (has_family(dw, n)) return n;
    return *names.begin();
}

}  // namespace

Result<std::unique_ptr<Ui>> Ui::create(IDWriteFactory* dw, std::string_view lang) {
    std::unique_ptr<Ui> ui(new Ui(dw));
    ui->strings = load_bundle(lang);
    ui->theme = current_theme();
    const std::wstring locale = ui->strings.lang() == "cs" ? L"cs-CZ" : ui->strings.lang() == "de" ? L"de-DE" : L"en-US";
    const wchar_t* text_family = first_family(dw, {L"Segoe UI Variable Text", L"Segoe UI"});
    ui->text_family_ = text_family;
    const wchar_t* display_family = first_family(dw, {L"Segoe UI Variable Display", L"Segoe UI"});
    ui->code_family_ = first_family(dw, {L"Cascadia Mono", L"Consolas"});
    const wchar_t* icon_family = first_family(dw, {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    struct Spec {
        const wchar_t* family;
        DWRITE_FONT_WEIGHT weight;
        float size;
    };
    // The Windows 11 type ramp (D7).
    const Spec specs[] = {
        {text_family, DWRITE_FONT_WEIGHT_NORMAL, 14},       // body
        {text_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14},    // body_strong
        {text_family, DWRITE_FONT_WEIGHT_NORMAL, 12},       // caption
        {display_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20}, // subtitle
        {display_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 28}, // title
        {display_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20}, // heading1
        {display_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 17}, // heading2
        {text_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15},    // heading3
        {ui->code_family_.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 13},  // code
        {icon_family, DWRITE_FONT_WEIGHT_NORMAL, 16},       // icon
        {icon_family, DWRITE_FONT_WEIGHT_NORMAL, 10},       // caption_icon (the window buttons)
    };
    for (size_t i = 0; i < std::size(specs); ++i) {
        const HRESULT hr = dw->CreateTextFormat(specs[i].family, nullptr, specs[i].weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, specs[i].size, locale.c_str(), ui->formats_[i].put());
        if (FAILED(hr)) return make_error("ui", "CreateTextFormat failed");
    }
    return ui;
}

IDWriteTextLayout* Ui::layout(std::u16string_view text, TextStyle s, float max_w, bool wrap) {
    return texts_.get(format(s), static_cast<int>(s), text, std::max(1.f, max_w), wrap);
}

Size Ui::measure_text(std::u16string_view text, TextStyle s, float max_w, bool wrap) {
    IDWriteTextLayout* l = layout(text, s, max_w, wrap);
    if (!l) return {};
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return {std::ceil(m.widthIncludingTrailingWhitespace), std::ceil(m.height)};
}

// --- Canvas -----------------------------------------------------------------------------------------------------

Canvas::Canvas(Renderer& r, Ui& ui) : r_(r), ui_(ui) {}

ID2D1SolidColorBrush* Canvas::brush(uint32_t rgb, float alpha) {
    const uint64_t key = (static_cast<uint64_t>(rgb) << 8) | static_cast<uint64_t>(std::lround(alpha * 255.f));
    auto& slot = r_.brushes()[key];
    if (!slot) dc()->CreateSolidColorBrush(color(rgb, alpha), slot.put());
    return slot.get();
}

void Canvas::fill(const Rect& r, Paint p, float alpha) {
    if (p == Paint::none) return;
    dc()->FillRectangle(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), brush(p, alpha));
}

void Canvas::round(const Rect& r, float radius, Paint fill, Paint stroke, float alpha) {
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(r.x, r.y, r.right(), r.bottom()), radius, radius};
    if (fill != Paint::none) dc()->FillRoundedRectangle(rr, brush(fill, alpha));
    if (stroke != Paint::none) {
        const D2D1_ROUNDED_RECT in{D2D1::RectF(r.x + 0.5f, r.y + 0.5f, r.right() - 0.5f, r.bottom() - 0.5f), radius, radius};
        dc()->DrawRoundedRectangle(in, brush(stroke), 1.f);
    }
}

void Canvas::line(float x1, float y1, float x2, float y2, Paint p, float width) {
    dc()->DrawLine({x1, y1}, {x2, y2}, brush(p), width);
}

void Canvas::arc(float cx, float cy, float r, float width, Paint p, double fraction) {
    if (fraction <= 0) return;
    if (fraction >= 1) {
        dc()->DrawEllipse(D2D1::Ellipse({cx, cy}, r, r), brush(p), width);
        return;
    }
    com_ptr<ID2D1Factory> factory;
    dc()->GetFactory(factory.put());
    com_ptr<ID2D1PathGeometry> path;
    if (!factory || FAILED(factory->CreatePathGeometry(path.put()))) return;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;
    const double a = fraction * 2 * 3.14159265358979323846;  // clockwise from the top
    const D2D1_POINT_2F end{cx + r * static_cast<float>(std::sin(a)), cy - r * static_cast<float>(std::cos(a))};
    sink->BeginFigure({cx, cy - r}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  fraction > 0.5 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    dc()->DrawGeometry(path.get(), brush(p), width);
}

void Canvas::text(IDWriteTextLayout* layout, float x, float y, Paint p) {
    if (layout) dc()->DrawTextLayout({x, y}, layout, brush(p), D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void Canvas::focus_ring(const Rect& r, float radius) {
    // Two rings (focus colour outside, background inside) stay visible on any background (D7).
    const Rect outer = r.inset(-2.f);
    const D2D1_ROUNDED_RECT o{D2D1::RectF(outer.x + 1, outer.y + 1, outer.right() - 1, outer.bottom() - 1), radius + 2, radius + 2};
    dc()->DrawRoundedRectangle(o, brush(Paint::focus), 2.f);
    const D2D1_ROUNDED_RECT i{D2D1::RectF(r.x + 0.5f, r.y + 0.5f, r.right() - 0.5f, r.bottom() - 0.5f), radius, radius};
    dc()->DrawRoundedRectangle(i, brush(Paint::background), 1.f);
}

void Canvas::push_clip(const Rect& r) {
    dc()->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::pop_clip() { dc()->PopAxisAlignedClip(); }

// --- paint ----------------------------------------------------------------------------------------------------

namespace {
void draw_node(Node& n, Canvas& c, const Rect& region, const Node* focused) {
    if (!n.visible) return;
    const bool clip = n.clips || n.scrolls;
    if (n.frame().intersect(region).empty() && n.parent()) return;
    n.draw(c);
    const Rect sub = clip ? region.intersect(n.frame()) : region;
    if (!sub.empty()) {
        if (clip) c.push_clip(sub);
        for (const auto& ch : n.children()) draw_node(*ch, c, sub, focused);
        if (clip) c.pop_clip();
    }
    n.draw_over(c);
    if (&n == focused) {
        const Rect fr = n.focus_rect();
        if (!fr.empty()) c.focus_ring(fr);
    }
}
}  // namespace

void paint(Tree& tree, Canvas& canvas, const std::vector<Rect>& regions) {
    for (const Rect& r : regions) {
        // Grow by the focus ring's reach so rings around nodes next to the region are redrawn whole.
        canvas.push_clip(r);
        canvas.fill(r, Paint::background);
        draw_node(tree.root(), canvas, r.inset(-4.f), tree.focused());
        canvas.pop_clip();
    }
}

}  // namespace wb::ui
