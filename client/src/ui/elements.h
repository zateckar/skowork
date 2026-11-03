// The elements of the native window (add-native-ui design D2): box, label, button, icon button, checkbox, card,
// badge, image, scroll view, splitter, and the overlay layer with menus and tooltips. The virtual list is in list.h
// and Markdown rich text in richtext.h. Every interactive element is focusable, has a UI Automation role and a name
// (D8), and draws its own focus state through the paint traversal.
#pragma once

#include <wincodec.h>

#include <functional>
#include <map>
#include <string>

#include "ui/canvas.h"

namespace wb::ui {

// Segoe Fluent Icons code points used by the window.
namespace icon {
constexpr char16_t add = 0xE710, settings = 0xE713, back = 0xE72B, chevron_down = 0xE70D, chevron_up = 0xE70E,
                   chevron_right = 0xE76C, copy = 0xE8C8, del = 0xE74D, more = 0xE712, search = 0xE721, cancel = 0xE711,
                   check = 0xE73E, pin = 0xE718, archive = 0xE7B8, send = 0xE724, stop = 0xE71A, folder = 0xE8B7,
                   warning = 0xE7BA, error = 0xEA39, info = 0xE946, attach = 0xE723, fork = 0xEA3C, rename = 0xE8AC,
                   add_people = 0xE8FA, note = 0xE70B, leave = 0xF3B1, chat = 0xE8BD, calendar_day = 0xE8BF,
                   calendar_week = 0xE8C0, history = 0xE81C, microphone = 0xE720;
}

// A container with an optional background, border and corner radius.
class Box : public Node {
public:
    explicit Box(std::string id = {}, Paint background = Paint::none, float radius = 0, Paint border = Paint::none);
    Paint background, border;
    float radius;
    void draw(Canvas& c) override;
};

// Plain text in one style. Wrapping labels fill their width; single-line labels fit their text and end with an
// ellipsis when cut. The accessible name is the text.
class Label : public Node {
public:
    Label(Ui& ui, std::u16string text, TextStyle style = TextStyle::body, Paint color = Paint::text, bool wrap = true,
          std::string id = {});
    void set_text(std::u16string text);
    const std::u16string& text() const { return text_; }
    Paint color;
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    std::u16string text_;
    TextStyle style_;
    bool wrap_;
};

class Button : public Node {
public:
    enum class Kind { standard, primary, subtle };
    // A button with a text label and an optional leading icon. An icon-only button (empty label) uses the name as
    // its tooltip.
    Button(Ui& ui, std::string id, std::u16string label, Kind kind = Kind::standard, char16_t glyph = 0);
    std::function<void()> on_click;
    void set_label(std::u16string label);
    // A text-sized button that lines up with caption labels (a status bar): caption text, no control height.
    void make_inline();
    bool can_invoke() const override { return enabled; }
    void invoke() override;
    void draw(Canvas& c) override;

protected:
    Ui& ui_;
    std::u16string label_;
    Kind kind_;
    char16_t glyph_;
    TextStyle text_style_ = TextStyle::body;
};

class IconButton : public Button {
public:
    // name is the accessible name and the tooltip.
    IconButton(Ui& ui, std::string id, char16_t glyph, std::u16string name, Kind kind = Kind::subtle);
};

class Checkbox : public Node {
public:
    Checkbox(Ui& ui, std::string id, std::u16string label, bool checked = false);
    bool checked() const { return checked_; }
    void set_checked(bool on);
    std::function<void(bool)> on_change;
    int toggle_state() const override { return checked_ ? 1 : 0; }
    void toggle() override { set_checked(!checked_); }
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    std::u16string label_;
    bool checked_;
};

// A card: a surface with a title row and a body. An expandable card's header is a button that expands and
// collapses the body (UI Automation ExpandCollapse).
class Card : public Box {
public:
    Card(Ui& ui, std::string id, std::u16string title, bool expandable = false, bool expanded = true);
    Node& header() { return *header_; }
    Node& body() { return *body_; }
    bool expanded() const { return body_->visible; }
    void set_expanded_state(bool on);
    std::function<void(bool)> on_expand;

private:
    class Header;
    Node* header_;
    Node* body_;
};

// A small pill with a count or a short state ("3", "Running").
class Badge : public Node {
public:
    Badge(Ui& ui, std::u16string text, Paint fill = Paint::accent, Paint color = Paint::accent_text, std::string id = {});
    void set_text(std::u16string text);
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    std::u16string text_;
    Paint fill_, color_;
};

// A round avatar with up to two initials of a name (brand look, improve-desktop-look D6). It is decorative: the
// sender's name stands next to it, so it has no UI Automation role.
class Avatar : public Node {
public:
    Avatar(Ui& ui, std::u16string_view who, Paint fill = Paint::surface_alt, Paint color = Paint::text, float size = 28);
    static std::u16string initials(std::u16string_view who);
    const std::u16string& text() const { return text_; }
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    std::u16string text_;
    Paint fill_, color_;
};

// Decodes an image (PNG, JPEG, GIF first frame, BMP) into premultiplied BGRA, scaled down so neither side exceeds
// max_side. Any thread with COM initialised (design D6: image decoding runs on a worker thread).
Result<com_ptr<IWICBitmapSource>> decode_image(const void* data, size_t size, UINT max_side = 4096);

// An image scaled to the available width (never up), keeping its aspect ratio, at most max_h DIPs high.
class ImageView : public Node {
public:
    ImageView(std::u16string alt, com_ptr<IWICBitmapSource> image, float max_h = 480, std::string id = {});
    void draw(Canvas& c) override;
    Size pixel_size() const { return {static_cast<float>(w_), static_cast<float>(h_)}; }

private:
    com_ptr<IWICBitmapSource> image_;
    com_ptr<ID2D1Bitmap1> bitmap_;
    uint64_t device_ = 0;
    UINT w_ = 0, h_ = 0;
    float max_h_;
};

// A vertically scrolling container with an overlay scroll bar that can be dragged.
class ScrollView : public Node {
public:
    explicit ScrollView(std::string id = {});
    void draw_over(Canvas& c) override;
    bool on_pointer(const PointerEvent& e) override;
    void on_hover(bool on) override;

private:
    Rect thumb() const;
    float drag_from_y_ = 0, drag_from_scroll_ = 0;
};

// A vertical bar that resizes `target` (its fixed width) by dragging or with the Left and Right keys.
class Splitter : public Node {
public:
    Splitter(std::string id, std::u16string name, Node& target, float min_w, float max_w, bool target_left = true);
    bool on_pointer(const PointerEvent& e) override;
    bool on_key(const KeyEvent& e) override;
    int cursor() const override { return 2; }
    void draw(Canvas& c) override;
    std::function<void(float)> on_resized;

private:
    void set_width(float w);
    Node& target_;
    float min_w_, max_w_;
    bool target_left_;
    float drag_x_ = 0, drag_w_ = 0;
};

// Popups (menus, tooltips) above the content, placed next to an anchor rectangle. A press outside every popup or
// Escape closes them, and keyboard focus returns where it was.
class OverlayLayer : public Node {
public:
    OverlayLayer();
    void show(std::unique_ptr<Node> popup, const Rect& anchor, bool take_focus);
    void close(Node* popup);
    void close_all();
    bool has_popup() const { return !children().empty(); }
    bool arrange_children(Tree& tree, const Rect& inner) override;
    bool on_pointer(const PointerEvent& e) override;
    bool on_key(const KeyEvent& e) override;

private:
    std::map<const Node*, Rect> anchors_;
    uint32_t restore_focus_ = 0;
};

class MenuItem;

class Menu : public Box {
public:
    explicit Menu(std::string id, std::u16string name);
    MenuItem* add_item(Ui& ui, std::string id, std::u16string label, std::function<void()> action, char16_t glyph = 0);
    bool on_key(const KeyEvent& e) override;
    bool arrange_children(Tree& tree, const Rect& inner) override;  // items as wide as the widest
};

class MenuItem : public Node {
public:
    MenuItem(Ui& ui, std::string id, std::u16string label, std::function<void()> action, char16_t glyph);
    bool can_invoke() const override { return enabled; }
    void invoke() override;
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    std::u16string label_;
    std::function<void()> action_;
    char16_t glyph_;
};

class Tooltip : public Box {
public:
    Tooltip(Ui& ui, std::u16string text);
};

// A modal dialog: a title, a body and a row of buttons at the end. Escape cancels. Shown by ModalLayer, which keeps
// the keyboard and the pointer inside it until it closes.
class Dialog : public Box {
public:
    Dialog(Ui& ui, std::string id, std::u16string title, float width = 440);
    Node& body() { return *body_; }
    // Adds a footer button; the first primary button is the default for Enter in the dialog's fields.
    Button* add_button(std::string id, std::u16string label, Button::Kind kind, std::function<void()> action);
    // Called when the user cancels (Escape); the default closes the dialog.
    std::function<void()> on_cancel;
    void close();
    bool on_key(const KeyEvent& e) override;
    Ui& ui() { return ui_; }

private:
    static std::string id_or(const std::string& id, const char* suffix);
    Ui& ui_;
    Node* body_;
    Node* footer_;
};

// The layer for modal dialogs, above the content and the popups; it dims what lies under the dialog.
class ModalLayer : public Node {
public:
    ModalLayer();
    Dialog* show(std::unique_ptr<Dialog> dialog);
    void close(Dialog* dialog);
    Dialog* top() const;
    bool arrange_children(Tree& tree, const Rect& inner) override;
    void draw(Canvas& c) override;

private:
    std::vector<uint32_t> restore_focus_;
};

// A window button (client/desktop-host Window Title Bar, improve-window-frame D3): Minimize,
// Maximize or Restore down, Close. Invoking it asks the window (WM_SYSCOMMAND). The pointer reaches it through the
// window's non-client messages, which set hot and down.
class CaptionButton : public Button {
public:
    enum class Action { minimize, maximize, close };
    CaptionButton(Ui& ui, Action action);
    Action action() const { return action_; }
    void sync();  // the name and glyph of Maximize follow the window's state
    void invoke() override;
    void draw(Canvas& c) override;
    bool hot = false, down = false;
    bool active = true;  // the window is active: an inactive window's glyphs are dimmed

private:
    Action action_;
};

// The window buttons at the window's top-right corner (improve-window-frame D3): a layer above everything that holds
// Minimize, Maximize and Close; the pointer passes through it elsewhere. There is no title bar row: the panes' top
// rows, kHeight high, are the caption band.
class WindowButtons : public Node {
public:
    static constexpr float kHeight = 40, kWidth = 3 * 46;
    explicit WindowButtons(Ui& ui);
    void set_active(bool active);
    void sync();  // after the window is maximised or restored
    CaptionButton& button(CaptionButton::Action a) { return *buttons_[static_cast<int>(a)]; }
    bool arrange_children(Tree& tree, const Rect& inner) override;

private:
    CaptionButton* buttons_[3]{};
};

// The end of a pane's header row: as wide as the window buttons while the row reaches the window's top-right corner,
// else nothing, so the header's controls stay clear of the buttons whichever pane is at the corner (D3).
class CaptionSpace : public Node {
public:
    CaptionSpace();
    bool arrange_children(Tree& tree, const Rect& inner) override;
};

// The Workbench mark: an Emerald rounded square with an Electric Green "S" (the app icon, improve-window-frame D4).
// Decorative: the name beside it, or the window's name, carries it.
class BrandMark : public Node {
public:
    BrandMark(Ui& ui, float size = 18);
    void draw(Canvas& c) override;

private:
    Ui& ui_;
    float size_;
};

// The window's root: the content filling the window; the overlay layer, the modal layer and, when the window draws its
// own frame, the window buttons lie on top of it.
class WindowRoot {
public:
    WindowRoot(Tree& tree, Ui* window_buttons_ui = nullptr);
    Node& content() { return *content_; }
    OverlayLayer& overlay() { return *overlay_; }
    ModalLayer& modal() { return *modal_; }
    WindowButtons* window_buttons() { return buttons_; }

private:
    Node* content_;
    OverlayLayer* overlay_;
    ModalLayer* modal_;
    WindowButtons* buttons_ = nullptr;
};

}  // namespace wb::ui
