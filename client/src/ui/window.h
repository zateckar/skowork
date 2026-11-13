// The native window host (add-native-ui design D1, D2, D7, D8): an HWND drawn by the software renderer, the
// element tree with its content and overlay layers, mouse and keyboard input, tooltips, per-monitor DPI, theme
// changes, link opening with confirmation, the clipboard, and UI Automation. Runs on the thread that creates it;
// that thread pumps messages.
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "ui/canvas.h"
#include "ui/device.h"
#include "ui/elements.h"
#include "ui/textfield.h"
#include "ui/uia.h"

namespace wb::ui {

// The caption band's parts, in DIPs, for frame_hit.
struct FrameLayout {
    float width = 0;
    float caption_height = 0;  // 0: the system draws the frame
    float top_border = 0;      // the resize band at the top while not maximised
    bool maximized = false;
    Rect minimize, maximize, close;
    bool over_control = false;  // the point is over a control of the pane under it (a button, a field, a splitter)
};

// Which part of the window a client point is, as a WM_NCHITTEST code (improve-window-frame D2): HTTOP, HTMINBUTTON,
// HTMAXBUTTON, HTCLOSE, HTCAPTION or HTCLIENT. The other edges are the system's own frame.
LRESULT frame_hit(const FrameLayout& f, float x, float y);

class Window : public UiaSite {
public:
    struct Options {
        std::wstring title = L"Skowork";
        int width = 1100, height = 720;  // DIPs
        std::string lang = "system";     // "cs", "de", "en" or "system"
        bool show = true;
        std::optional<Theme> theme;  // fixed theme (tests); otherwise Windows decides and changes are followed
        bool custom_frame = true;    // no system title bar: the panes' top row is the caption (improve-window-frame D1)
    };
    static Result<std::unique_ptr<Window>> create(const Options& options);
    ~Window() override;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    HWND hwnd() const override { return hwnd_; }
    Tree& tree() override { return tree_; }
    Ui& ui() { return *ui_; }
    Renderer& renderer() { return *renderer_; }
    Node& content() { return root_->content(); }
    OverlayLayer& overlay() { return root_->overlay(); }
    ModalLayer& modal() { return root_->modal(); }
    float scale() const { return renderer_->scale(); }
    void set_title(const std::wstring& title);
    WindowButtons* window_buttons() { return root_->window_buttons(); }  // null with the system title bar

    // Draws the pending dirty regions now (WM_PAINT does the same).
    void render();
    // Draws the whole window into an off-screen bitmap at 96 DPI, BGRA rows (golden-image tests of the screens).
    // Text fields are child windows and are not in it.
    Result<std::vector<uint32_t>> capture(UINT& width, UINT& height);
    // Announces text to screen readers (UI Automation notification event), e.g. a new Action item.
    void announce(const std::u16string& text);
    // Also told every announcement (the screen tests read them here).
    std::function<void(const std::u16string& text)> on_announce;
    void set_theme(const Theme& t);
    void close();
    // Runs fn once after ms on the window's thread (a debounced save); cancel_timer drops it. Timers end with the
    // window.
    UINT_PTR set_timer(UINT ms, std::function<void()> fn);
    void cancel_timer(UINT_PTR id);

    // Shown before a link opens; return true to open. Default: a message box with the target address.
    std::function<bool(const std::u16string& url)> confirm_link;
    // Opens a confirmed link. Default: the default browser (http, https and mailto only).
    std::function<void(const std::u16string& url)> launch_link;
    std::function<void()> on_closed;
    // Keys no element handled (window shortcuts); return true when handled. Also gets the keys text fields pass on.
    std::function<bool(const KeyEvent&)> on_key;
    // The window was activated or deactivated (the open Thread is "in focus" only while the window is active).
    std::function<void(bool active)> on_activate;
    // The user asked to close the window (the close box, Alt+F4); return false to keep it open. Default: closes.
    std::function<bool()> on_close_request;
    // Handles a key as the window would: the element tree first, then on_key.
    bool dispatch_key(const KeyEvent& e);

    // UiaSite
    RECT screen_rect(const Rect& dip) const override;
    Node* node_at_screen(POINT screen) override;
    Node* focus_target() override;
    void focus(Node& n) override;

private:
    Window() = default;
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void relayout_to_client();
    void pointer(PointerEvent::Kind kind, LPARAM lp, int button = 0);
    void hover_changed();
    void open_link(const std::u16string& url);
    void copy_text(const std::u16string& text);
    void place_fields();
    TextField* field_of(HWND h) const;

    HWND hwnd_ = nullptr;
    Tree tree_;
    std::unique_ptr<WindowRoot> root_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Ui> ui_;
    std::unique_ptr<Accessibility> uia_;
    bool fixed_theme_ = false;
    bool layout_posted_ = false;
    bool tracking_leave_ = false;
    uint32_t last_hover_ = 0;  // runtime ids: nodes may go away between messages
    uint32_t tooltip_ = 0;
    std::map<UINT_PTR, std::function<void()>> timers_;
    UINT_PTR next_timer_ = 0x100;
    void hide_tooltip();
    // The window's own frame (improve-window-frame D1, D2).
    bool custom_frame_ = false;
    HICON big_icon_ = nullptr, small_icon_ = nullptr;
    void set_icons();
    LRESULT nc_hit(LPARAM lp);
    CaptionButton* caption_button(LRESULT ht);
    void set_nc_hot(LRESULT ht, bool down);
    LRESULT nc_hot_ = 0;
    bool tracking_nc_leave_ = false;
};

}  // namespace wb::ui
