// Tests of the native UI elements (add-native-ui tasks 2.3, 2.4): element behaviour with real DirectWrite text,
// Markdown rendering into elements, and golden images rendered off-screen on the software device.
#include <gtest/gtest.h>

#include <wincodec.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "ui/bundles.h"
#include "ui/canvas.h"
#include "ui/elements.h"
#include "ui/list.h"
#include "ui/richtext.h"
#include "ui/text16.h"
#include "support/golden.h"

using namespace wb;
using namespace wb::ui;

namespace {

const SystemColors kSys{0xFFFFFF, 0x000000, 0x0078D7, 0xFFFFFF, 0x0066CC, 0x6D6D6D, 0xF0F0F0, 0x000000};

struct ComInit {
    ComInit() : ok(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~ComInit() {
        if (ok) ::CoUninitialize();
    }
    bool ok;
};

// An off-screen surface with the services elements need, in a fixed light theme and English.
struct Surface {
    ComInit com;
    std::unique_ptr<Renderer> r;
    std::unique_ptr<Ui> ui;
    Tree tree;
    std::unique_ptr<WindowRoot> root;
    std::vector<std::u16string> opened, copied, announced;

    Surface(UINT w, UINT h, UINT dpi = 96) {
        auto rr = Renderer::offscreen(w, h, dpi);
        EXPECT_TRUE(rr.ok());
        r = std::move(rr).value();
        auto u = Ui::create(r->dwrite(), "en");
        EXPECT_TRUE(u.ok());
        ui = std::move(u).value();
        ui->theme = make_theme(false, false, kSys);
        ui->open_link = [this](const std::u16string& s) { opened.push_back(s); };
        ui->copy_text = [this](const std::u16string& s) { copied.push_back(s); };
        ui->announce = [this](const std::u16string& s) { announced.push_back(s); };
        root = std::make_unique<WindowRoot>(tree);
    }
    Node& content() { return root->content(); }
    std::vector<uint32_t> render() {
        tree.layout(r->size_dip());
        tree.invalidate_all();
        const auto regions = r->begin_frame(tree.take_dirty());
        {
            Canvas c(*r, *ui);
            paint(tree, c, regions);
        }
        EXPECT_TRUE(r->end_frame().ok());
        auto px = r->read_pixels();
        EXPECT_TRUE(px.ok());
        return px.ok() ? *px : std::vector<uint32_t>{};
    }
    uint32_t at(const std::vector<uint32_t>& px, float x, float y) const {
        const float k = r->scale();
        return px[static_cast<size_t>(y * k) * r->width_px() + static_cast<size_t>(x * k)] & 0xFFFFFF;
    }
};

// Pixels in a rectangle that differ clearly from the background (text, borders).
size_t ink(const std::vector<uint32_t>& px, UINT w, const Rect& r, uint32_t bg) {
    size_t n = 0;
    for (int y = static_cast<int>(r.y); y < static_cast<int>(r.bottom()); ++y)
        for (int x = static_cast<int>(r.x); x < static_cast<int>(r.right()); ++x) {
            const uint32_t p = px[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
            int d = 0;
            for (int s = 0; s < 24; s += 8) d += std::abs(static_cast<int>((p >> s) & 0xFF) - static_cast<int>((bg >> s) & 0xFF));
            if (d > 96) ++n;
        }
    return n;
}


}  // namespace

// --- elements ---------------------------------------------------------------------------------------------------

TEST(UiElements, ButtonsCheckboxAndCardBehave) {
    Surface s(400, 300);
    Node& c = s.content();
    c.padding = Insets::all(10);
    c.gap = 8;
    auto* b = c.emplace<Button>(*s.ui, "save", u"Save", Button::Kind::primary);
    int clicks = 0;
    b->on_click = [&] { ++clicks; };
    auto* icon = c.emplace<IconButton>(*s.ui, "more", icon::more, u"More options");
    auto* check = c.emplace<Checkbox>(*s.ui, "notify", u"Notify me");
    bool last = false;
    check->on_change = [&](bool on) { last = on; };
    auto* card = c.emplace<Card>(*s.ui, "card", u"Tool call: files.read", true, false);
    card->body().emplace<Label>(*s.ui, u"Arguments", TextStyle::body);
    s.tree.layout(s.r->size_dip());

    EXPECT_GT(b->frame().w, 40.f);  // fits its label
    EXPECT_FLOAT_EQ(b->frame().h, 32.f);
    EXPECT_FLOAT_EQ(icon->frame().w, 32.f);
    EXPECT_EQ(icon->help, u"More options");  // tooltip
    EXPECT_TRUE(a11y_problems(s.tree).empty());

    s.tree.pointer({PointerEvent::down, b->frame().x + 5, b->frame().y + 5});
    s.tree.pointer({PointerEvent::up, b->frame().x + 5, b->frame().y + 5});
    EXPECT_EQ(clicks, 1);
    s.tree.set_focus(check);
    s.tree.key({kKeySpace});
    EXPECT_TRUE(last);
    EXPECT_EQ(check->toggle_state(), 1);

    // The card starts collapsed; its header is an expand/collapse button.
    EXPECT_FALSE(card->body().visible);
    const float collapsed_h = card->frame().h;
    EXPECT_EQ(card->header().expand_state(), 0);
    s.tree.set_focus(&card->header());
    s.tree.key({kKeyEnter});
    s.tree.layout_if_needed();
    EXPECT_EQ(card->header().expand_state(), 1);
    EXPECT_GT(card->frame().h, collapsed_h);
}

TEST(UiElements, LabelsWrapAndEllipsize) {
    Surface s(300, 200);
    const std::u16string text(200, u'x');
    auto* wrap = s.content().emplace<Label>(*s.ui, text, TextStyle::body);
    auto* one = s.content().emplace<Label>(*s.ui, text, TextStyle::body, Paint::text, false);
    s.tree.layout(s.r->size_dip());
    EXPECT_GT(wrap->frame().h, 60.f);  // several lines
    EXPECT_LT(one->frame().h, 25.f);   // one line, cut to the width
    EXPECT_FLOAT_EQ(one->frame().w, 300.f);
    EXPECT_GT(s.ui->texts().size(), 0u);
}

TEST(UiElements, MenuOpensBelowItsAnchorAndClosesWithEscapeRestoringFocus) {
    Surface s(400, 300);
    auto* opener = s.content().emplace<IconButton>(*s.ui, "more", icon::more, u"More options");
    s.tree.layout(s.r->size_dip());
    s.tree.set_focus(opener);
    int archived = 0;
    auto menu = std::make_unique<Menu>("menu", u"Thread actions");
    menu->add_item(*s.ui, "archive", u"Archive", [&] { ++archived; }, icon::archive);
    menu->add_item(*s.ui, "delete", u"Delete", [] {}, icon::del);
    Menu* m = menu.get();
    s.root->overlay().show(std::move(menu), opener->frame(), true);
    s.tree.layout_if_needed();
    EXPECT_GE(m->frame().y, opener->frame().bottom());
    ASSERT_NE(s.tree.focused(), nullptr);
    EXPECT_EQ(s.tree.focused()->automation_id(), "archive");
    s.tree.key({kKeyDown});
    EXPECT_EQ(s.tree.focused()->automation_id(), "delete");
    s.tree.key({kKeyEscape});
    EXPECT_FALSE(s.root->overlay().has_popup());
    EXPECT_EQ(s.tree.focused(), opener);
    // Reopen, choose with Enter: the action runs and the menu closes.
    menu = std::make_unique<Menu>("menu", u"Thread actions");
    menu->add_item(*s.ui, "archive", u"Archive", [&] { ++archived; });
    s.root->overlay().show(std::move(menu), opener->frame(), true);
    s.tree.layout_if_needed();
    s.tree.key({kKeyEnter});
    EXPECT_EQ(archived, 1);
    EXPECT_FALSE(s.root->overlay().has_popup());
    // A press outside a popup closes it.
    menu = std::make_unique<Menu>("menu", u"Thread actions");
    menu->add_item(*s.ui, "archive", u"Archive", [] {});
    s.root->overlay().show(std::move(menu), opener->frame(), false);
    s.tree.layout_if_needed();
    s.tree.pointer({PointerEvent::down, 390, 290});
    EXPECT_FALSE(s.root->overlay().has_popup());
}

TEST(UiElements, SplitterResizesItsPaneByDragAndKeyboard) {
    Surface s(600, 300);
    Node& c = s.content();
    c.axis = Axis::row;
    c.height = Sizing::fill;
    auto* left = c.emplace<Box>("left");
    left->width = Sizing::fixed;
    left->fixed_w = 200;
    left->height = Sizing::fill;
    auto* split = c.emplace<Splitter>("split", u"Resize the thread list", *left, 150, 400);
    c.emplace<Box>("right")->height = Sizing::fill;
    s.tree.layout(s.r->size_dip());
    const float x = split->frame().x + 2;
    s.tree.pointer({PointerEvent::down, x, 100});
    s.tree.pointer({PointerEvent::move, x + 50, 100});
    s.tree.pointer({PointerEvent::up, x + 50, 100});
    EXPECT_FLOAT_EQ(left->fixed_w, 250);
    s.tree.set_focus(split);
    s.tree.key({kKeyRight});
    EXPECT_FLOAT_EQ(left->fixed_w, 266);
    s.tree.key({kKeyRight, false, true});
    s.tree.key({kKeyRight, false, true});
    s.tree.key({kKeyRight, false, true});
    EXPECT_FLOAT_EQ(left->fixed_w, 400);  // clamped
    s.tree.layout_if_needed();
    EXPECT_FLOAT_EQ(split->frame().x, 400);
}

TEST(UiElements, ImageDecodesScalesAndNeverUpscales) {
    Surface s(400, 300);
    // A 2x1 PNG made by WIC itself.
    const auto file = std::filesystem::temp_directory_path() / "workbench-ui-test.png";
    ASSERT_TRUE(write_png(file, {0xFFFF0000, 0xFF0000FF}, 2, 1));
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(file);
    auto img = decode_image(bytes.data(), bytes.size());
    ASSERT_TRUE(img.ok()) << img.error().message;
    auto* view = s.content().emplace<ImageView>(u"two pixels", *img);
    s.tree.layout(s.r->size_dip());
    EXPECT_FLOAT_EQ(view->frame().w, 2);
    EXPECT_FLOAT_EQ(view->frame().h, 1);
    EXPECT_FALSE(decode_image("not an image", 12).ok());
    EXPECT_FALSE(decode_image(nullptr, 0).ok());
}

// --- Markdown ---------------------------------------------------------------------------------------------------

TEST(UiMarkdown, BlocksBecomeElementsWithLinksAndCopy) {
    Surface s(600, 800);
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Answer");
    md->set_markdown(
        "# Plan\n\nRead the [guide](https://example.com/guide) and `run`.\n\n- one\n- two\n\n```sh\nmake all\n```\n\n"
        "| A | B |\n|---|---|\n| 1 | 2 |\n\n> quoted\n\n---\n\n![diagram](https://tracker.example/x.png) <script>x</script>\n");
    s.tree.layout(s.r->size_dip());
    EXPECT_EQ(md->blocks().size(), 9u);
    EXPECT_EQ(md->children().size(), 9u);
    EXPECT_EQ(md->value().find(u"# Plan"), std::u16string::npos);  // markup is not shown
    EXPECT_NE(md->value().find(u"<script>x</script>"), std::u16string::npos);
    EXPECT_TRUE(a11y_problems(s.tree).empty()) << a11y_problems(s.tree).front();

    // Links are focusable children placed over their text; the image is a link, never an image element.
    std::vector<LinkSpan*> links;
    std::function<void(Node&)> walk = [&](Node& n) {
        if (auto* l = dynamic_cast<LinkSpan*>(&n)) links.push_back(l);
        EXPECT_NE(n.role, Role::image);
        for (auto& c : n.children()) walk(*c);
    };
    walk(*md);
    ASSERT_EQ(links.size(), 2u);
    EXPECT_EQ(links[0]->url(), u"https://example.com/guide");
    EXPECT_EQ(links[0]->name, u"guide");
    EXPECT_EQ(links[1]->url(), u"https://tracker.example/x.png");
    EXPECT_FALSE(links[0]->frame().empty());
    EXPECT_GT(links[0]->frame().x, md->frame().x + 20);  // not at the start of the line
    s.tree.pointer({PointerEvent::down, links[0]->frame().x + 2, links[0]->frame().y + 2});
    s.tree.pointer({PointerEvent::up, links[0]->frame().x + 2, links[0]->frame().y + 2});
    EXPECT_EQ(s.opened, (std::vector<std::u16string>{u"https://example.com/guide"}));
    s.tree.set_focus(links[1]);
    s.tree.key({kKeyEnter});
    EXPECT_EQ(s.opened.size(), 2u);  // the window's open_link shows the target and asks before opening

    // The code block's Copy button copies the code and announces it.
    Node* copy = nullptr;
    for (Node* f : s.tree.focus_chain())
        if (f->automation_id().ends_with("-copy")) copy = f;
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->name, u"Copy code");
    copy->invoke();
    EXPECT_EQ(s.copied, (std::vector<std::u16string>{u"make all"}));
    EXPECT_EQ(s.announced.size(), 1u);
}

// client/work-inbox Message Text Selection (improve-desktop-look 1.3): a drag selects words, and across blocks in
// document order; a click without a drag clears it; one selection per window.
TEST(UiMarkdown, DragSelectsTextAcrossBlocksAndAClickClears) {
    Surface s(600, 600);
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Message");
    md->set_markdown("Alpha beta gamma delta.\n\n- one two\n- three\n");
    auto* other = s.content().emplace<MarkdownView>(*s.ui, u"Other");
    other->set_markdown("Another message.");
    s.tree.layout(s.r->size_dip());
    std::vector<RichText*> texts;
    std::function<void(Node&)> walk = [&](Node& n) {
        if (auto* r = dynamic_cast<RichText*>(&n)) texts.push_back(r);
        for (auto& c : n.children()) walk(*c);
    };
    walk(*md);
    ASSERT_EQ(texts.size(), 3u);
    RichText* para = texts[0];
    const float y = para->frame().y + para->frame().h / 2;
    // The x where a text position starts.
    const auto x_of = [&](uint32_t pos) {
        for (float x = para->frame().x; x < para->frame().right(); x += 0.5f)
            if (para->position_at(x, y) >= pos) return x;
        return para->frame().right();
    };
    EXPECT_EQ(para->cursor(), 3);  // the text cursor

    // Two words: "beta gamma".
    s.tree.pointer({PointerEvent::down, x_of(6) + 0.2f, y});
    s.tree.pointer({PointerEvent::move, x_of(16) + 0.2f, y});
    s.tree.pointer({PointerEvent::up, x_of(16) + 0.2f, y});
    EXPECT_EQ(md->selected_text(), u"beta gamma");
    EXPECT_EQ(s.ui->selection, md);

    // From the start of the paragraph to the end of the last list item, across three blocks.
    RichText* last = texts[2];
    s.tree.pointer({PointerEvent::down, para->frame().x + 0.5f, y});
    s.tree.pointer({PointerEvent::move, last->frame().right() - 1, last->frame().y + last->frame().h / 2});
    s.tree.pointer({PointerEvent::up, last->frame().right() - 1, last->frame().y + last->frame().h / 2});
    EXPECT_EQ(md->selected_text(), u"Alpha beta gamma delta.\none two\nthree");

    // Selecting in another message clears the first: one selection per window.
    RichText* o = dynamic_cast<RichText*>(other->children().front().get());
    ASSERT_NE(o, nullptr);
    const float oy = o->frame().y + o->frame().h / 2;
    s.tree.pointer({PointerEvent::down, o->frame().x + 0.5f, oy});
    s.tree.pointer({PointerEvent::move, o->frame().x + 40, oy});
    s.tree.pointer({PointerEvent::up, o->frame().x + 40, oy});
    EXPECT_EQ(s.ui->selection, other);
    EXPECT_TRUE(md->selected_text().empty());
    EXPECT_FALSE(other->selected_text().empty());

    // A click without a drag selects nothing.
    s.tree.pointer({PointerEvent::down, o->frame().x + 10, oy});
    s.tree.pointer({PointerEvent::up, o->frame().x + 10, oy});
    EXPECT_TRUE(other->selected_text().empty());
    EXPECT_EQ(s.ui->selection, nullptr);

    // A view that goes away gives up the selection.
    s.tree.pointer({PointerEvent::down, o->frame().x + 0.5f, oy});
    s.tree.pointer({PointerEvent::move, o->frame().x + 40, oy});
    s.tree.pointer({PointerEvent::up, o->frame().x + 40, oy});
    s.content().remove(other);
    EXPECT_EQ(s.ui->selection, nullptr);
}

TEST(UiMarkdown, StreamingRebuildsOnlyTheChangedTail) {
    Surface s(600, 600);
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Answer");
    std::string text = "First paragraph.\n\nSecond";
    md->set_markdown(text);
    EXPECT_EQ(md->rebuilt_last(), 2u);
    Node* first = md->children().front().get();
    text += " paragraph, still streaming";
    md->set_markdown(text);
    EXPECT_EQ(md->rebuilt_last(), 1u);
    EXPECT_EQ(md->children().front().get(), first);  // the finished block kept its element
    text += "\n\n```py\nprint(1)";
    md->set_markdown(text);
    EXPECT_EQ(md->rebuilt_last(), 1u);
    EXPECT_EQ(md->children().size(), 3u);
    s.tree.layout(s.r->size_dip());
    // Each prefix of a long answer lays out without trouble.
    const std::string full = "## Steps\n\n1. **Open** the file\n2. Run `make`\n\n| x | y |\n|---|---|\n| 1 | 2 |\n";
    for (size_t n = 0; n <= full.size(); n += 3) {
        md->set_markdown(std::string_view(full).substr(0, n));
        s.tree.layout(s.r->size_dip());
    }
}

// --- golden images ------------------------------------------------------------------------------------------------

TEST(UiGolden, ElementGallery) {
    if (!reference_fonts()) GTEST_SKIP() << "reference fonts (Segoe UI Variable, Segoe Fluent Icons) not installed";
    Surface s(480, 360);
    Node& c = s.content();
    c.padding = Insets::all(16);
    c.gap = 12;
    auto* row = c.emplace<Node>();
    row->axis = Axis::row;
    row->gap = 8;
    row->emplace<Button>(*s.ui, "primary", u"Approve", Button::Kind::primary, icon::check);
    row->emplace<Button>(*s.ui, "standard", u"Deny");
    row->emplace<IconButton>(*s.ui, "more", icon::more, u"More options", Button::Kind::standard);
    row->emplace<Badge>(*s.ui, u"3");
    c.emplace<Checkbox>(*s.ui, "check", u"Remember this choice", true);
    auto* card = c.emplace<Card>(*s.ui, "card", u"files.read", true, true);
    card->body().emplace<Label>(*s.ui, u"C:\\Users\\Example\\report.docx", TextStyle::code, Paint::text_secondary);
    auto* md = c.emplace<MarkdownView>(*s.ui, u"Answer");
    md->set_markdown("**Done.** See the [report](https://example.com) and `notes.txt`:\n\n- first\n- second\n");
    const auto px = s.render();
    // Sanity independent of the reference: the primary button is filled with the brand accent (Emerald in light).
    Node* primary = s.tree.focus_chain().front();
    EXPECT_EQ(s.at(px, primary->frame().x + 3, primary->frame().y + primary->frame().h / 2), 0x0E3A2Fu);
    expect_golden("element_gallery", px, s.r->width_px(), s.r->height_px());
}

TEST(UiGolden, MarkdownDarkHighDpi) {
    if (!reference_fonts()) GTEST_SKIP() << "reference fonts (Segoe UI Variable, Segoe Fluent Icons) not installed";
    Surface s(720, 540, 144);  // 150 %
    s.ui->theme = make_theme(true, false, kSys);
    Node& c = s.content();
    c.padding = Insets::all(12);
    auto* md = c.emplace<MarkdownView>(*s.ui, u"Answer");
    md->set_markdown(
        "## Result\n\nThe build **passed** with ~~two~~ no warnings.\n\n```cpp\nint main() { return 0; }\n```\n\n"
        "| Test | Time |\n|---|---|\n| ui_tests | 4 ms |\n\n> Note: *quoted*\n");
    const auto px = s.render();
    EXPECT_EQ(s.at(px, 2, 2), 0x131A17u);  // dark background
    EXPECT_GT(ink(px, s.r->width_px(), {0, 0, 720, 540}, 0x131A17), 2000u);
    expect_golden("markdown_dark_150", px, s.r->width_px(), s.r->height_px());
}

TEST(UiGolden, HighContrastUsesSystemColours) {
    Surface s(240, 80);
    const SystemColors hc{0x000000, 0xFFFFFF, 0x1AEBFF, 0x000000, 0xFFFF00, 0x3FF23F, 0x000000, 0xFFFFFF};
    s.ui->theme = make_theme(true, true, hc);
    s.content().padding = Insets::all(8);
    auto* b = s.content().emplace<Button>(*s.ui, "ok", u"OK", Button::Kind::primary);
    const auto px = s.render();
    EXPECT_EQ(s.at(px, 1, 1), 0x000000u);
    EXPECT_EQ(s.at(px, b->frame().x + 4, b->frame().y + b->frame().h / 2), 0x1AEBFFu);  // highlight, not the accent
}

// Ctrl+C copies the message selection while the selected view has keyboard focus; a press elsewhere moves the focus
// and clears the selection (design D8).
TEST(UiMarkdown, CtrlCCopiesTheSelectedWordsAndAPressElsewhereClearsThem) {
    Surface s(600, 600);
    std::vector<std::u16string> copied;
    s.ui->copy_text = [&copied](const std::u16string& t) { copied.push_back(t); };
    auto* md = s.content().emplace<MarkdownView>(*s.ui, u"Message");
    md->set_markdown("Alpha beta gamma delta.\n\n- one two\n- three\n");
    auto* other = s.content().emplace<Button>(*s.ui, "other-action", u"Other");
    s.tree.layout(s.r->size_dip());
    std::vector<RichText*> texts;
    std::function<void(Node&)> walk = [&](Node& n) {
        if (auto* r = dynamic_cast<RichText*>(&n)) texts.push_back(r);
        for (auto& c : n.children()) walk(*c);
    };
    walk(*md);
    ASSERT_EQ(texts.size(), 3u);
    RichText* para = texts[0];
    const float y = para->frame().y + para->frame().h / 2;
    const auto x_of = [&](uint32_t pos) {
        for (float x = para->frame().x; x < para->frame().right(); x += 0.5f)
            if (para->position_at(x, y) >= pos) return x;
        return para->frame().right();
    };
    // A press and drag over "beta gamma" gives the view keyboard focus.
    s.tree.pointer({PointerEvent::down, x_of(6) + 0.2f, y});
    s.tree.pointer({PointerEvent::move, x_of(16) + 0.2f, y});
    s.tree.pointer({PointerEvent::up, x_of(16) + 0.2f, y});
    EXPECT_EQ(s.tree.focused(), md);
    const KeyEvent ctrl_c{kKeyC, false, true, false};
    EXPECT_TRUE(copy_message_selection(*s.ui, ctrl_c));
    ASSERT_EQ(copied.size(), 1u);
    EXPECT_EQ(copied[0], u"beta gamma");
    EXPECT_FALSE(copy_message_selection(*s.ui, KeyEvent{kKeyC, true, true, false}));  // Ctrl+Shift+C is not a copy here
    EXPECT_FALSE(copy_message_selection(*s.ui, KeyEvent{0x56, false, true, false}));  // Ctrl+V

    // A press on another control moves the focus there and clears the selection.
    const float bx = other->frame().x + other->frame().w / 2, by = other->frame().y + other->frame().h / 2;
    s.tree.pointer({PointerEvent::down, bx, by});
    s.tree.pointer({PointerEvent::up, bx, by});
    EXPECT_EQ(s.tree.focused(), other);
    EXPECT_EQ(s.ui->selection, nullptr);
    EXPECT_TRUE(md->selected_text().empty());
    EXPECT_FALSE(copy_message_selection(*s.ui, ctrl_c));
    EXPECT_EQ(copied.size(), 1u);
}
