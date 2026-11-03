#include "ui/elements.h"

#include <algorithm>
#include <cmath>

namespace wb::ui {

namespace {
constexpr float kControlHeight = 32.f;
constexpr float kRadius = 4.f;

std::u16string glyph_text(char16_t g) { return std::u16string(1, g); }

// Hover and press tints over a fill.
void state_fill(Canvas& c, const Rect& r, Paint base, bool hovered, bool pressed, float radius = kRadius) {
    if (base != Paint::none) c.round(r, radius, base);
    if (pressed) c.round(r, radius, Paint::text, Paint::none, 0.12f);
    else if (hovered) c.round(r, radius, Paint::text, Paint::none, 0.06f);
}
}  // namespace

// --- Box ----------------------------------------------------------------------------------------------------

Box::Box(std::string id, Paint bg, float r, Paint b) : Node(std::move(id)), background(bg), border(b), radius(r) {}

void Box::draw(Canvas& c) {
    if (background == Paint::none && border == Paint::none) return;
    if (radius > 0) c.round(frame(), radius, background, border);
    else {
        c.fill(frame(), background);
        if (border != Paint::none) c.round(frame(), 0, Paint::none, border);
    }
}

// --- Label --------------------------------------------------------------------------------------------------

Label::Label(Ui& ui, std::u16string text, TextStyle style, Paint col, bool wrap, std::string id)
    : Node(std::move(id)), color(col), ui_(ui), text_(std::move(text)), style_(style), wrap_(wrap) {
    role = Role::text;
    name = text_;
    if (!wrap_) width = Sizing::fit;
    measure = [this](float w) {
        if (wrap_) {
            const Size s = ui_.measure_text(text_, style_, w, true);
            return Size{w, s.h};
        }
        return ui_.measure_text(text_, style_, 100000.f, false);
    };
}

void Label::set_text(std::u16string text) {
    if (text == text_) return;
    text_ = std::move(text);
    name = text_;
    relayout();
    invalidate();
}

void Label::draw(Canvas& c) {
    const Rect& f = frame();
    const Rect in{f.x + padding.left, f.y + padding.top, f.w - padding.left - padding.right, f.h - padding.top - padding.bottom};
    c.text(ui_.layout(text_, style_, in.w, wrap_), in.x, in.y, enabled ? color : Paint::text_secondary);
}

// --- Button -------------------------------------------------------------------------------------------------

Button::Button(Ui& ui, std::string id, std::u16string label, Kind kind, char16_t glyph)
    : Node(std::move(id)), ui_(ui), label_(std::move(label)), kind_(kind), glyph_(glyph) {
    role = Role::button;
    focusable = true;
    name = label_;
    width = Sizing::fit;
    measure = [this](float) {
        float w = 0;
        if (glyph_) w += 16;
        if (!label_.empty()) w += (glyph_ ? 8 : 0) + ui_.measure_text(label_, TextStyle::body, 100000.f, false).w;
        return Size{label_.empty() ? kControlHeight - 2 * 8 : w, kControlHeight - 2 * 5};
    };
    padding = label_.empty() ? Insets::xy(8, 5) : Insets::xy(12, 5);
}

void Button::make_inline() {
    text_style_ = TextStyle::caption;
    padding = Insets::xy(4, 0);
    measure = [this](float) { return ui_.measure_text(label_, text_style_, 100000.f, false); };
    relayout();
}

void Button::set_label(std::u16string label) {
    label_ = std::move(label);
    name = label_;
    relayout();
    invalidate();
}

void Button::invoke() {
    // A handler often closes or rebuilds what holds this button (a dialog's buttons, a list's rows), which destroys
    // on_click while it runs; it runs from a copy.
    if (!on_click) return;
    const std::function<void()> f = on_click;
    f();
}

void Button::draw(Canvas& c) {
    const Rect& f = frame();
    const Paint fill = kind_ == Kind::primary ? Paint::accent : kind_ == Kind::standard ? Paint::surface : Paint::none;
    const Paint stroke = kind_ == Kind::standard && !ui_.theme.high_contrast ? Paint::control_border
                         : ui_.theme.high_contrast && kind_ != Kind::subtle ? Paint::text
                                                                            : Paint::none;
    if (enabled) state_fill(c, f, fill, hovered_, pressed_);
    else if (fill != Paint::none) c.round(f, kRadius, fill, Paint::none, 0.4f);
    if (stroke != Paint::none) c.round(f, kRadius, Paint::none, stroke);
    const Paint ink = !enabled ? Paint::text_secondary : kind_ == Kind::primary ? Paint::accent_text : Paint::text;
    float x = f.x + padding.left;
    const float cy = f.y + f.h / 2;
    if (glyph_) {
        IDWriteTextLayout* g = ui_.layout(glyph_text(glyph_), TextStyle::icon, 100.f, false);
        DWRITE_TEXT_METRICS m{};
        if (g) g->GetMetrics(&m);
        c.text(g, label_.empty() ? f.x + (f.w - m.width) / 2 : x, cy - m.height / 2, ink);
        x += 16 + 8;
    }
    if (!label_.empty()) {
        IDWriteTextLayout* t = ui_.layout(label_, text_style_, std::max(1.f, f.right() - padding.right - x), false);
        DWRITE_TEXT_METRICS m{};
        if (t) t->GetMetrics(&m);
        c.text(t, x, cy - m.height / 2, ink);
    }
}

IconButton::IconButton(Ui& ui, std::string id, char16_t glyph, std::u16string tip, Kind kind)
    : Button(ui, std::move(id), {}, kind, glyph) {
    name = tip;
    help = std::move(tip);  // shown as the tooltip
    width = Sizing::fixed;
    height = Sizing::fixed;
    fixed_w = fixed_h = kControlHeight;
}

// --- Checkbox -----------------------------------------------------------------------------------------------

Checkbox::Checkbox(Ui& ui, std::string id, std::u16string label, bool checked)
    : Node(std::move(id)), ui_(ui), label_(std::move(label)), checked_(checked) {
    role = Role::checkbox;
    focusable = true;
    name = label_;
    width = Sizing::fit;
    padding = Insets::xy(0, 4);
    measure = [this](float) {
        const Size t = ui_.measure_text(label_, TextStyle::body, 100000.f, false);
        return Size{20 + 8 + t.w, std::max(20.f, t.h)};
    };
}

void Checkbox::set_checked(bool on) {
    if (on == checked_) return;
    checked_ = on;
    invalidate();
    if (on_change) on_change(on);
}

void Checkbox::draw(Canvas& c) {
    const Rect& f = frame();
    const Rect box{f.x, f.y + (f.h - 20) / 2, 20, 20};
    if (checked_ && !enabled) {  // checked and fixed (a member already): muted, not the accent
        c.round(box, kRadius, Paint::surface_alt, Paint::control_border);
        c.text(ui_.layout(glyph_text(icon::check), TextStyle::icon, 100.f, false), box.x + 2, box.y + 1, Paint::text_secondary);
    } else if (checked_) {
        state_fill(c, box, Paint::accent, hovered_, pressed_);
        IDWriteTextLayout* g = ui_.layout(glyph_text(icon::check), TextStyle::icon, 100.f, false);
        c.text(g, box.x + 2, box.y + 1, Paint::accent_text);
    } else {
        state_fill(c, box, Paint::surface, hovered_, pressed_);
        c.round(box, kRadius, Paint::none, Paint::control_border);
    }
    IDWriteTextLayout* t = ui_.layout(label_, TextStyle::body, std::max(1.f, f.w - 28), false);
    DWRITE_TEXT_METRICS m{};
    if (t) t->GetMetrics(&m);
    c.text(t, f.x + 28, f.y + (f.h - m.height) / 2, enabled ? Paint::text : Paint::text_secondary);
}

// --- Card ---------------------------------------------------------------------------------------------------

class Card::Header : public Node {
public:
    Header(Ui& ui, Card& card, std::u16string title, bool expandable)
        : Node(card.automation_id() + "-header"), ui_(ui), card_(card), expandable_(expandable) {
        axis = Axis::row;
        gap = 8;
        padding = Insets::xy(12, 8);
        name = title;
        if (expandable) {
            role = Role::button;
            focusable = true;
        }
        title_ = emplace<Label>(ui, std::move(title), TextStyle::body_strong, Paint::text, true);
        title_->role = Role::none;  // the header itself carries the name
    }
    int expand_state() const override { return expandable_ ? (card_.expanded() ? 1 : 0) : -1; }
    void set_expanded(bool on) override { card_.set_expanded_state(on); }
    void draw(Canvas& c) override {
        if (!expandable_) return;
        if (hovered_ || pressed_) c.round(frame(), 6, Paint::text, Paint::none, pressed_ ? 0.08f : 0.04f);
        const char16_t g = card_.expanded() ? icon::chevron_up : icon::chevron_down;
        IDWriteTextLayout* l = ui_.layout(std::u16string(1, g), TextStyle::icon, 100.f, false);
        c.text(l, frame().right() - 12 - 16, frame().y + padding.top + 1, Paint::text_secondary);  // in the room kept for it
    }

private:
    Ui& ui_;
    Card& card_;
    bool expandable_;
    Label* title_;
};

Card::Card(Ui& ui, std::string id, std::u16string title, bool expandable, bool expanded)
    : Box(std::move(id), Paint::surface, 8, Paint::border) {
    role = Role::group;
    name = title;
    auto h = std::make_unique<Header>(ui, *this, std::move(title), expandable);
    if (expandable) h->padding.right += 24;  // room for the chevron
    header_ = add(std::move(h));
    body_ = emplace<Node>(automation_id() + "-body");
    body_->padding = {12, 0, 12, 12};
    body_->gap = 8;
    body_->visible = expanded || !expandable;
}

void Card::set_expanded_state(bool on) {
    if (on == body_->visible) return;
    body_->visible = on;
    relayout();
    invalidate();
    header_->invalidate();
    if (on_expand) on_expand(on);
}

// --- Badge --------------------------------------------------------------------------------------------------

Badge::Badge(Ui& ui, std::u16string text, Paint fill, Paint color, std::string id)
    : Node(std::move(id)), ui_(ui), text_(std::move(text)), fill_(fill), color_(color) {
    role = Role::text;
    name = text_;
    width = Sizing::fit;
    padding = Insets::xy(6, 1);
    measure = [this](float) {
        const Size s = ui_.measure_text(text_, TextStyle::caption, 100000.f, false);
        return Size{std::max(s.w, 8.f), s.h};
    };
}

void Badge::set_text(std::u16string text) {
    text_ = std::move(text);
    name = text_;
    relayout();
    invalidate();
}

void Badge::draw(Canvas& c) {
    const Rect& f = frame();
    c.round(f, f.h / 2, fill_, ui_.theme.high_contrast ? Paint::text : Paint::none);
    c.text(ui_.layout(text_, TextStyle::caption, 100000.f, false), f.x + padding.left, f.y + padding.top, color_);
}

// --- Avatar -------------------------------------------------------------------------------------------------

Avatar::Avatar(Ui& ui, std::u16string_view who, Paint fill, Paint color, float size)
    : ui_(ui), text_(initials(who)), fill_(fill), color_(color) {
    width = Sizing::fixed;
    height = Sizing::fixed;
    fixed_w = fixed_h = size;
}

std::u16string Avatar::initials(std::u16string_view who) {
    std::u16string out;
    bool start = true;
    for (char16_t ch : who) {
        const bool sep = ch == u' ' || ch == u'.' || ch == u'_' || ch == u'-' || ch == u'@';
        if (ch == u'@') break;  // a mail address: its local part only
        if (sep) {
            start = true;
            continue;
        }
        if (start && out.size() < 2) out += (ch >= u'a' && ch <= u'z') ? static_cast<char16_t>(ch - 32) : ch;
        start = false;
    }
    return out.empty() ? u"?" : out;
}

void Avatar::draw(Canvas& c) {
    const Rect& f = frame();
    c.round(f, f.w / 2, fill_, ui_.theme.high_contrast ? Paint::text : Paint::border);
    IDWriteTextLayout* l = ui_.layout(text_, TextStyle::caption, 100000.f, false);
    if (!l) return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    c.text(l, f.x + (f.w - m.widthIncludingTrailingWhitespace) / 2, f.y + (f.h - m.height) / 2, color_);
}

// --- images -------------------------------------------------------------------------------------------------

Result<com_ptr<IWICBitmapSource>> decode_image(const void* data, size_t size, UINT max_side) {
    if (!data || size == 0 || size > 0x7FFFFFFF) return make_error("image", "no image data");
    com_ptr<IWICImagingFactory> wic;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, wic.put_void());
    if (FAILED(hr)) return make_error("image", "WIC unavailable");
    com_ptr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size))))
        return make_error("image", "stream");
    com_ptr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put())))
        return make_error("image", "unsupported image format");
    com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put()))) return make_error("image", "no frame");
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return make_error("image", "empty image");
    com_ptr<IWICBitmapSource> src;
    frame.as(src, IID_IWICBitmapSource);
    if (w > max_side || h > max_side) {
        const double k = static_cast<double>(max_side) / std::max(w, h);
        com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
            FAILED(scaler->Initialize(src.get(), std::max(1u, static_cast<UINT>(w * k)), std::max(1u, static_cast<UINT>(h * k)),
                                      WICBitmapInterpolationModeFant)))
            return make_error("image", "scale");
        scaler.as(src, IID_IWICBitmapSource);
    }
    com_ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return make_error("image", "convert");
    // Decode now, into memory owned by the bitmap, so the source no longer reads the caller's buffer.
    com_ptr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, bmp.put()))) return make_error("image", "decode");
    com_ptr<IWICBitmapSource> out;
    bmp.as(out, IID_IWICBitmapSource);
    return out;
}

ImageView::ImageView(std::u16string alt, com_ptr<IWICBitmapSource> image, float max_h, std::string id)
    : Node(std::move(id)), image_(std::move(image)), max_h_(max_h) {
    role = Role::image;
    name = std::move(alt);
    if (image_) image_->GetSize(&w_, &h_);
    width = Sizing::fit;
    measure = [this](float avail) {
        if (!w_ || !h_) return Size{};
        float w = std::min(static_cast<float>(w_), avail);
        float h = w * static_cast<float>(h_) / static_cast<float>(w_);
        if (h > max_h_) {
            h = max_h_;
            w = h * static_cast<float>(w_) / static_cast<float>(h_);
        }
        return Size{w, h};
    };
}

void ImageView::draw(Canvas& c) {
    if (!image_) return;
    if (device_ != c.device_id()) {
        bitmap_.reset();
        device_ = c.device_id();
        c.dc()->CreateBitmapFromWicBitmap(image_.get(), nullptr, bitmap_.put());
    }
    if (!bitmap_) return;
    const Rect& f = frame();
    c.dc()->DrawBitmap(bitmap_.get(), D2D1::RectF(f.x, f.y, f.right(), f.bottom()), 1.f,
                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

// --- ScrollView ---------------------------------------------------------------------------------------------

ScrollView::ScrollView(std::string id) : Node(std::move(id)) {
    scrolls = true;
    clips = true;
    role = Role::scroll_view;
    height = Sizing::fill;
}

Rect ScrollView::thumb() const {
    const Rect& f = frame();
    if (max_scroll() <= 0 || content_height() <= 0) return {};
    const float track = f.h - 4;
    const float h = std::max(24.f, track * f.h / content_height());
    const float y = f.y + 2 + (track - h) * scroll_y() / max_scroll();
    const float w = hovered_ || pressed_ ? 8.f : 4.f;
    return {f.right() - w - 2, y, w, h};
}

void ScrollView::draw_over(Canvas& c) {
    const Rect t = thumb();
    if (!t.empty()) c.round(t, t.w / 2, Paint::text_secondary, Paint::none, hovered_ || pressed_ ? 0.8f : 0.5f);
}

void ScrollView::on_hover(bool) { invalidate(); }

bool ScrollView::on_pointer(const PointerEvent& e) {
    Tree* t = tree();
    if (!t) return false;
    if (e.kind == PointerEvent::down && e.button == 0) {
        Rect hit = thumb();
        hit.x -= 6;
        hit.w += 8;
        if (hit.empty() || !hit.contains(e.x, e.y)) return false;
        drag_from_y_ = e.y;
        drag_from_scroll_ = scroll_y();
        invalidate();
        return true;
    }
    if (e.kind == PointerEvent::move && pressed_) {
        const Rect th = thumb();
        const float track = frame().h - 4 - th.h;
        if (track > 0) t->scroll_to(*this, drag_from_scroll_ + (e.y - drag_from_y_) * max_scroll() / track);
        return true;
    }
    return e.kind == PointerEvent::up && pressed_;
}

// --- Splitter -----------------------------------------------------------------------------------------------

Splitter::Splitter(std::string id, std::u16string nm, Node& target, float lo, float hi, bool target_left)
    : Node(std::move(id)), target_(target), min_w_(lo), max_w_(hi), target_left_(target_left) {
    role = Role::splitter;
    focusable = true;
    name = std::move(nm);
    width = Sizing::fixed;
    fixed_w = 6;
    height = Sizing::fill;
}

void Splitter::set_width(float w) {
    w = std::clamp(w, min_w_, max_w_);
    if (w == target_.fixed_w) return;
    target_.fixed_w = w;
    target_.relayout();
    if (on_resized) on_resized(w);
}

bool Splitter::on_pointer(const PointerEvent& e) {
    if (e.kind == PointerEvent::down && e.button == 0) {
        drag_x_ = e.x;
        drag_w_ = target_.fixed_w;
        invalidate();
        return true;
    }
    if (e.kind == PointerEvent::move && pressed_) {
        set_width(drag_w_ + (target_left_ ? 1.f : -1.f) * (e.x - drag_x_));
        return true;
    }
    return e.kind == PointerEvent::up && pressed_;
}

bool Splitter::on_key(const KeyEvent& e) {
    const float step = e.ctrl ? 64.f : 16.f;
    if (e.key == kKeyLeft) set_width(target_.fixed_w + (target_left_ ? -step : step));
    else if (e.key == kKeyRight) set_width(target_.fixed_w + (target_left_ ? step : -step));
    else return false;
    return true;
}

void Splitter::draw(Canvas& c) {
    const Rect& f = frame();
    const float x = std::floor(f.x + f.w / 2);
    c.line(x, f.y, x, f.bottom(), hovered_ || pressed_ ? Paint::accent : Paint::border, hovered_ || pressed_ ? 2.f : 1.f);
}

// --- overlays -----------------------------------------------------------------------------------------------

OverlayLayer::OverlayLayer() : Node("overlay") {
    floating = true;
    visible = false;
}

void OverlayLayer::show(std::unique_ptr<Node> popup, const Rect& anchor, bool take_focus) {
    Tree* t = tree();
    if (take_focus && t && t->focused() && restore_focus_ == 0) restore_focus_ = t->focused()->runtime_id();
    anchors_[popup.get()] = anchor;
    Node* p = add(std::move(popup));
    visible = true;
    relayout();
    if (take_focus && t) {
        t->layout_if_needed();
        for (Node* f : t->focus_chain()) {
            for (Node* a = f; a; a = a->parent())
                if (a == p) {
                    t->set_focus(f);
                    return;
                }
        }
    }
}

namespace {
Node* find_runtime(Node& n, uint32_t id) {
    if (n.runtime_id() == id) return &n;
    for (const auto& c : n.children())
        if (Node* f = find_runtime(*c, id)) return f;
    return nullptr;
}
}  // namespace

void OverlayLayer::close(Node* popup) {
    anchors_.erase(popup);
    Tree* t = tree();
    if (t) t->invalidate(popup->frame().inset(-8.f));  // with the shadow
    remove(popup);
    visible = has_popup();
    if (!has_popup() && restore_focus_ && t) {
        if (Node* f = find_runtime(t->root(), restore_focus_)) t->set_focus(f);
        restore_focus_ = 0;
    }
}

void OverlayLayer::close_all() {
    while (has_popup()) close(children().back().get());
}

bool OverlayLayer::arrange_children(Tree& tree, const Rect& in) {
    for (const auto& ch : children()) {
        const auto it = anchors_.find(ch.get());
        const Rect a = it == anchors_.end() ? Rect{} : it->second;
        const float avail = std::min(in.w - 16, ch->width == Sizing::fixed ? ch->fixed_w : 360.f);
        const Size s = tree.measure(*ch, std::max(40.f, avail));
        const float w = std::min(s.w, in.w - 16), h = std::min(s.h, in.h - 16);
        float x = std::clamp(a.x, in.x + 8, std::max(in.x + 8, in.right() - 8 - w));
        float y = a.bottom() + 4;
        if (y + h > in.bottom() - 8) y = std::max(in.y + 8, a.y - 4 - h);  // above the anchor when it does not fit below
        tree.place(*ch, {x, y, w, h});
    }
    return true;
}

bool OverlayLayer::on_pointer(const PointerEvent& e) {
    if (e.kind != PointerEvent::down) return false;
    for (const auto& ch : children())
        if (ch->frame().contains(e.x, e.y)) return false;  // inside a popup: its own controls handle it
    close_all();
    return true;  // a press outside closes the popups and goes no further
}

bool OverlayLayer::on_key(const KeyEvent& e) {
    if (e.key != kKeyEscape || !has_popup()) return false;
    close(children().back().get());
    return true;
}

Menu::Menu(std::string id, std::u16string nm) : Box(std::move(id), Paint::surface, 8, Paint::border) {
    role = Role::menu;
    name = std::move(nm);
    padding = Insets::all(4);
    width = Sizing::fit;
    min_w = 180;
}

MenuItem* Menu::add_item(Ui& ui, std::string id, std::u16string label, std::function<void()> action, char16_t glyph) {
    return emplace<MenuItem>(ui, std::move(id), std::move(label), std::move(action), glyph);
}

bool Menu::arrange_children(Tree& tree, const Rect& in) {
    float y = in.y;
    for (const auto& c : children()) {
        if (!c->visible) continue;
        const float h = tree.measure(*c, in.w).h;
        tree.place(*c, {in.x, y, in.w, h});
        y += h + gap;
    }
    return true;
}

bool Menu::on_key(const KeyEvent& e) {
    Tree* t = tree();
    if (!t || (e.key != kKeyDown && e.key != kKeyUp && e.key != kKeyHome && e.key != kKeyEnd)) return false;
    std::vector<Node*> items;
    for (const auto& c : children())
        if (c->focusable && c->enabled && c->visible) items.push_back(c.get());
    if (items.empty()) return true;
    auto it = std::find(items.begin(), items.end(), t->focused());
    size_t i = it == items.end() ? 0 : static_cast<size_t>(it - items.begin());
    if (e.key == kKeyDown) i = (i + 1) % items.size();
    else if (e.key == kKeyUp) i = (i + items.size() - 1) % items.size();
    else if (e.key == kKeyHome) i = 0;
    else i = items.size() - 1;
    t->set_focus(items[i]);
    return true;
}

MenuItem::MenuItem(Ui& ui, std::string id, std::u16string label, std::function<void()> action, char16_t glyph)
    : Node(std::move(id)), ui_(ui), label_(std::move(label)), action_(std::move(action)), glyph_(glyph) {
    role = Role::menu_item;
    focusable = true;
    name = label_;
    width = Sizing::fit;
    padding = Insets::xy(12, 6);
    measure = [this](float) {
        const Size t = ui_.measure_text(label_, TextStyle::body, 100000.f, false);
        return Size{t.w + 28, std::max(20.f, t.h)};
    };
}

void MenuItem::invoke() {
    auto action = action_;  // closing the menu destroys this item
    for (Node* p = parent(); p; p = p->parent())
        if (auto* o = dynamic_cast<OverlayLayer*>(p)) {
            Node* popup = this;
            while (popup->parent() != o) popup = popup->parent();
            o->close(popup);
            break;
        }
    if (action) action();
}

void MenuItem::draw(Canvas& c) {
    const Rect& f = frame();
    if (hovered_ || pressed_) c.round(f, kRadius, Paint::text, Paint::none, pressed_ ? 0.1f : 0.06f);
    if (glyph_) c.text(ui_.layout(std::u16string(1, glyph_), TextStyle::icon, 100.f, false), f.x + padding.left, f.y + padding.top + 2,
                       Paint::text);
    IDWriteTextLayout* t = ui_.layout(label_, TextStyle::body, std::max(1.f, f.w - padding.left - padding.right - 28), false);
    c.text(t, f.x + padding.left + 28, f.y + padding.top, enabled ? Paint::text : Paint::text_secondary);
}

Tooltip::Tooltip(Ui& ui, std::u16string text) : Box("tooltip", Paint::surface, 4, Paint::border) {
    role = Role::tooltip;
    name = text;
    padding = Insets::xy(8, 4);
    width = Sizing::fit;
    auto* l = emplace<Label>(ui, std::move(text), TextStyle::caption, Paint::text, false);
    l->role = Role::none;
}

// --- dialogs ------------------------------------------------------------------------------------------------

Dialog::Dialog(Ui& ui, std::string id, std::u16string title, float w) : Box(std::move(id), Paint::surface, 8, Paint::border), ui_(ui) {
    role = Role::dialog;
    name = title;
    width = Sizing::fixed;
    fixed_w = w;
    padding = Insets::all(24);
    gap = 16;
    auto* t = emplace<Label>(ui, std::move(title), TextStyle::subtitle, Paint::text, true, id_or(automation_id(), "-title"));
    t->role = Role::heading;
    body_ = emplace<Node>();
    body_->gap = 12;
    footer_ = emplace<Node>();
    footer_->axis = Axis::row;
    footer_->gap = 8;
    footer_->padding.top = 8;
    footer_->wraps = true;  // buttons that do not fit continue on the next line (improve-desktop-look D3)
    footer_->align_end = true;
}

std::string Dialog::id_or(const std::string& id, const char* suffix) { return id.empty() ? std::string{} : id + suffix; }

Button* Dialog::add_button(std::string id, std::u16string label, Button::Kind kind, std::function<void()> action) {
    auto* b = footer_->emplace<Button>(ui_, std::move(id), std::move(label), kind);
    b->on_click = std::move(action);
    return b;
}

void Dialog::close() {
    for (Node* p = parent(); p; p = p->parent())
        if (auto* m = dynamic_cast<ModalLayer*>(p)) {
            m->close(this);
            return;
        }
}

bool Dialog::on_key(const KeyEvent& e) {
    if (e.key != kKeyEscape) return false;
    if (on_cancel) on_cancel();
    else close();
    return true;
}

ModalLayer::ModalLayer() : Node("modal") {
    floating = true;
    visible = false;
}

Dialog* ModalLayer::show(std::unique_ptr<Dialog> d) {
    Tree* t = tree();
    restore_focus_.push_back(t && t->focused() ? t->focused()->runtime_id() : 0);
    auto* p = static_cast<Dialog*>(add(std::move(d)));
    visible = true;
    relayout();
    if (t) {
        t->invalidate_all();
        t->layout_if_needed();
        t->set_modal(p);
        // The keyboard starts inside the dialog, on its first control; the caller may move it (a text field).
        if (Node* first = t->next_focus(nullptr)) t->set_focus(first);
    }
    return p;
}

Dialog* ModalLayer::top() const { return children().empty() ? nullptr : static_cast<Dialog*>(children().back().get()); }

void ModalLayer::close(Dialog* d) {
    Tree* t = tree();
    remove(d);
    visible = !children().empty();
    const uint32_t restore = restore_focus_.empty() ? 0 : restore_focus_.back();
    if (!restore_focus_.empty()) restore_focus_.pop_back();
    if (!t) return;
    t->invalidate_all();
    t->set_modal(top());
    if (restore)
        if (Node* f = find_runtime(t->root(), restore)) t->set_focus(f);
}

bool ModalLayer::arrange_children(Tree& tree, const Rect& in) {
    for (const auto& ch : children()) {
        const float w = std::min(ch->width == Sizing::fixed ? ch->fixed_w : 440.f, std::max(40.f, in.w - 32));
        const Size s = tree.measure(*ch, w);
        const float h = std::min(s.h, std::max(40.f, in.h - 32));
        tree.place(*ch, {in.x + std::floor((in.w - w) / 2), in.y + std::floor((in.h - h) / 3), w, h});
    }
    return true;
}

void ModalLayer::draw(Canvas& c) {
    // Darkens what lies under the dialog (improve-desktop-look D9). A high-contrast theme keeps its colours: no dimming,
    // the dialog's border sets it apart.
    if (c.ui().theme.high_contrast) return;
    const Rect& f = frame();
    c.dc()->FillRectangle(D2D1::RectF(f.x, f.y, f.right(), f.bottom()), c.brush(0x000000u, c.ui().theme.dark ? 0.4f : 0.3f));
}

// --- the window buttons -------------------------------------------------------------------------------------

namespace {

constexpr char16_t kGlyphMinimize = 0xE921, kGlyphMaximize = 0xE922, kGlyphRestore = 0xE923, kGlyphClose = 0xE8BB;
constexpr uint32_t kCloseRed = 0xC42B1C;  // the Windows 11 close button

const char* caption_id(CaptionButton::Action a) {
    switch (a) {
    case CaptionButton::Action::minimize: return "title-bar-minimize";
    case CaptionButton::Action::maximize: return "title-bar-maximize";
    case CaptionButton::Action::close: return "title-bar-close";
    }
    return "title-bar-button";
}

}  // namespace

BrandMark::BrandMark(Ui& ui, float size) : ui_(ui), size_(size) {
    width = Sizing::fixed;
    height = Sizing::fill;
    fixed_w = size;
    min_h = size;
}

void BrandMark::draw(Canvas& c) {
    const Rect& f = frame();
    const Rect sq{f.x, f.y + (f.h - size_) / 2, size_, size_};
    const float r = size_ * 0.22f;
    const bool hc = ui_.theme.high_contrast;
    if (hc) c.round(sq, r, Paint::text);
    else c.dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sq.x, sq.y, sq.right(), sq.bottom()), r, r), c.brush(0x0E3A2Fu, 1.f));
    IDWriteTextLayout* l = ui_.layout(u"S", TextStyle::body_strong, 100.f, false);
    if (!l) return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    const float x = sq.x + (sq.w - m.width) / 2, y = sq.y + (sq.h - m.height) / 2;
    if (hc) c.text(l, x, y, Paint::background);
    else c.dc()->DrawTextLayout(D2D1::Point2F(x, y), l, c.brush(0x78FAAEu, 1.f));
}

CaptionButton::CaptionButton(Ui& ui, Action action) : Button(ui, caption_id(action), {}, Kind::subtle), action_(action) {
    width = Sizing::fixed;
    fixed_w = 46;
    height = Sizing::fill;
    padding = {};
    focusable = false;  // as in other Windows apps: the keyboard reaches them through the window menu (Alt+Space)
    system_keyboard = true;
    sync();
}

void CaptionButton::sync() {
    const bool zoomed = ui_.host && ::IsZoomed(ui_.host);
    const char* key = action_ == Action::minimize ? "window.minimize"
                      : action_ == Action::close  ? "window.close"
                      : zoomed                    ? "window.restore"
                                                  : "window.maximize";
    name = ui_.strings.get(key);
    help = name;
    glyph_ = action_ == Action::minimize ? kGlyphMinimize : action_ == Action::close ? kGlyphClose : zoomed ? kGlyphRestore : kGlyphMaximize;
    invalidate();
}

void CaptionButton::invoke() {
    if (!ui_.host) return;
    const WPARAM cmd = action_ == Action::minimize ? SC_MINIMIZE
                       : action_ == Action::close  ? SC_CLOSE
                       : ::IsZoomed(ui_.host)      ? SC_RESTORE
                                                   : SC_MAXIMIZE;
    ::PostMessageW(ui_.host, WM_SYSCOMMAND, cmd, 0);
}

void CaptionButton::draw(Canvas& c) {
    const Rect& f = frame();
    const bool hc = ui_.theme.high_contrast;
    const bool on = hot || hovered_, press = down || pressed_;
    bool white = false;
    if (action_ == Action::close && (on || press) && !hc) {
        c.dc()->FillRectangle(D2D1::RectF(f.x, f.y, f.right(), f.bottom()), c.brush(kCloseRed, press ? 0.85f : 1.f));
        white = true;
    } else if (on || press) {
        c.round(f, 0, hc ? Paint::selection : Paint::text, Paint::none, hc ? 1.f : press ? 0.10f : 0.06f);
    }
    IDWriteTextLayout* g = ui_.layout(std::u16string(1, glyph_), TextStyle::caption_icon, 100.f, false);
    if (!g) return;
    DWRITE_TEXT_METRICS m{};
    g->GetMetrics(&m);
    const float x = f.x + (f.w - m.width) / 2, y = f.y + (f.h - m.height) / 2;
    const Paint ink = hc && (on || press) ? Paint::selection_text : active ? Paint::text : Paint::text_secondary;
    if (white) c.dc()->DrawTextLayout(D2D1::Point2F(x, y), g, c.brush(0xFFFFFFu, 1.f));
    else c.text(g, x, y, ink);
}

WindowButtons::WindowButtons(Ui& ui) : Node("window-buttons") {
    floating = true;
    hit_through = true;
    for (auto a : {CaptionButton::Action::minimize, CaptionButton::Action::maximize, CaptionButton::Action::close})
        buttons_[static_cast<int>(a)] = emplace<CaptionButton>(ui, a);
}

bool WindowButtons::arrange_children(Tree& tree, const Rect& in) {
    float x = in.right() - kWidth;
    for (CaptionButton* b : buttons_) {
        tree.place(*b, {x, in.y, kWidth / 3, kHeight});
        x += kWidth / 3;
    }
    return true;
}

void WindowButtons::set_active(bool active) {
    for (CaptionButton* b : buttons_) {
        b->active = active;
        b->invalidate();
    }
}

void WindowButtons::sync() {
    for (CaptionButton* b : buttons_) b->sync();
}

CaptionSpace::CaptionSpace() : Node("caption-space") {
    width = Sizing::fixed;
    fixed_w = 0;
}

bool CaptionSpace::arrange_children(Tree& tree, const Rect&) {
    const Node* row = parent();
    if (!row) return true;
    // The row's right edge does not depend on this node's width, so the decision settles after one more pass.
    const bool corner = row->frame().y < WindowButtons::kHeight && row->frame().right() >= tree.viewport().w - 0.5f;
    const float want = corner ? WindowButtons::kWidth : 0.f;
    if (want != fixed_w) {
        fixed_w = want;
        tree.layout_again();
    }
    return true;
}

WindowRoot::WindowRoot(Tree& tree, Ui* window_buttons_ui) {
    Node& root = tree.root();
    content_ = root.emplace<Box>("content", Paint::background);
    content_->height = Sizing::fill;
    overlay_ = root.emplace<OverlayLayer>();
    modal_ = root.emplace<ModalLayer>();
    // Above the dialogs: the window can always be minimised, maximised and closed.
    if (window_buttons_ui) buttons_ = root.emplace<WindowButtons>(*window_buttons_ui);
}

}  // namespace wb::ui
