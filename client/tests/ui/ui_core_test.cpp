// Portable tests of the native UI toolkit core (add-native-ui tasks 2.2, 2.4, 2.6): UTF-16 text, Markdown blocks,
// layout, focus order, the virtual list, string bundles and plural rules.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "ui/bundles.h"
#include "ui/layout.h"
#include "ui/list.h"
#include "ui/markdown.h"
#include "ui/strings.h"
#include "ui/text16.h"

using namespace wb::ui;

// --- text16 -------------------------------------------------------------------------------------------------

TEST(Text16, RoundTripsCzechAndEmoji) {
    const std::string s = "Příliš žluťoučký kůň 😀";
    const std::u16string u = to_utf16(s);
    EXPECT_EQ(u.size(), 23u);  // the emoji is a surrogate pair
    EXPECT_EQ(to_utf8(u), s);
}

TEST(Text16, InvalidBytesBecomeReplacement) {
    EXPECT_EQ(to_utf16("a\xff" "b"), u"a�b");
    EXPECT_EQ(to_utf16("\xc0\xaf"), u"��");          // overlong
    EXPECT_EQ(to_utf16("\xed\xa0\x80"), u"�");            // encoded surrogate
    EXPECT_EQ(to_utf16("\xe2\x82"), u"��");          // truncated
    EXPECT_EQ(to_utf8(std::u16string(1, char16_t(0xD800))), "\xef\xbf\xbd");
}

// --- Markdown -----------------------------------------------------------------------------------------------

namespace {
const Span* find_span(const std::vector<Span>& spans, unsigned style) {
    for (const Span& s : spans)
        if (s.style == style) return &s;
    return nullptr;
}
std::u16string span_text(const std::u16string& t, const Span& s) { return t.substr(s.start, s.length); }
}  // namespace

TEST(Markdown, HeadingListTableAndCodeHideTheMarkup) {
    const auto b = parse_markdown("## Plan\n\n- one\n- **two**\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\n```js\nx = 1;\n```\n");
    ASSERT_EQ(b.size(), 5u);
    EXPECT_EQ(b[0].kind, BlockKind::heading);
    EXPECT_EQ(b[0].level, 2);
    EXPECT_EQ(b[0].text, u"Plan");
    EXPECT_EQ(b[1].kind, BlockKind::item);
    EXPECT_EQ(b[1].marker, u"•");
    EXPECT_EQ(b[1].text, u"one");
    EXPECT_EQ(b[2].text, u"two");
    const Span* bold = find_span(b[2].spans, kBold);
    ASSERT_NE(bold, nullptr);
    EXPECT_EQ(span_text(b[2].text, *bold), u"two");
    EXPECT_EQ(b[3].kind, BlockKind::table);
    ASSERT_EQ(b[3].rows.size(), 2u);
    EXPECT_TRUE(b[3].rows[0][0].header);
    EXPECT_EQ(b[3].rows[1][1].text, u"2");
    EXPECT_EQ(b[4].kind, BlockKind::code);
    EXPECT_EQ(b[4].language, u"js");
    EXPECT_EQ(b[4].text, u"x = 1;");
}

TEST(Markdown, InlineStyles) {
    const auto b = parse_markdown("a *i* `c` ~~s~~ [l](https://x.test/p)");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].text, u"a i c s l");
    ASSERT_NE(find_span(b[0].spans, kItalic), nullptr);
    ASSERT_NE(find_span(b[0].spans, kCode), nullptr);
    ASSERT_NE(find_span(b[0].spans, kStrike), nullptr);
    const Span* link = find_span(b[0].spans, kLink);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->url, u"https://x.test/p");
    EXPECT_EQ(span_text(b[0].text, *link), u"l");
}

TEST(Markdown, OrderedTaskQuoteRuleAndNesting) {
    const auto b = parse_markdown("3. c\n4. d\n   - n\n\n- [ ] todo\n- [x] done\n\n> q\n\n---\n");
    ASSERT_EQ(b.size(), 7u);
    EXPECT_EQ(b[0].marker, u"3.");
    EXPECT_EQ(b[1].marker, u"4.");
    EXPECT_EQ(b[2].indent, 2);
    EXPECT_EQ(b[3].marker, u"☐");
    EXPECT_EQ(b[4].marker, u"☑");
    EXPECT_EQ(b[5].quote, 1);
    EXPECT_EQ(b[5].text, u"q");
    EXPECT_EQ(b[6].kind, BlockKind::rule);
}

TEST(Markdown, RawHtmlStaysLiteralText) {
    const auto b = parse_markdown("before <script>alert(1)</script> after\n\n<div>block</div>\n");
    const std::u16string all = plain_text(b);
    EXPECT_NE(all.find(u"<script>alert(1)</script>"), std::u16string::npos);
    EXPECT_NE(all.find(u"<div>block</div>"), std::u16string::npos);
}

TEST(Markdown, ImageIsAltTextAndLinkNeverAnImage) {
    const auto b = parse_markdown("![a cat](https://tracker.example/cat.png)");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].text, u"a cat [https://tracker.example/cat.png]");
    const Span* link = find_span(b[0].spans, kLink);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->url, u"https://tracker.example/cat.png");
    // The result is text and link spans only: there is no image block type for a renderer to fetch.
    for (const Block& x : b) EXPECT_TRUE(x.kind == BlockKind::paragraph);
}

TEST(Markdown, EntitiesDecode) {
    const auto b = parse_markdown("a &amp; b &#x1F600; &#65; &copy;");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].text, u"a & b 😀 A &copy;");
}

TEST(Markdown, StreamingReparseIsStableAtEveryPrefix) {
    const std::string full = "# Title\n\nSome **bold** and `code`.\n\n```py\nprint(1)\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n";
    for (size_t n = 0; n <= full.size(); ++n) {
        const auto b = parse_markdown(std::string_view(full).substr(0, n));
        for (const Block& x : b)
            for (const Span& s : x.spans) ASSERT_LE(s.start + s.length, x.text.size()) << "prefix " << n;
    }
    // An unclosed fence while streaming is still a code block with the text so far.
    const auto b = parse_markdown("```py\nprint(1)\n");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].kind, BlockKind::code);
    EXPECT_EQ(b[0].text, u"print(1)");
    EXPECT_EQ(parse_markdown(full).size(), 4u);
}

TEST(Markdown, SpansAreUtf16Units) {
    const auto b = parse_markdown("😀 **ž**");
    ASSERT_EQ(b.size(), 1u);
    const Span* bold = find_span(b[0].spans, kBold);
    ASSERT_NE(bold, nullptr);
    EXPECT_EQ(bold->start, 3u);
    EXPECT_EQ(span_text(b[0].text, *bold), u"ž");
}

// --- layout -------------------------------------------------------------------------------------------------

namespace {
Node* fixed(Node& parent, const char* id, float w, float h) {
    auto* n = parent.emplace<Node>(id);
    n->width = Sizing::fixed;
    n->height = Sizing::fixed;
    n->fixed_w = w;
    n->fixed_h = h;
    return n;
}
}  // namespace

TEST(Layout, RowWithFixedFillAndGap) {
    Tree t;
    Node& r = t.root();
    r.axis = Axis::row;
    r.padding = Insets::all(10);
    r.gap = 5;
    Node* a = fixed(r, "a", 100, 20);
    Node* b = r.emplace<Node>("b");
    b->width = Sizing::fill;
    b->height = Sizing::fill;
    Node* c = r.emplace<Node>("c");
    c->width = Sizing::fill;
    c->weight = 3;
    c->height = Sizing::fill;
    t.layout({520, 300});
    EXPECT_EQ(a->frame(), (Rect{10, 10, 100, 20}));
    // free = 500 - 100 - 2*5 = 390, shared 1:3
    EXPECT_FLOAT_EQ(b->frame().x, 115);
    EXPECT_FLOAT_EQ(b->frame().w, 97.5f);
    EXPECT_FLOAT_EQ(b->frame().h, 280);
    EXPECT_FLOAT_EQ(c->frame().x, 217.5f);
    EXPECT_FLOAT_EQ(c->frame().w, 292.5f);
}

TEST(Layout, ColumnFitsWrappingTextToWidth) {
    Tree t;
    Node& r = t.root();
    auto* text = r.emplace<Node>("text");
    // 10 DIPs per character, 1000 characters, 20 DIPs per line.
    text->measure = [](float w) {
        const float per_line = std::max(1.f, std::floor(w / 10.f));
        return Size{w, std::ceil(1000.f / per_line) * 20.f};
    };
    auto* footer = fixed(r, "footer", 50, 30);
    t.layout({400, 2000});
    EXPECT_FLOAT_EQ(text->frame().h, 500);  // 40 chars per line, 25 lines
    EXPECT_FLOAT_EQ(footer->frame().y, 500);
    t.layout({200, 2000});
    EXPECT_FLOAT_EQ(text->frame().h, 1000);
    EXPECT_FLOAT_EQ(footer->frame().y, 1000);
}

// client/work-inbox Dialogs And Action Rows (improve-desktop-look D3): a row of buttons wider than its parent continues
// on a second line, its height covers both lines, and align_end puts each line's buttons at its end.
TEST(Layout, WrappingRowContinuesOnTheNextLine) {
    Tree t;
    Node& r = t.root();
    auto* row = r.emplace<Node>("row");
    row->axis = Axis::row;
    row->wraps = true;
    row->align_end = true;
    row->gap = 10;
    Node* a = fixed(*row, "a", 120, 30);
    Node* b = fixed(*row, "b", 120, 30);
    Node* c = fixed(*row, "c", 120, 30);
    auto* below = fixed(r, "below", 50, 20);
    t.layout({300, 400});
    // 120 + 10 + 120 = 250 fits in 300; c goes to the second line. Each line sits at the end (free 50, then 180).
    EXPECT_EQ(a->frame(), (Rect{50, 0, 120, 30}));
    EXPECT_EQ(b->frame(), (Rect{180, 0, 120, 30}));
    EXPECT_EQ(c->frame(), (Rect{180, 40, 120, 30}));
    EXPECT_FLOAT_EQ(row->frame().h, 70);
    EXPECT_FLOAT_EQ(below->frame().y, 70);
    // Wide enough: one line.
    t.layout({500, 400});
    EXPECT_FLOAT_EQ(c->frame().y, 0);
    EXPECT_FLOAT_EQ(row->frame().h, 30);
    // A fill child takes what is left of its line.
    row->align_end = false;
    auto* rest = row->emplace<Node>("rest");
    rest->width = Sizing::fill;
    t.layout({500, 400});
    EXPECT_FLOAT_EQ(a->frame().x, 0);
    EXPECT_FLOAT_EQ(rest->frame().x, 390);
    EXPECT_FLOAT_EQ(rest->frame().w, 110);
}

TEST(Layout, FitWidthAndMinimums) {
    Tree t;
    Node& r = t.root();
    r.axis = Axis::row;
    auto* fit = r.emplace<Node>("fit");
    fit->width = Sizing::fit;
    fit->padding = {4, 2, 4, 2};
    fit->measure = [](float) { return Size{30, 10}; };
    auto* fill = r.emplace<Node>("fill");
    fill->width = Sizing::fill;
    fill->min_w = 500;
    t.layout({300, 100});
    EXPECT_EQ(fit->frame(), (Rect{0, 0, 38, 14}));
    EXPECT_FLOAT_EQ(fill->frame().w, 500);  // the minimum wins over the free space
}

TEST(Layout, HiddenNodesTakeNoSpaceAndAreNotHit) {
    Tree t;
    Node& r = t.root();
    Node* a = fixed(r, "a", 100, 50);
    Node* b = fixed(r, "b", 100, 50);
    a->visible = false;
    t.layout({200, 200});
    EXPECT_FLOAT_EQ(b->frame().y, 0);
    EXPECT_EQ(t.hit_test(10, 10), b);
    EXPECT_EQ(t.hit_test(150, 150), &r);
    EXPECT_EQ(t.hit_test(500, 10), nullptr);
}

TEST(Layout, HitTestFindsDeepestNode) {
    Tree t;
    Node& r = t.root();
    Node* card = fixed(r, "card", 200, 100);
    card->axis = Axis::row;
    card->padding = Insets::all(10);
    Node* button = fixed(*card, "button", 40, 20);
    t.layout({300, 300});
    EXPECT_EQ(t.hit_test(15, 15), button);
    EXPECT_EQ(t.hit_test(100, 50), card);
}

TEST(Layout, InvalidationMergesAndTracksFrameChanges) {
    Tree t;
    Node& r = t.root();
    Node* a = fixed(r, "a", 100, 50);
    t.layout({400, 400});
    EXPECT_EQ(t.take_dirty(), (std::vector<Rect>{{0, 0, 400, 400}}));  // first layout: everything
    t.layout({400, 400});
    EXPECT_TRUE(t.take_dirty().empty());  // nothing moved
    a->fixed_h = 60;
    t.layout({400, 400});
    EXPECT_EQ(t.take_dirty(), (std::vector<Rect>{{0, 0, 100, 60}}));  // old and new frame, merged
    t.invalidate(Rect{0, 0, 10, 10});
    t.invalidate(Rect{300, 300, 10, 10});
    t.invalidate(Rect{5, 5, 10, 10});
    const auto d = t.take_dirty();
    ASSERT_EQ(d.size(), 2u);
    t.invalidate(Rect{-50, -50, 60, 60});
    EXPECT_EQ(t.take_dirty(), (std::vector<Rect>{{0, 0, 10, 10}}));  // clipped to the viewport
    for (int i = 0; i < 20; ++i) t.invalidate(Rect{static_cast<float>(i * 20), 0, 5, 5});
    EXPECT_LE(t.take_dirty().size(), Tree::kMaxDirty);
}

TEST(Focus, TabOrderIsTreeOrderSkippingHiddenAndDisabled) {
    Tree t;
    Node& r = t.root();
    r.axis = Axis::row;
    Node* nav = r.emplace<Node>("nav");
    Node* list = r.emplace<Node>("list");
    Node* n1 = nav->emplace<Node>("new");
    Node* n2 = nav->emplace<Node>("settings");
    Node* l1 = list->emplace<Node>("row1");
    Node* hidden = list->emplace<Node>("hidden");
    Node* disabled = list->emplace<Node>("disabled");
    Node* l2 = list->emplace<Node>("row2");
    for (Node* n : {n1, n2, l1, hidden, disabled, l2}) n->focusable = true;
    hidden->visible = false;
    disabled->enabled = false;
    EXPECT_EQ(t.focus_chain(), (std::vector<Node*>{n1, n2, l1, l2}));
    EXPECT_EQ(t.next_focus(nullptr), n1);
    EXPECT_EQ(t.next_focus(nullptr, true), l2);
    EXPECT_EQ(t.next_focus(n2), l1);
    EXPECT_EQ(t.next_focus(l2), n1);        // wraps
    EXPECT_EQ(t.next_focus(n1, true), l2);  // Shift+Tab wraps
    EXPECT_EQ(t.next_focus(hidden), n1);    // a node outside the chain starts over
}

TEST(Focus, SetFocusInvalidatesAndForgetClears) {
    Tree t;
    Node& r = t.root();
    Node* a = fixed(r, "a", 100, 20);
    Node* b = fixed(r, "b", 100, 20);
    a->focusable = b->focusable = true;
    t.layout({200, 200});
    t.take_dirty();
    t.set_focus(a);
    t.set_focus(b);
    EXPECT_EQ(t.take_dirty(), (std::vector<Rect>{{0, 0, 104, 44}}));  // with the focus ring's 4 DIPs, clipped
    t.forget(b);
    EXPECT_EQ(t.focused(), nullptr);
}

namespace {
// A button-like node: counts invocations.
struct Clickable : Node {
    explicit Clickable(std::string id) : Node(std::move(id)) {
        role = Role::button;
        focusable = true;
        name = u"Click";
        width = height = Sizing::fixed;
        fixed_w = 50;
        fixed_h = 20;
    }
    bool can_invoke() const override { return true; }
    void invoke() override { ++invoked; }
    void on_hover(bool on) override { hover = on; }
    int invoked = 0;
    bool hover = false;
};

PointerEvent at(PointerEvent::Kind k, float x, float y) {
    PointerEvent e;
    e.kind = k;
    e.x = x;
    e.y = y;
    return e;
}
}  // namespace

TEST(Input, ClickInvokesWhenReleasedOverThePressedNode) {
    Tree t;
    auto* b = t.root().emplace<Clickable>("b");
    auto* label = b->emplace<Node>("label");  // clicks on a child reach the button
    label->width = label->height = Sizing::fixed;
    label->fixed_w = 10;
    label->fixed_h = 10;
    t.layout({200, 200});
    t.pointer(at(PointerEvent::move, 5, 5));
    EXPECT_TRUE(b->hover);
    EXPECT_EQ(t.hovered(), b);
    t.pointer(at(PointerEvent::down, 5, 5));
    EXPECT_EQ(t.focused(), b);
    t.pointer(at(PointerEvent::up, 5, 5));
    EXPECT_EQ(b->invoked, 1);
    // Pressed on the button, released elsewhere: no click.
    t.pointer(at(PointerEvent::down, 5, 5));
    t.pointer(at(PointerEvent::up, 150, 150));
    EXPECT_EQ(b->invoked, 1);
    t.pointer(at(PointerEvent::leave, 0, 0));
    EXPECT_FALSE(b->hover);
}

// A handler that removes its own node (opening a Thread rebuilds the list row that was clicked) ends the dispatch: the
// removed node is not touched again (the debug build traps on such a use after free, as the client did on 2026-11-01).
TEST(Input, HandlerThatRemovesItsNodeEndsTheDispatch) {
    struct SelfRemoving : Node {
        explicit SelfRemoving(PointerEvent::Kind k) : Node("self"), on(k) {
            width = height = Sizing::fixed;
            fixed_w = 50;
            fixed_h = 20;
        }
        bool on_pointer(const PointerEvent& e) override {
            if (e.kind == PointerEvent::down && on == PointerEvent::up) return true;  // takes the press
            if (e.kind == on) (void)parent()->remove(this);                         // destroys this node
            return false;
        }
        PointerEvent::Kind on;
    };
    for (const auto kind : {PointerEvent::down, PointerEvent::up}) {
        Tree t;
        int parent_calls = 0;
        struct Counting : Node {
            explicit Counting(int& n) : Node("parent"), calls(n) {
                width = height = Sizing::fixed;
                fixed_w = fixed_h = 100;
            }
            bool on_pointer(const PointerEvent&) override { ++calls; return false; }
            int& calls;
        };
        auto* parent = t.root().emplace<Counting>(parent_calls);
        parent->emplace<SelfRemoving>(kind);
        t.layout({200, 200});
        const char* what = kind == PointerEvent::down ? "removed on press" : "removed on release";
        EXPECT_TRUE(t.pointer(at(PointerEvent::down, 5, 5))) << what;
        EXPECT_EQ(parent_calls, 0) << what << ": the press did not bubble on from the removed node";
        t.layout_if_needed();
        EXPECT_FALSE(t.pointer(at(PointerEvent::up, 5, 5))) << what;
        EXPECT_TRUE(parent->children().empty()) << what;
        // Removed on press: the release reaches the parent, now under the pointer. Removed while captured: the
        // release ends with the removed node.
        EXPECT_EQ(parent_calls, kind == PointerEvent::down ? 1 : 0) << what;
        EXPECT_FALSE(t.pointer(at(PointerEvent::move, 5, 5))) << what;  // the tree still works
    }
}

TEST(Input, KeyboardTabEnterSpaceAndBubbling) {
    Tree t;
    auto* a = t.root().emplace<Clickable>("a");
    auto* b = t.root().emplace<Clickable>("b");
    t.layout({200, 200});
    Node* seen = nullptr;
    t.on_focus_changed = [&](Node* n) { seen = n; };
    EXPECT_TRUE(t.key({kKeyTab}));
    EXPECT_EQ(t.focused(), a);
    EXPECT_EQ(seen, a);
    t.key({kKeyTab});
    EXPECT_EQ(t.focused(), b);
    t.key({kKeyEnter});
    t.key({kKeySpace});
    EXPECT_EQ(b->invoked, 2);
    t.key({kKeyTab, true});  // Shift+Tab
    EXPECT_EQ(t.focused(), a);
    EXPECT_FALSE(t.key({kKeyF10}));
}

TEST(Scrolling, ScrollColumnMovesChildrenAndClampsAndScrollsFocusIntoView) {
    Tree t;
    Node& r = t.root();
    auto* s = r.emplace<Node>("scroller");
    s->scrolls = true;
    s->height = Sizing::fixed;
    s->fixed_h = 100;
    std::vector<Clickable*> items;
    for (int i = 0; i < 10; ++i) items.push_back(s->emplace<Clickable>("i" + std::to_string(i)));  // 20 each
    auto* below = fixed(r, "below", 50, 50);
    t.layout({200, 300});
    EXPECT_FLOAT_EQ(s->content_height(), 200);
    EXPECT_FLOAT_EQ(s->max_scroll(), 100);
    EXPECT_FLOAT_EQ(below->frame().y, 100);
    t.take_dirty();
    EXPECT_TRUE(t.scroll_to(*s, 30));
    EXPECT_FLOAT_EQ(items[0]->frame().y, -30);
    EXPECT_FLOAT_EQ(items[5]->frame().y, 70);
    EXPECT_FALSE(t.take_dirty().empty());
    EXPECT_TRUE(t.scroll_to(*s, 1000));
    EXPECT_FLOAT_EQ(s->scroll_y(), 100);  // clamped
    EXPECT_EQ(t.hit_test(10, 5), items[5]);
    EXPECT_EQ(t.hit_test(10, 120), below);  // a scrolled-out item under "below" is clipped away
    // Wheel over the scroller scrolls it.
    PointerEvent w = at(PointerEvent::wheel, 10, 50);
    w.wheel_dy = -40;
    t.pointer(w);
    EXPECT_FLOAT_EQ(s->scroll_y(), 60);
    // Focusing an item out of view scrolls it in.
    t.set_focus(items[0]);
    EXPECT_FLOAT_EQ(s->scroll_y(), 0);
    t.set_focus(items[9]);
    EXPECT_FLOAT_EQ(s->scroll_y(), 100);
    // Page Down from the focused item scrolls its scroller.
    t.scroll_to(*s, 0);
    t.key({kKeyPageDown});
    EXPECT_GT(s->scroll_y(), 0);
}

TEST(Layout, FloatingChildCoversTheParentOutsideTheFlow) {
    Tree t;
    Node& r = t.root();
    Node* a = fixed(r, "a", 50, 50);
    auto* overlay = r.emplace<Node>("overlay");
    overlay->floating = true;
    Node* b = fixed(r, "b", 50, 50);
    t.layout({300, 200});
    EXPECT_FLOAT_EQ(b->frame().y, 50);  // the floating node takes no space
    EXPECT_EQ(overlay->frame(), (Rect{0, 0, 300, 200}));
    (void)a;
}

TEST(ListView, RealisesOnlyVisibleRowsAndMovesSelectionByKeyboard) {
    Tree t;
    int built = 0;
    auto* list = t.root().emplace<ListView>("threads", [&](size_t i, ListRow& row) {
        ++built;
        row.name = u"Thread " + to_utf16(std::to_string(i));
        row.measure = [](float w) { return Size{w, 30}; };
    }, 30.f);
    list->set_count(10000);
    t.layout({400, 300});
    // 10 rows fit, plus overscan of 2 below.
    EXPECT_EQ(list->children().size(), 12u);
    EXPECT_LE(built, 12);
    EXPECT_FLOAT_EQ(list->content_height(), 300000);
    ASSERT_NE(list->row(0), nullptr);
    EXPECT_EQ(list->row(100), nullptr);
    // Keyboard: focus the list, Down selects the first row, End jumps to the last and scrolls it into view.
    std::vector<size_t> selected;
    list->on_select = [&](size_t i) { selected.push_back(i); };
    t.key({kKeyTab});
    EXPECT_EQ(t.focused(), list);
    t.key({kKeyDown});
    EXPECT_EQ(list->selected(), 0u);
    t.key({kKeyDown});
    EXPECT_EQ(list->selected(), 1u);
    t.key({kKeyEnd});
    EXPECT_EQ(list->selected(), 9999u);
    ASSERT_NE(list->row(9999), nullptr);
    EXPECT_FLOAT_EQ(list->row(9999)->frame().bottom(), 300);
    EXPECT_LE(list->children().size(), 14u);  // rows near the top were dropped
    EXPECT_EQ(list->row(0), nullptr);
    EXPECT_EQ(selected, (std::vector<size_t>{0, 1, 9999}));
    EXPECT_EQ(list->row(9999)->selected_state(), 1);
    EXPECT_EQ(list->focus_rect(), list->row(9999)->frame());
    // Clicking a row selects it (rows are list items, the list holds focus).
    t.key({kKeyHome});
    t.layout_if_needed();
    t.pointer(at(PointerEvent::down, 10, 45));
    t.pointer(at(PointerEvent::up, 10, 45));
    EXPECT_EQ(list->selected(), 1u);
    EXPECT_EQ(t.focused(), list);
    // Children are in index order for the accessibility tree.
    const auto kids = a11y_children(*list);
    for (size_t i = 1; i < kids.size(); ++i)
        EXPECT_LT(static_cast<ListRow*>(kids[i - 1])->index(), static_cast<ListRow*>(kids[i])->index());
}

TEST(Accessibility, TreeSkipsLayoutNodesAndReportsProblems) {
    Tree t;
    Node& r = t.root();
    auto* pane = r.emplace<Node>("pane");  // layout only
    auto* group = pane->emplace<Node>("group");
    group->role = Role::group;
    auto* ok = group->emplace<Clickable>("ok");
    auto* nameless = pane->emplace<Clickable>("nameless");
    nameless->name.clear();
    auto* unreachable = pane->emplace<Clickable>("unreachable");
    unreachable->focusable = false;
    auto* norole = pane->emplace<Node>("norole");
    norole->focusable = true;
    EXPECT_EQ(a11y_children(r), (std::vector<Node*>{group, nameless, unreachable}));
    EXPECT_EQ(a11y_parent(*ok), group);
    EXPECT_EQ(a11y_parent(*group), &r);
    const auto problems = a11y_problems(t);
    EXPECT_EQ(problems, (std::vector<std::string>{"button 'nameless' has no name", "button 'unreachable' cannot be reached by keyboard",
                                                  "focusable node 'norole' has no role"}));
}

TEST(VirtualList, OffsetsAndVisibleRange) {
    VirtualList v(10000, 40);
    EXPECT_FLOAT_EQ(v.total(), 400000);
    EXPECT_EQ(v.index_at(0), 0u);
    EXPECT_EQ(v.index_at(39.9f), 0u);
    EXPECT_EQ(v.index_at(40), 1u);
    EXPECT_EQ(v.index_at(1e9f), 9999u);
    v.set_height(0, 100);
    v.set_height(5000, 10);
    EXPECT_FLOAT_EQ(v.offset(1), 100);
    EXPECT_FLOAT_EQ(v.offset(5001), 100 + 4999 * 40 + 10);
    EXPECT_FLOAT_EQ(v.total(), 400000 + 60 - 30);
    EXPECT_EQ(v.index_at(100), 1u);
    const auto r = v.visible(100, 400, 0);
    EXPECT_EQ(r.first, 1u);
    EXPECT_EQ(r.last, 11u);  // [100, 500) covers items 1..10
    const auto o = v.visible(100, 400, 2);
    EXPECT_EQ(o.first, 0u);
    EXPECT_EQ(o.last, 13u);
    EXPECT_EQ(VirtualList().visible(0, 100).last, 0u);
}

// --- strings ------------------------------------------------------------------------------------------------

TEST(Plural, CzechOneFewOther) {
    EXPECT_EQ(plural_category("cs", 0), Plural::other);
    EXPECT_EQ(plural_category("cs", 1), Plural::one);
    for (int n : {2, 3, 4}) EXPECT_EQ(plural_category("cs", n), Plural::few) << n;
    for (int n : {5, 11, 12, 21, 22, 100}) EXPECT_EQ(plural_category("cs", n), Plural::other) << n;
    EXPECT_EQ(plural_category("cs", -3), Plural::few);
    EXPECT_EQ(plural_category("de", 3), Plural::other);
    EXPECT_EQ(plural_category("en", 1), Plural::one);
}

TEST(Strings, FillsParametersAndPlurals) {
    auto cs = Strings::parse("cs", R"({"n":{"one":"{n} vlákno","few":"{n} vlákna","other":"{n} vláken"},
        "g":"Ahoj {name}, {{literal}}"})");
    ASSERT_TRUE(cs.ok());
    EXPECT_EQ(cs->count("n", 1), u"1 vlákno");
    EXPECT_EQ(cs->count("n", 3), u"3 vlákna");
    EXPECT_EQ(cs->count("n", 5), u"5 vláken");
    EXPECT_EQ(cs->get("g", {{"name", u"Pavle"}}), u"Ahoj Pavle, {literal}");
    EXPECT_EQ(cs->get("missing.key"), u"missing.key");
    EXPECT_EQ(cs->get("g"), u"Ahoj {name}, {literal}");  // an unfilled parameter stays visible
}

TEST(Strings, RejectsMalformedBundles) {
    EXPECT_FALSE(Strings::parse("en", "[]").ok());
    EXPECT_FALSE(Strings::parse("en", R"({"a":1})").ok());
    EXPECT_FALSE(Strings::parse("en", R"({"a":{"many":"x"}})").ok());
    EXPECT_FALSE(template_params("a {b").ok());
    EXPECT_FALSE(template_params("a } b").ok());
    EXPECT_FALSE(template_params("{a b}").ok());
    EXPECT_EQ(*template_params("{a} {{x}} {b_2}"), (std::set<std::string>{"a", "b_2"}));
}

TEST(Strings, BuiltInBundlesLoadAndAgree) {
    const Strings cs = load_bundle("cs"), de = load_bundle("de"), en = load_bundle("en");
    EXPECT_EQ(cs.lang(), "cs");
    EXPECT_FALSE(en.entries().empty());
    EXPECT_TRUE(check_bundles({cs, de, en}).empty());
    EXPECT_EQ(cs.count("nav.count", 3), u"3 vlákna");
    EXPECT_EQ(de.get("views.inbox"), u"Posteingang");
    EXPECT_EQ(en.get("status_bar.mcp", {{"healthy", u"2"}, {"total", u"3"}}), u"Connections 2/3");
}

TEST(Strings, LanguageFollowsSettingThenSystem) {
    EXPECT_EQ(resolve_language("de", "cs-CZ"), "de");
    EXPECT_EQ(resolve_language("system", "cs-CZ"), "cs");
    EXPECT_EQ(resolve_language("", "DE_at"), "de");
    EXPECT_EQ(resolve_language("system", "fr-FR"), "en");
    EXPECT_EQ(resolve_language("xx", ""), "en");
}

TEST(Strings, CheckFindsMissingKeysParametersAndPluralForms) {
    auto en = *Strings::parse("en", R"({"a":"A {x}","b":"B","c":{"one":"{n} c","other":"{n} cs"}})");
    auto cs = *Strings::parse("cs", R"({"a":"A {y}","c":{"one":"{n} c","other":"{n} c"},"z":"Z"})");
    auto de = *Strings::parse("de", R"({"a":"A {x}","b":"B","c":{"one":"{n} c","other":"{n} cs"}})");
    const auto issues = check_bundles({en, cs, de});
    auto has = [&](const std::string& s) { return std::find(issues.begin(), issues.end(), s) != issues.end(); };
    EXPECT_TRUE(has("cs: a: missing parameter {x}"));
    EXPECT_TRUE(has("cs: a: unknown parameter {y}"));
    EXPECT_TRUE(has("cs: b: missing"));
    EXPECT_TRUE(has("cs: c: missing plural form 'few'"));
    EXPECT_TRUE(has("cs: z: not in the reference bundle"));
    EXPECT_EQ(issues.size(), 5u);
    EXPECT_TRUE(check_bundles({en, de}).empty());
}
