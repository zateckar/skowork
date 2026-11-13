#include "ui/window.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ui/bundles.h"
#include "ui/list.h"
#include "ui/richtext.h"
#include "ui/text16.h"

namespace wb::ui {

namespace {
constexpr wchar_t kClass[] = L"SkoworkNativeWindow";
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT kTooltipDelayMs = 600;
constexpr UINT kLayoutMessage = WM_APP + 0x40;
constexpr long kUiaRootObjectId = -25;  // UiaRootObjectId

std::wstring wide(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }

bool openable(std::u16string_view url) {
    auto starts = [&](std::u16string_view p) {
        if (url.size() < p.size()) return false;
        for (size_t i = 0; i < p.size(); ++i) {
            char16_t c = url[i];
            if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c - u'A' + u'a');
            if (c != p[i]) return false;
        }
        return true;
    };
    return starts(u"https://") || starts(u"http://") || starts(u"mailto:");
}

Node* find_runtime(Node& n, uint32_t id) {
    if (n.runtime_id() == id) return &n;
    for (const auto& c : n.children())
        if (Node* f = find_runtime(*c, id)) return f;
    return nullptr;
}
}  // namespace

Result<std::unique_ptr<Window>> Window::create(const Options& o) {
    std::unique_ptr<Window> w(new Window());
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // The Skowork icon of the executable (client/desktop-host App Icon); none in the test programs.
    wc.hIcon = static_cast<HICON>(::LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXICON),
                                               ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.lpszClassName = kClass;
    ::RegisterClassExW(&wc);  // fails harmlessly when already registered

    // Per-monitor DPI awareness v2 for this window, whatever the process default is (D1).
    const DPI_AWARENESS_CONTEXT prev = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The DPI of the primary monitor (the system DPI reads 96 in a process that is not DPI-aware).
    UINT dpi = ::GetDpiForSystem();
    {
        UINT dx = 0, dy = 0;
        if (SUCCEEDED(::GetDpiForMonitor(::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy)) && dx)
            dpi = dx;
    }
    const float k = static_cast<float>(dpi) / 96.f;
    RECT rc{0, 0, static_cast<LONG>(std::lround(static_cast<float>(o.width) * k)), static_cast<LONG>(std::lround(static_cast<float>(o.height) * k))};
    ::AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    // With the custom frame the caption and top frame are client area (WM_NCCALCSIZE), so they are not added: the client
    // area is the requested size whatever the system's caption height.
    if (o.custom_frame) rc.top = 0;
    w->custom_frame_ = o.custom_frame;
    w->hwnd_ = ::CreateWindowExW(0, kClass, o.title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, w.get());
    if (prev) ::SetThreadDpiAwarenessContext(prev);
    if (!w->hwnd_) return make_error("ui_window", "CreateWindowExW failed");

    auto r = Renderer::for_window(w->hwnd_, ::GetDpiForWindow(w->hwnd_));
    if (!r) return r.error();
    w->renderer_ = std::move(r).value();

    const std::string lang = resolve_language(o.lang, system_language());
    auto ui = Ui::create(w->renderer_->dwrite(), lang);
    if (!ui) return ui.error();
    w->ui_ = std::move(ui).value();
    if (o.theme) {
        w->ui_->theme = *o.theme;
        w->fixed_theme_ = true;
    }
    apply_title_bar(w->hwnd_, w->ui_->theme);
    Window* self = w.get();
    w->ui_->open_link = [self](const std::u16string& url) { self->open_link(url); };
    w->ui_->copy_text = [self](const std::u16string& text) { self->copy_text(text); };
    w->ui_->announce = [self](const std::u16string& text) { self->announce(text); };
    w->ui_->host = w->hwnd_;
    w->ui_->scale = w->renderer_->scale();
    w->ui_->dispatch_key = [self](const KeyEvent& e) { return self->dispatch_key(e); };
    w->root_ = std::make_unique<WindowRoot>(w->tree_, o.custom_frame ? w->ui_.get() : nullptr);
    w->uia_ = std::make_unique<Accessibility>(self);
    w->tree_.on_dirty = [self] {
        if (!self->hwnd_) return;
        ::InvalidateRect(self->hwnd_, nullptr, FALSE);
        // A structure change is laid out from the message loop even when nothing paints (a hidden or minimised
        // window), so UI Automation clients see the current structure: a list realises its rows in layout. Posted,
        // not done here, so a layout never runs inside a change.
        if (self->tree_.layout_requested() && !self->layout_posted_) {
            self->layout_posted_ = true;
            ::PostMessageW(self->hwnd_, kLayoutMessage, 0, 0);
        }
    };
    w->tree_.on_focus_changed = [self](Node* n) {
        // A text field takes the keyboard into its own window (TextField::on_focus) and raises its own UI
        // Automation focus event; any other node takes it back to the window.
        if (n && n->native_window) return;
        HWND f = ::GetFocus();
        if (f && f != self->hwnd_ && ::IsChild(self->hwnd_, f)) ::SetFocus(self->hwnd_);
        if (::GetFocus() == self->hwnd_) self->uia_->focus_changed(self->focus_target());
    };
    w->set_icons();
    if (w->custom_frame_) {
        // The whole window is client area (WM_NCCALCSIZE); a one-pixel frame margin keeps the system border and shadow.
        const MARGINS m{0, 0, 1, 0};
        ::DwmExtendFrameIntoClientArea(w->hwnd_, &m);
        ::SetWindowPos(w->hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    w->relayout_to_client();
    if (o.show) {
        ::ShowWindow(w->hwnd_, SW_SHOWNORMAL);
        ::UpdateWindow(w->hwnd_);
    }
    return w;
}

Window::~Window() {
    if (hwnd_) {
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        if (uia_) uia_->detach();
        ::DestroyWindow(hwnd_);
    }
    if (big_icon_) ::DestroyIcon(big_icon_);
    if (small_icon_) ::DestroyIcon(small_icon_);
    // The elements go first: they use the Ui (text fields unregister from it), which is destroyed before the tree.
    timers_.clear();
    tree_.root().clear_children();
}

void Window::close() {
    if (hwnd_) ::DestroyWindow(hwnd_);
}

UINT_PTR Window::set_timer(UINT ms, std::function<void()> fn) {
    if (!hwnd_) return 0;
    const UINT_PTR id = next_timer_++;
    timers_[id] = std::move(fn);
    ::SetTimer(hwnd_, id, ms, nullptr);
    return id;
}

void Window::cancel_timer(UINT_PTR id) {
    if (timers_.erase(id) && hwnd_) ::KillTimer(hwnd_, id);
}

LRESULT CALLBACK Window::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* w = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w || !w->renderer_) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return w->handle(msg, wp, lp);
}

void Window::relayout_to_client() {
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    renderer_->resize(static_cast<UINT>(std::max<LONG>(1, rc.right)), static_cast<UINT>(std::max<LONG>(1, rc.bottom)));
    tree_.layout(renderer_->size_dip());
    tree_.invalidate_all();
}

void Window::set_title(const std::wstring& title) {
    if (hwnd_) ::SetWindowTextW(hwnd_, title.c_str());
}

// The window's icons at its DPI (the taskbar and Alt+Tab take the large one, the window menu the small one).
void Window::set_icons() {
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    const auto load = [&](int cx, int cy) {
        return static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetricsForDpi(cx, dpi),
                                               ::GetSystemMetricsForDpi(cy, dpi), LR_DEFAULTCOLOR));
    };
    HICON big = load(SM_CXICON, SM_CYICON), small = load(SM_CXSMICON, SM_CYSMICON);
    if (!big || !small) {
        if (big) ::DestroyIcon(big);
        if (small) ::DestroyIcon(small);
        return;
    }
    ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    if (big_icon_) ::DestroyIcon(big_icon_);
    if (small_icon_) ::DestroyIcon(small_icon_);
    big_icon_ = big;
    small_icon_ = small;
}

LRESULT frame_hit(const FrameLayout& f, float x, float y) {
    if (f.caption_height <= 0 || y >= f.caption_height) return HTCLIENT;
    if (!f.maximized && y < f.top_border) return HTTOP;
    if (f.close.contains(x, y)) return HTCLOSE;
    if (f.maximize.contains(x, y)) return HTMAXBUTTON;
    if (f.minimize.contains(x, y)) return HTMINBUTTON;
    return f.over_control ? HTCLIENT : HTCAPTION;
}

// Whether a point of the caption band is over a control (D2): the deepest node there, or an ancestor, takes pointer
// input of its own. Pane backgrounds, headings and labels are not controls, so the window moves from them.
static bool over_control(Tree& tree, float x, float y) {
    for (const Node* n = tree.hit_test(x, y); n; n = n->parent())
        if (n->enabled && (n->focusable || n->can_invoke() || n->native_window || n->cursor() != 0)) return true;
    return false;
}

LRESULT Window::nc_hit(LPARAM lp) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ::ScreenToClient(hwnd_, &p);
    WindowButtons* tb = window_buttons();
    const float k = renderer_->scale();
    const float x = static_cast<float>(p.x) / k, y = static_cast<float>(p.y) / k;
    FrameLayout f;
    f.width = renderer_->size_dip().w;
    f.caption_height = tb ? WindowButtons::kHeight : 0;
    f.maximized = ::IsZoomed(hwnd_) != FALSE;
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    f.top_border = static_cast<float>(::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)) / k;
    if (tb) {
        f.minimize = tb->button(CaptionButton::Action::minimize).frame();
        f.maximize = tb->button(CaptionButton::Action::maximize).frame();
        f.close = tb->button(CaptionButton::Action::close).frame();
        if (y < f.caption_height) f.over_control = over_control(tree_, x, y);
    }
    return frame_hit(f, x, y);
}

CaptionButton* Window::caption_button(LRESULT ht) {
    WindowButtons* tb = window_buttons();
    if (!tb) return nullptr;
    switch (ht) {
    case HTMINBUTTON: return &tb->button(CaptionButton::Action::minimize);
    case HTMAXBUTTON: return &tb->button(CaptionButton::Action::maximize);
    case HTCLOSE: return &tb->button(CaptionButton::Action::close);
    default: return nullptr;
    }
}

// The pointer over a window button (non-client messages): hot and pressed, and its tooltip after a moment.
void Window::set_nc_hot(LRESULT ht, bool down) {
    WindowButtons* tb = window_buttons();
    if (!tb) return;
    for (auto a : {CaptionButton::Action::minimize, CaptionButton::Action::maximize, CaptionButton::Action::close}) {
        CaptionButton& b = tb->button(a);
        const bool hot = caption_button(ht) == &b;
        if (b.hot != hot || b.down != (hot && down)) {
            b.hot = hot;
            b.down = hot && down;
            b.invalidate();
        }
    }
    if (ht != nc_hot_) {
        hide_tooltip();
        if (caption_button(ht)) ::SetTimer(hwnd_, kTooltipTimer, kTooltipDelayMs, nullptr);
    }
    nc_hot_ = ht;
}

bool Window::dispatch_key(const KeyEvent& e) {
    hide_tooltip();
    bool handled = tree_.key(e) || (on_key && on_key(e));
    // Ctrl+C copies the selected message text (improve-desktop-look D5); text fields copy their own selection.
    if (!handled && copy_message_selection(*ui_, e)) handled = true;
    if (handled) render();
    return handled;
}

TextField* Window::field_of(HWND h) const {
    for (TextField* f : ui_->fields)
        if (f->hwnd() == h) return f;
    return nullptr;
}

namespace {
bool shown_in(const Node& n, const Node& root) {
    const Node* p = &n;
    for (; p->parent(); p = p->parent())
        if (!p->visible) return false;
    return p == &root && p->visible;
}

bool inside(const Node* n, const Node* subtree) {
    for (; n; n = n->parent())
        if (n == subtree) return true;
    return false;
}
}  // namespace

// Places the text fields' child windows over their nodes. A field under a modal dialog, or under a popup, is
// hidden: child windows would otherwise cover what the window draws above them.
void Window::place_fields() {
    for (TextField* f : ui_->fields) {
        const bool attached = shown_in(*f, tree_.root());
        Rect vis = attached ? tree_.visible_rect(*f) : Rect{};
        bool allowed = attached;
        if (allowed && tree_.modal() && !inside(f, tree_.modal())) allowed = false;
        if (allowed && overlay().visible)
            for (const auto& p : overlay().children())
                if (!p->frame().intersect(vis).empty() && !inside(f, p.get())) allowed = false;
        f->place(vis, allowed);
    }
}

void Window::render() {
    tree_.layout_if_needed();
    place_fields();
    if (!tree_.has_dirty()) return;
    const std::vector<Rect> regions = renderer_->begin_frame(tree_.take_dirty());
    if (regions.empty()) return;
    {
        Canvas canvas(*renderer_, *ui_);
        paint(tree_, canvas, regions);
    }
    auto end = renderer_->end_frame();
    if (end && *end == Renderer::FrameEnd::recreated) tree_.invalidate_all();  // the renderer dropped its brushes
}

Result<std::vector<uint32_t>> Window::capture(UINT& width, UINT& height) {
    tree_.layout_if_needed();
    const Size s = tree_.viewport();
    width = static_cast<UINT>(std::lround(s.w));
    height = static_cast<UINT>(std::lround(s.h));
    auto r = Renderer::offscreen(width, height, 96);
    if (!r) return r.error();
    Renderer& off = **r;
    const std::vector<Rect> regions = off.begin_frame({Rect{0, 0, s.w, s.h}});
    {
        Canvas canvas(off, *ui_);
        paint(tree_, canvas, regions);
    }
    if (auto end = off.end_frame(); !end) return end.error();
    auto px = off.read_pixels();
    tree_.invalidate_all();  // text layouts took the off-screen device's brushes
    return px;
}

void Window::announce(const std::u16string& text) {
    if (on_announce) on_announce(text);
    uia_->notify(text);
}

void Window::set_theme(const Theme& t) {
    ui_->theme = t;
    fixed_theme_ = true;
    apply_title_bar(hwnd_, t);
    tree_.invalidate_all();
}

RECT Window::screen_rect(const Rect& d) const {
    const float k = renderer_->scale();
    POINT a{static_cast<LONG>(std::floor(d.x * k)), static_cast<LONG>(std::floor(d.y * k))};
    POINT b{static_cast<LONG>(std::ceil(d.right() * k)), static_cast<LONG>(std::ceil(d.bottom() * k))};
    ::ClientToScreen(hwnd_, &a);
    ::ClientToScreen(hwnd_, &b);
    return {a.x, a.y, b.x, b.y};
}

Node* Window::node_at_screen(POINT p) {
    ::ScreenToClient(hwnd_, &p);
    const float k = renderer_->scale();
    return tree_.hit_test(static_cast<float>(p.x) / k, static_cast<float>(p.y) / k);
}

Node* Window::focus_target() {
    Node* f = tree_.focused();
    if (auto* list = dynamic_cast<ListView*>(f); list && list->selected())
        if (ListRow* r = list->row(*list->selected())) return r;
    return f;
}

void Window::focus(Node& n) {
    ::SetFocus(hwnd_);
    tree_.set_focus(&n);
}

void Window::hide_tooltip() {
    ::KillTimer(hwnd_, kTooltipTimer);
    if (!tooltip_) return;
    for (const auto& c : overlay().children())
        if (c->runtime_id() == tooltip_) {
            overlay().close(c.get());
            break;
        }
    tooltip_ = 0;
}

void Window::hover_changed() {
    Node* h = tree_.hovered();
    const uint32_t id = h ? h->runtime_id() : 0;
    if (id == last_hover_) return;
    last_hover_ = id;
    hide_tooltip();
    if (h && !h->help.empty()) ::SetTimer(hwnd_, kTooltipTimer, kTooltipDelayMs, nullptr);
}

void Window::pointer(PointerEvent::Kind kind, LPARAM lp, int button) {
    const float k = renderer_->scale();
    PointerEvent e;
    e.kind = kind;
    e.x = static_cast<float>(GET_X_LPARAM(lp)) / k;
    e.y = static_cast<float>(GET_Y_LPARAM(lp)) / k;
    e.button = button;
    if (kind == PointerEvent::down) {
        hide_tooltip();
        // A press outside the message that holds the selection clears it.
        if (ui_->selection) {
            const Node* h = tree_.hit_test(e.x, e.y);
            while (h && h != ui_->selection) h = h->parent();
            if (!h) ui_->selection->clear_selection();
        }
    }
    tree_.pointer(e);
    hover_changed();
}

void Window::open_link(const std::u16string& url) {
    if (!openable(url)) {
        ::MessageBeep(MB_ICONWARNING);
        return;
    }
    bool ok;
    if (confirm_link) {
        ok = confirm_link(url);
    } else {
        const std::wstring title = wide(ui_->strings.get("link.confirm_title"));
        const std::wstring body = wide(ui_->strings.get("link.confirm_body", {{"url", url}}));
        ok = ::MessageBoxW(hwnd_, body.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    }
    if (!ok) return;
    if (launch_link) launch_link(url);
    else ::ShellExecuteW(hwnd_, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Window::copy_text(const std::u16string& text) {
    if (!::OpenClipboard(hwnd_)) return;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(char16_t);
    if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = ::GlobalLock(h)) {
            std::memcpy(p, text.c_str(), bytes);
            ::GlobalUnlock(h);
            if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
        } else {
            ::GlobalFree(h);
        }
    }
    ::CloseClipboard();
}

LRESULT Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kUiaCallMessage:
        if (Accessibility::run_call(wp, lp)) return 1;
        break;
    case kLayoutMessage:
        layout_posted_ = false;
        tree_.layout_if_needed();
        place_fields();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        ::BeginPaint(hwnd_, &ps);
        ::EndPaint(hwnd_, &ps);
        render();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    // The window's own title bar (improve-window-frame D1, D2).
    case WM_NCCALCSIZE:
        if (custom_frame_ && wp) {
            // The system keeps its resize frame on the left, right and bottom; the caption becomes client area. A
            // maximised window reaches past the monitor by the frame, which is taken back at the top.
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            const LONG top = p->rgrc[0].top;
            const LRESULT r = ::DefWindowProcW(hwnd_, msg, wp, lp);
            if (r != 0) return r;
            p->rgrc[0].top = top;
            if (::IsZoomed(hwnd_)) {
                const UINT dpi = ::GetDpiForWindow(hwnd_);
                p->rgrc[0].top += ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (custom_frame_) {
            // The system's frame on the left, right and bottom (and the top corners); the rest is ours. The system's
            // answer for the top is not used: it still places its own caption there.
            switch (const LRESULT sys = ::DefWindowProcW(hwnd_, msg, wp, lp)) {
            case HTLEFT:
            case HTRIGHT:
            case HTBOTTOM:
            case HTBOTTOMLEFT:
            case HTBOTTOMRIGHT:
            case HTTOPLEFT:
            case HTTOPRIGHT: return sys;
            default: return nc_hit(lp);
            }
        }
        break;
    case WM_NCACTIVATE:
        if (custom_frame_) {
            if (WindowButtons* tb = window_buttons()) tb->set_active(wp != FALSE);
            return ::DefWindowProcW(hwnd_, msg, wp, -1);  // -1: Windows paints no caption of its own
        }
        break;
    case WM_NCMOUSEMOVE:
        if (custom_frame_) {
            if (!tracking_nc_leave_) {
                TRACKMOUSEEVENT t{};
                t.cbSize = sizeof t;
                t.dwFlags = TME_LEAVE | TME_NONCLIENT;
                t.hwndTrack = hwnd_;
                tracking_nc_leave_ = ::TrackMouseEvent(&t);
            }
            const LRESULT ht = static_cast<LRESULT>(wp);
            set_nc_hot(caption_button(ht) ? ht : 0, caption_button(ht) && caption_button(ht)->down);
            if (caption_button(ht)) return 0;
        }
        break;
    case WM_NCMOUSELEAVE:
        tracking_nc_leave_ = false;
        if (custom_frame_) set_nc_hot(0, false);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (custom_frame_ && caption_button(static_cast<LRESULT>(wp))) {
            set_nc_hot(static_cast<LRESULT>(wp), true);
            return 0;  // Windows must not draw buttons of its own
        }
        break;
    case WM_NCLBUTTONUP:
        if (custom_frame_)
            if (CaptionButton* b = caption_button(static_cast<LRESULT>(wp))) {
                const bool was_down = b->down;
                set_nc_hot(static_cast<LRESULT>(wp), false);
                if (was_down) b->invoke();
                return 0;
            }
        break;
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
        if (custom_frame_ && caption_button(static_cast<LRESULT>(wp))) return 0;
        break;
    case WM_SIZE:
        if (WindowButtons* tb = window_buttons()) tb->sync();  // Maximize or Restore down
        if (wp != SIZE_MINIMIZED && renderer_ && ui_) {
            renderer_->resize(LOWORD(lp), HIWORD(lp));
            tree_.layout(renderer_->size_dip());
            render();
        }
        return 0;
    case WM_DPICHANGED: {
        renderer_->set_dpi(HIWORD(wp));
        ui_->scale = renderer_->scale();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        ::SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        set_icons();
        relayout_to_client();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        if (is_theme_change(msg, wp, lp) && !fixed_theme_) {
            ui_->theme = current_theme();
            apply_title_bar(hwnd_, ui_->theme);
            tree_.invalidate_all();
        }
        break;
    case WM_GETOBJECT:
        if (static_cast<long>(lp) == static_cast<long>(kUiaRootObjectId)) return uia_->on_getobject(wp, lp);
        break;
    case WM_SETFOCUS:
        if (!tree_.focused()) tree_.set_focus(tree_.next_focus(nullptr));
        // Back from another application: the keyboard returns to the text field that had it.
        if (auto* f = dynamic_cast<TextField*>(tree_.focused()); f && f->hwnd()) {
            f->on_focus(true);
            return 0;
        }
        uia_->focus_changed(focus_target());
        tree_.invalidate(tree_.focused() ? tree_.focused()->focus_rect().inset(-4.f) : Rect{});
        return 0;
    case WM_KILLFOCUS: hide_tooltip(); return 0;
    case WM_MOUSEMOVE:
        if (!tracking_leave_) {
            TRACKMOUSEEVENT t{};
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd_;
            tracking_leave_ = ::TrackMouseEvent(&t);
        }
        pointer(PointerEvent::move, lp);
        return 0;
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        pointer(PointerEvent::leave, lp);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        ::SetFocus(hwnd_);
        ::SetCapture(hwnd_);
        pointer(PointerEvent::down, lp, 0);
        return 0;
    case WM_LBUTTONUP:
        ::ReleaseCapture();
        pointer(PointerEvent::up, lp, 0);
        return 0;
    case WM_RBUTTONDOWN:
        ::SetFocus(hwnd_);
        pointer(PointerEvent::down, lp, 1);
        return 0;
    case WM_RBUTTONUP: pointer(PointerEvent::up, lp, 1); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ::ScreenToClient(hwnd_, &p);
        UINT lines = 3;
        ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const float k = renderer_->scale();
        PointerEvent e;
        e.kind = PointerEvent::wheel;
        e.x = static_cast<float>(p.x) / k;
        e.y = static_cast<float>(p.y) / k;
        e.wheel_dy = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * static_cast<float>(std::min(lines, 10u)) * 16.f;
        hide_tooltip();
        tree_.pointer(e);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            ::GetCursorPos(&p);
            int c = 0;
            for (Node* n = node_at_screen(p); n && c == 0; n = n->parent()) c = n->cursor();
            const wchar_t* id = c == 1 ? IDC_HAND : c == 2 ? IDC_SIZEWE : c == 3 ? IDC_IBEAM : IDC_ARROW;
            ::SetCursor(::LoadCursorW(nullptr, id));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        hide_tooltip();
        KeyEvent e;
        e.key = static_cast<unsigned>(wp);
        e.shift = ::GetKeyState(VK_SHIFT) < 0;
        e.ctrl = ::GetKeyState(VK_CONTROL) < 0;
        e.alt = msg == WM_SYSKEYDOWN || ::GetKeyState(VK_MENU) < 0;
        if (e.alt && e.key == VK_F4) break;  // the system closes the window
        if (dispatch_key(e)) return 0;
        break;
    }
    case WM_COMMAND:
        if (lp)
            if (TextField* f = field_of(reinterpret_cast<HWND>(lp))) {
                f->on_command(HIWORD(wp));
                return 0;
            }
        break;
    case WM_NOTIFY: {
        const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (TextField* f = field_of(hdr->hwndFrom)) {
            f->on_notify(hdr);
            return 1;  // the field handled dropped files itself
        }
        break;
    }
    case WM_ACTIVATE:
        if (WindowButtons* tb = window_buttons()) tb->set_active(LOWORD(wp) != WA_INACTIVE);
        if (on_activate) on_activate(LOWORD(wp) != WA_INACTIVE);
        break;
    case WM_CLOSE:
        if (on_close_request && !on_close_request()) return 0;
        break;
    case WM_TIMER:
        if (wp == kTooltipTimer) {
            ::KillTimer(hwnd_, kTooltipTimer);
            Node* h = caption_button(nc_hot_) ? caption_button(nc_hot_) : tree_.hovered();
            if (h && !h->help.empty() && !tooltip_) {
                auto tip = std::make_unique<Tooltip>(*ui_, h->help);
                tooltip_ = tip->runtime_id();
                overlay().show(std::move(tip), h->frame(), false);
            }
            return 0;
        }
        if (auto it = timers_.find(wp); it != timers_.end()) {
            ::KillTimer(hwnd_, wp);
            const std::function<void()> fn = std::move(it->second);
            timers_.erase(it);
            if (fn) fn();
            return 0;
        }
        break;
    case WM_DESTROY:
        ::KillTimer(hwnd_, kTooltipTimer);
        for (const auto& [id, fn] : timers_) ::KillTimer(hwnd_, id);
        timers_.clear();
        uia_->detach();
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        if (big_icon_) ::DestroyIcon(big_icon_);
        if (small_icon_) ::DestroyIcon(small_icon_);
        big_icon_ = small_icon_ = nullptr;
        if (on_closed) on_closed();
        return 0;
    default: break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace wb::ui
