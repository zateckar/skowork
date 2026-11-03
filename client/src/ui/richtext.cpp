#include "ui/richtext.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "ui/elements.h"

namespace wb::ui {

namespace {

TextStyle heading_style(int level) {
    return level <= 1 ? TextStyle::heading1 : level == 2 ? TextStyle::heading2 : TextStyle::heading3;
}

bool same_spans(const std::vector<Span>& a, const std::vector<Span>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].style != b[i].style || a[i].url != b[i].url) return false;
    return true;
}

bool same_block(const Block& a, const Block& b) {
    if (a.kind != b.kind || a.level != b.level || a.indent != b.indent || a.quote != b.quote || a.marker != b.marker ||
        a.language != b.language || a.text != b.text || !same_spans(a.spans, b.spans) || a.rows.size() != b.rows.size())
        return false;
    for (size_t r = 0; r < a.rows.size(); ++r) {
        if (a.rows[r].size() != b.rows[r].size()) return false;
        for (size_t c = 0; c < a.rows[r].size(); ++c) {
            const Cell &x = a.rows[r][c], &y = b.rows[r][c];
            if (x.text != y.text || x.header != y.header || !same_spans(x.spans, y.spans)) return false;
        }
    }
    return true;
}

// Draws the bars of a block quote at its left edge.
class QuoteBlock : public Node {
public:
    explicit QuoteBlock(int depth) : depth_(depth) { padding.left = 14.f * static_cast<float>(depth); }
    void draw(Canvas& c) override {
        const Rect& f = frame();
        for (int k = 0; k < depth_; ++k) {
            const float x = f.x + 3 + 14.f * static_cast<float>(k);
            c.dc()->FillRectangle(D2D1::RectF(x, f.y, x + 3, f.bottom()), c.brush(Paint::border));
        }
    }

private:
    int depth_;
};

class Rule : public Node {
public:
    Rule() {
        role = Role::separator;
        height = Sizing::fixed;
        fixed_h = 17;
    }
    void draw(Canvas& c) override {
        const Rect& f = frame();
        const float y = std::floor(f.y + f.h / 2) + 0.5f;
        c.line(f.x, y, f.right(), y, Paint::border);
    }
};

class TableRow : public Node {
public:
    explicit TableRow(bool last) : last_(last) { axis = Axis::row; }
    void draw(Canvas& c) override {
        if (last_) return;
        const Rect& f = frame();
        c.line(f.x, f.bottom() - 0.5f, f.right(), f.bottom() - 0.5f, Paint::border);
    }

private:
    bool last_;
};

}  // namespace

// --- RichText -----------------------------------------------------------------------------------------------

RichText::RichText(Ui& ui, std::u16string text, std::vector<Span> spans, TextStyle base, Paint color, std::string id)
    : Node(std::move(id)), ui_(ui), text_(std::move(text)), spans_(std::move(spans)), base_(base), color_(color) {
    role = Role::text;
    name = text_;
    measure = [this](float w) {
        IDWriteTextLayout* l = layout_for(w);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return Size{w, std::ceil(m.height)};
    };
    for (const Span& s : spans_)
        if (s.style & kLink)
            emplace<LinkSpan>(ui_, text_.substr(s.start, s.length), s.url, s.start, s.length);
}

IDWriteTextLayout* RichText::layout_for(float w) {
    w = std::max(1.f, w);
    if (layout_ && layout_w_ == w) return layout_.get();
    layout_.reset();
    IDWriteTextFormat* f = ui_.format(base_);
    if (FAILED(ui_.dwrite()->CreateTextLayout(reinterpret_cast<const wchar_t*>(text_.data()), static_cast<UINT32>(text_.size()), f,
                                              w, 1e6f, layout_.put())))
        return nullptr;
    layout_w_ = w;
    const float base_size = f->GetFontSize();
    for (const Span& s : spans_) {
        const DWRITE_TEXT_RANGE r{s.start, s.length};
        if (s.style & kBold) layout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
        if (s.style & kItalic) layout_->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
        if (s.style & kStrike) layout_->SetStrikethrough(TRUE, r);
        if (s.style & kLink) layout_->SetUnderline(TRUE, r);
        if (s.style & kCode) {
            layout_->SetFontFamilyName(ui_.code_family().c_str(), r);
            layout_->SetFontSize(std::round(base_size * 0.93f), r);
        }
    }
    return layout_.get();
}

bool RichText::arrange_children(Tree& tree, const Rect& in) {
    IDWriteTextLayout* l = layout_for(in.w);
    for (const auto& ch : children()) {
        auto* link = static_cast<LinkSpan*>(ch.get());
        Rect u;
        UINT32 n = 0;
        if (l) {
            l->HitTestTextRange(link->start(), link->length(), in.x, in.y, nullptr, 0, &n);
            std::vector<DWRITE_HIT_TEST_METRICS> m(n);
            if (n && SUCCEEDED(l->HitTestTextRange(link->start(), link->length(), in.x, in.y, m.data(), n, &n)))
                for (const auto& h : m) u = u.unite({h.left, h.top, h.width, h.height});
        }
        tree.place(*link, u);
    }
    return true;
}

Rect RichText::inner() const {
    const Rect& f = frame();
    return {f.x + padding.left, f.y + padding.top, f.w - padding.left - padding.right, f.h - padding.top - padding.bottom};
}

uint32_t RichText::position_at(float x, float y) {
    const Rect in = inner();
    IDWriteTextLayout* l = layout_for(in.w);
    if (!l) return 0;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(l->HitTestPoint(x - in.x, y - in.y, &trailing, &inside, &m))) return 0;
    return std::min<uint32_t>(m.textPosition + (trailing ? m.length : 0), static_cast<uint32_t>(text_.size()));
}

void RichText::set_selection(uint32_t from, uint32_t to) {
    if (from == sel_from_ && to == sel_to_) return;
    sel_from_ = from;
    sel_to_ = to;
    invalidate();
}

void RichText::draw(Canvas& c) {
    const Rect in = inner();
    IDWriteTextLayout* l = layout_for(in.w);
    if (!l) return;
    if (sel_to_ > sel_from_) {
        UINT32 n = 0;
        l->HitTestTextRange(sel_from_, sel_to_ - sel_from_, in.x, in.y, nullptr, 0, &n);
        std::vector<DWRITE_HIT_TEST_METRICS> m(n);
        if (n && SUCCEEDED(l->HitTestTextRange(sel_from_, sel_to_ - sel_from_, in.x, in.y, m.data(), n, &n)))
            for (const auto& h : m) c.fill({h.left, h.top, std::max(h.width, 4.f), h.height}, Paint::selection);
    }
    for (const Span& s : spans_) {
        const DWRITE_TEXT_RANGE r{s.start, s.length};
        if (s.style & kLink) l->SetDrawingEffect(c.brush(Paint::link), r);  // set each frame: follows theme and device
        if (s.style & kCode) {
            UINT32 n = 0;
            l->HitTestTextRange(s.start, s.length, in.x, in.y, nullptr, 0, &n);
            std::vector<DWRITE_HIT_TEST_METRICS> m(n);
            if (n && SUCCEEDED(l->HitTestTextRange(s.start, s.length, in.x, in.y, m.data(), n, &n)))
                for (const auto& h : m) c.round({h.left - 2, h.top, h.width + 4, h.height}, 3, Paint::code_background);
        }
    }
    c.text(l, in.x, in.y, color_);
}

// --- LinkSpan -----------------------------------------------------------------------------------------------

LinkSpan::LinkSpan(Ui& ui, std::u16string text, std::u16string url, uint32_t start, uint32_t length)
    : Node("link"), ui_(ui), url_(std::move(url)), start_(start), length_(length) {
    role = Role::link;
    focusable = true;
    name = std::move(text);
    help = url_;  // screen readers and the tooltip show the target before it is opened
}

void LinkSpan::invoke() {
    if (ui_.open_link) ui_.open_link(url_);
}

void LinkSpan::draw(Canvas& c) {
    if (hovered_) c.round(frame(), 2, Paint::link, Paint::none, 0.12f);
}

// --- MarkdownView -------------------------------------------------------------------------------------------

MarkdownView::MarkdownView(Ui& ui, std::u16string nm, std::string id) : Node(std::move(id)), ui_(ui) {
    role = Role::document;
    name = std::move(nm);
    gap = 8;
}

MarkdownView::~MarkdownView() {
    if (ui_.selection == this) ui_.selection = nullptr;
}

std::vector<RichText*> MarkdownView::texts() const {
    std::vector<RichText*> out;
    const std::function<void(const Node&)> walk = [&](const Node& n) {
        for (const auto& c : n.children()) {
            if (auto* r = dynamic_cast<RichText*>(c.get())) out.push_back(r);
            else walk(*c);
        }
    };
    walk(*this);
    return out;
}

// The text position under a point: in the block it is over, else the nearest block on that line, else the end of the
// last block above it (or the start when it is above them all).
MarkdownView::Point MarkdownView::point_at(float x, float y) const {
    const auto ts = texts();
    std::optional<size_t> on_line;
    for (size_t i = 0; i < ts.size(); ++i) {
        const Rect& f = ts[i]->frame();
        if (y < f.y || y >= f.bottom()) continue;
        if (x >= f.x && x < f.right()) return {i, ts[i]->position_at(x, y)};
        if (!on_line) on_line = i;
    }
    if (on_line) return {*on_line, ts[*on_line]->position_at(x, y)};
    for (size_t i = ts.size(); i-- > 0;)
        if (ts[i]->frame().bottom() <= y) return {i, static_cast<uint32_t>(ts[i]->text().size())};
    return {};
}

void MarkdownView::apply_selection() {
    Point a = anchor_, b = focus_;
    if (b.text < a.text || (b.text == a.text && b.pos < a.pos)) std::swap(a, b);
    const auto ts = texts();
    for (size_t i = 0; i < ts.size(); ++i) {
        if (i < a.text || i > b.text) {
            ts[i]->set_selection(0, 0);
            continue;
        }
        ts[i]->set_selection(i == a.text ? a.pos : 0, i == b.text ? b.pos : static_cast<uint32_t>(ts[i]->text().size()));
    }
}

void MarkdownView::clear_selection() {
    for (RichText* r : texts()) r->set_selection(0, 0);
    anchor_ = focus_ = {};
    selecting_ = false;
    if (ui_.selection == this) ui_.selection = nullptr;
}

std::u16string MarkdownView::selected_text() const {
    std::u16string out;
    bool first = true;
    for (const RichText* r : texts()) {
        if (r->selection_to() <= r->selection_from()) continue;
        if (!first) out += u'\n';
        first = false;
        out += r->text().substr(r->selection_from(), r->selection_to() - r->selection_from());
    }
    return out;
}

bool MarkdownView::on_pointer(const PointerEvent& e) {
    if (e.kind == PointerEvent::down) {
        if (e.button != 0) return false;
        // Links, Copy buttons and other controls inside the message keep their clicks.
        if (Tree* t = tree())
            for (Node* n = t->hit_test(e.x, e.y); n && n != this; n = n->parent())
                if (n->focusable || n->can_invoke()) return false;
        bool on_text = false;
        for (const RichText* r : texts()) on_text = on_text || r->frame().contains(e.x, e.y);
        if (!on_text) return false;
        if (ui_.selection && ui_.selection != this) ui_.selection->clear_selection();
        anchor_ = focus_ = point_at(e.x, e.y);
        selecting_ = true;
        moved_ = false;
        ui_.selection = this;
        apply_selection();
        return true;  // the view keeps the pointer until release
    }
    if (!selecting_) return false;
    if (e.kind == PointerEvent::move) {
        const Point p = point_at(e.x, e.y);
        if (!(p == focus_)) {
            focus_ = p;
            moved_ = true;
            apply_selection();
        }
        return true;
    }
    if (e.kind == PointerEvent::up) {
        selecting_ = false;
        if (anchor_ == focus_) clear_selection();  // a click without a drag selects nothing
        return true;
    }
    return false;
}

void MarkdownView::set_markdown(std::string_view utf8) {
    plain_text_ = false;
    set_blocks(parse_markdown(utf8));
}

void MarkdownView::set_plain(std::u16string text) {
    if (!plain_text_) clear_children();
    blocks_.clear();
    plain_text_ = true;
    std::vector<Block> next(1);
    next[0].text = std::move(text);
    set_blocks(std::move(next));
}

void MarkdownView::set_blocks(std::vector<Block> next) {
    size_t keep = 0;
    while (keep < blocks_.size() && keep < next.size() && same_block(blocks_[keep], next[keep])) ++keep;
    if (ui_.selection == this && (keep < blocks_.size() || keep < next.size())) clear_selection();  // its blocks change
    // Elements for blocks [keep, end) are replaced.
    while (children().size() > keep) remove(children().back().get());
    for (size_t i = keep; i < next.size(); ++i) add(build(next[i], i));
    rebuilt_ = next.size() - keep;
    blocks_ = std::move(next);
    plain_ = plain_text(blocks_);
    relayout();
    invalidate();
}

std::unique_ptr<Node> MarkdownView::build(const Block& b, size_t index) {
    const std::string id = plain_text_ ? automation_id() + "-text" : "md-" + std::to_string(index);
    std::unique_ptr<Node> node;
    switch (b.kind) {
    case BlockKind::heading: {
        auto t = std::make_unique<RichText>(ui_, b.text, b.spans, heading_style(b.level), Paint::text, id);
        t->padding.top = b.level <= 2 ? 6.f : 2.f;
        node = std::move(t);
        break;
    }
    case BlockKind::item: {
        auto row = std::make_unique<Node>(id);
        row->axis = Axis::row;
        row->padding.left = 20.f * static_cast<float>(std::max(0, b.indent - 1));
        auto* marker = row->emplace<Label>(ui_, b.marker, TextStyle::body, Paint::text_secondary, false);
        marker->role = Role::none;  // the item text is enough for screen readers; the marker is decoration
        marker->width = Sizing::fixed;
        marker->fixed_w = 22;
        row->add(std::make_unique<RichText>(ui_, b.text, b.spans, TextStyle::body));
        node = std::move(row);
        break;
    }
    case BlockKind::code: {
        auto box = std::make_unique<Box>(id, Paint::code_background, 6, ui_.theme.high_contrast ? Paint::text : Paint::none);
        box->padding = {12, 4, 4, 12};
        auto* head = box->emplace<Node>();
        head->axis = Axis::row;
        auto* lang = head->emplace<Label>(ui_, b.language.empty() ? ui_.strings.get("markdown.code") : b.language,
                                           TextStyle::caption, Paint::text_secondary, true);
        lang->padding.top = 6;
        lang->role = Role::none;
        auto* copy = head->emplace<Button>(ui_, id + "-copy", ui_.strings.get("markdown.copy"), Button::Kind::subtle, icon::copy);
        copy->name = ui_.strings.get("markdown.copy_code");
        std::u16string code = b.text;
        Ui* ui = &ui_;
        copy->on_click = [ui, code] {
            if (ui->copy_text) ui->copy_text(code);
            if (ui->announce) ui->announce(ui->strings.get("markdown.copied"));
        };
        auto* text = box->emplace<RichText>(ui_, b.text, std::vector<Span>{}, TextStyle::code, Paint::text, id + "-text");
        text->padding.right = 8;
        node = std::move(box);
        break;
    }
    case BlockKind::rule: node = std::make_unique<Rule>(); break;
    case BlockKind::table: {
        auto table = std::make_unique<Box>(id, Paint::none, 6, Paint::border);
        table->role = Role::group;
        table->name = ui_.strings.get("markdown.table");
        for (size_t r = 0; r < b.rows.size(); ++r) {
            auto* row = table->emplace<TableRow>(r + 1 == b.rows.size());
            for (const Cell& cell : b.rows[r]) {
                auto* t = row->emplace<RichText>(ui_, cell.text, cell.spans, cell.header ? TextStyle::body_strong : TextStyle::body);
                t->padding = Insets::xy(8, 6);
            }
        }
        node = std::move(table);
        break;
    }
    case BlockKind::paragraph:
    default: node = std::make_unique<RichText>(ui_, b.text, b.spans, TextStyle::body, Paint::text, id); break;
    }
    if (b.quote > 0) {
        auto q = std::make_unique<QuoteBlock>(b.quote);
        q->add(std::move(node));
        return q;
    }
    return node;
}

}  // namespace wb::ui
