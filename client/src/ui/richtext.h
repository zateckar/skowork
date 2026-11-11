// Markdown rendering in the Thread feed (client/work-inbox Markdown Rendering; add-native-ui design D4): the blocks
// of parse_markdown() become elements: styled text (bold, italic, strikethrough, inline code, links), headings,
// list items with markers, block quotes, code blocks in a monospace font with Copy, tables and rules.
//
// Model output is untrusted. Raw HTML arrives as literal text from the parser; images arrive as alternative text
// and a link and are never fetched. A link is a focusable child element: clicking it or pressing Enter calls
// Ui::open_link, which shows the target and asks before opening it in the browser.
//
// While a message streams, set_markdown() is called with the whole text so far: blocks that did not change keep
// their elements (and their text layouts), and only the changed tail is rebuilt.
#pragma once

#include "ui/canvas.h"
#include "ui/markdown.h"

namespace wb::ui {

// One block of styled text. Its text can be part of a selection (improve-desktop-look D5), which MarkdownView drives.
class RichText : public Node {
public:
    RichText(Ui& ui, std::u16string text, std::vector<Span> spans, TextStyle base = TextStyle::body,
             Paint color = Paint::text, std::string id = {});
    const std::u16string& text() const { return text_; }
    void draw(Canvas& c) override;
    bool arrange_children(Tree& tree, const Rect& inner) override;
    int cursor() const override { return 3; }
    // The text position nearest to a point (window DIPs), clamped to the text.
    uint32_t position_at(float x, float y);
    // The selected range [from, to), empty when from == to.
    void set_selection(uint32_t from, uint32_t to);
    uint32_t selection_from() const { return sel_from_; }
    uint32_t selection_to() const { return sel_to_; }

private:
    Rect inner() const;
    uint32_t sel_from_ = 0, sel_to_ = 0;
    IDWriteTextLayout* layout_for(float w);
    Ui& ui_;
    std::u16string text_;
    std::vector<Span> spans_;
    TextStyle base_;
    Paint color_;
    com_ptr<IDWriteTextLayout> layout_;
    float layout_w_ = -1;
};

// A link inside rich text: positioned over its text, focusable, opens through Ui::open_link.
class LinkSpan : public Node {
public:
    LinkSpan(Ui& ui, std::u16string text, std::u16string url, uint32_t start, uint32_t length);
    const std::u16string& url() const { return url_; }
    uint32_t start() const { return start_; }
    uint32_t length() const { return length_; }
    bool can_invoke() const override { return true; }
    void invoke() override;
    int cursor() const override { return 1; }
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    std::u16string url_;
    uint32_t start_, length_;
};

// A message rendered from Markdown. A drag over its text selects it, across its blocks in document order (client/
// work-inbox Message Text Selection, improve-desktop-look D5); the window copies the selection with Ctrl+C. One
// selection exists per window: Ui::selection names the view that holds it.
// Ctrl+C that no node handled copies the message selection; true when it did (design D8). A text field copies its own
// selection before this is asked.
bool copy_message_selection(Ui& ui, const KeyEvent& e);

class MarkdownView : public Node {
public:
    MarkdownView(Ui& ui, std::u16string name, std::string id = {});
    ~MarkdownView() override;
    void set_markdown(std::string_view utf8);
    // Text shown as written, one paragraph, without Markdown (the user's own messages), and still selectable.
    void set_plain(std::u16string text);
    const std::vector<Block>& blocks() const { return blocks_; }
    std::u16string value() const override { return plain_; }
    size_t rebuilt_last() const { return rebuilt_; }  // blocks rebuilt by the last set_markdown (tests)
    size_t sets() const { return sets_; }             // set_markdown calls that re-parsed (tests)
    bool on_pointer(const PointerEvent& e) override;
    // The selected text as plain text, the parts of different blocks on lines of their own; empty when none.
    std::u16string selected_text() const;
    void clear_selection();
    // Focus leaves the view: its selection goes too, so Ctrl+C never copies text the user cannot see selected.
    void on_focus(bool on) override;

private:
    struct Point {
        size_t text = 0;  // index into texts()
        uint32_t pos = 0;
        bool operator==(const Point&) const = default;
    };
    std::vector<RichText*> texts() const;
    Point point_at(float x, float y) const;
    void apply_selection();
    std::unique_ptr<Node> build(const Block& b, size_t index);
    void set_blocks(std::vector<Block> next);
    Point anchor_, focus_;
    bool selecting_ = false, moved_ = false;
    bool plain_text_ = false;  // set_plain: the one block is named after the view
    Ui& ui_;
    std::vector<Block> blocks_;
    std::u16string plain_;
    size_t rebuilt_ = 0;
    size_t sets_ = 0;
};

}  // namespace wb::ui
