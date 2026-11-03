// A text field of the native window (add-native-ui design D3): a rich-edit child window (Msftedit RICHEDIT50W) with
// the system's IME, spell checking, clipboard and undo, placed by the element tree over its node. The window draws
// the field's frame and placeholder; the child window is shown only while the field has focus or holds text, so
// popups and dialogs drawn by the window are not covered by an idle field. Keys the field does not use (Tab,
// Escape, Enter in a single-line field, window shortcuts) go to the element tree as if the window had them.
//
// Screen readers see the child window itself (its own UI Automation provider), named from `name` through dynamic
// annotation. Without a host window (off-screen tests) the field keeps its text in memory.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/canvas.h"

namespace wb::ui {

class TextField : public Node {
public:
    struct Options {
        bool multiline = false;
        std::u16string placeholder;
        size_t max_length = 0;  // characters; 0: no limit beyond the control's
        int max_lines = 8;      // a multi-line field grows with its text up to this many lines, then scrolls
        int min_lines = 1;
    };
    TextField(Ui& ui, std::string id, std::u16string label, Options options);
    TextField(Ui& ui, std::string id, std::u16string label) : TextField(ui, std::move(id), std::move(label), Options{}) {}
    ~TextField() override;

    std::u16string text() const;
    void set_text(const std::u16string& text);  // does not call on_change
    void set_placeholder(std::u16string p);
    bool empty() const { return text().empty(); }
    void select_all();
    // Inserts text at the caret (a skill invocation chosen from a list).
    void insert(const std::u16string& text);
    // The character before the caret; 0 at the start (dictated text is separated from the text before it).
    char16_t char_before_caret() const;

    // Called after every user edit.
    std::function<void()> on_change;
    // Called for every key pressed in the field before it acts; return true to swallow the key (Enter to send).
    std::function<bool(const KeyEvent&)> on_key_down;
    // Pasted or dropped files (the composer attaches them); empty when the field does not take files.
    std::function<void(const std::vector<std::wstring>& paths)> on_files;

    HWND hwnd() const { return edit_; }
    bool has_window_focus() const;

    void draw(Canvas& c) override;
    void on_focus(bool on) override;
    int cursor() const override { return 3; }

    // The window's placement pass: the visible rectangle of the node (DIPs) and whether it may be shown now.
    void place(const Rect& vis, bool allowed);
    // Routing from the host window (WM_COMMAND and WM_NOTIFY notifications of the child window).
    void on_command(UINT code);
    void on_notify(const NMHDR* hdr);

private:
    bool visible_rect_ok(const Rect& vis) const;
    static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT edit_message(UINT msg, WPARAM wp, LPARAM lp);
    void create_window();
    void apply_theme();
    void update_height();
    float line_height() const;

    Ui& ui_;
    Options opt_;
    HWND edit_ = nullptr;
    WNDPROC base_proc_ = nullptr;
    std::u16string offline_text_;  // without a window
    int lines_ = 1;
    uint32_t applied_text_ = 0xFFFFFFFF, applied_bg_ = 0xFFFFFFFF;
    bool shown_ = false;
    bool focusing_ = false;
    bool hiding_ = false;
    bool swallow_char_ = false;  // the WM_CHAR of a key the field passed on
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);  // a handler can tell whether it destroyed the field
};

}  // namespace wb::ui
