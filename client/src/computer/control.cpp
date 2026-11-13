#include "computer/control.h"

#include "computer/input.h"
#include "platform/log.h"

#include <cstdio>
#include <cstdlib>

namespace wb::computer {

namespace {

constexpr wchar_t kClass[] = L"SkoworkComputerOverlay";
constexpr COLORREF kKey = RGB(255, 0, 255);     // transparent
constexpr COLORREF kFrame = RGB(255, 140, 0);   // the target frame
constexpr COLORREF kBanner = RGB(32, 32, 32);
constexpr UINT kUpdate = WM_APP + 1;
constexpr UINT kUserInput = WM_APP + 2;
constexpr char kLog[] = "computer";

std::string hex_of(unsigned v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%X", v);
    return buf;
}
constexpr int kHotkeyId = 0x534B;

std::mutex g_mx;
ControlSession* g_session = nullptr;  // the hooks' target; one session at a time

// Virtual-key codes that belong to the stop hotkey: pressing it must stop, not count as taking over.
bool part_of_hotkey(const Hotkey& h, DWORD vk) {
    if (vk == h.vk) return true;
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return (h.modifiers & MOD_CONTROL) != 0;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return (h.modifiers & MOD_ALT) != 0;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return (h.modifiers & MOD_SHIFT) != 0;
    case VK_LWIN: case VK_RWIN: return (h.modifiers & MOD_WIN) != 0;
    default: return false;
    }
}

}  // namespace

ControlSession::ControlSession(Hotkey hotkey, ControlCallbacks callbacks) : hotkey_(std::move(hotkey)), callbacks_(std::move(callbacks)) {}

Result<std::unique_ptr<ControlSession>> ControlSession::start(const Hotkey& hotkey, ControlCallbacks callbacks) {
    {
        const std::lock_guard lock(g_mx);
        if (g_session) return make_error("busy", "another Run is controlling the desktop");
    }
    std::unique_ptr<ControlSession> s(new ControlSession(hotkey, std::move(callbacks)));
    std::promise<Status> ready;
    auto fut = ready.get_future();
    s->thread_ = std::thread([p = s.get(), &ready] { p->run(&ready); });
    Status st = fut.get();
    if (!st) {
        if (s->thread_.joinable()) s->thread_.join();
        return st.error();
    }
    return s;
}

ControlSession::~ControlSession() {
    if (thread_id_) ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void ControlSession::run(std::promise<Status>* ready) {
    thread_id_ = ::GetCurrentThreadId();
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's message queue
    {
        const std::lock_guard lock(g_mx);
        if (g_session) {
            ready->set_value(make_error("busy", "another Run is controlling the desktop"));
            return;
        }
        g_session = this;
    }
    const auto fail = [&](Error e) {
        const std::lock_guard lock(g_mx);
        g_session = nullptr;
        thread_id_ = 0;
        ready->set_value(std::move(e));
    };
    if (!::RegisterHotKey(nullptr, kHotkeyId, hotkey_.modifiers | MOD_NOREPEAT, hotkey_.vk))
        return fail(make_error("hotkey", "the stop hotkey " + hotkey_.text + " could not be registered (another application uses it)"));

    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &ControlSession::wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);  // fails harmlessly when already registered
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN), vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass,
                                  L"Skowork computer use", WS_POPUP, vx, vy, vw, vh, nullptr, nullptr, inst, this);
    if (!hwnd) {
        ::UnregisterHotKey(nullptr, kHotkeyId);
        return fail(make_error("overlay", "the overlay window could not be created"));
    }
    ::SetLayeredWindowAttributes(hwnd, kKey, 0, LWA_COLORKEY);
    // Never in the model's screenshots (design D6).
    ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    overlay_ = hwnd;
    HHOOK mouse = ::SetWindowsHookExW(WH_MOUSE_LL, &ControlSession::mouse_hook, inst, 0);
    HHOOK keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, &ControlSession::keyboard_hook, inst, 0);
    if (!mouse || !keyboard) {
        if (mouse) ::UnhookWindowsHookEx(mouse);
        if (keyboard) ::UnhookWindowsHookEx(keyboard);
        ::DestroyWindow(hwnd);
        overlay_ = nullptr;
        ::UnregisterHotKey(nullptr, kHotkeyId);
        return fail(make_error("hooks", "the input hooks could not be installed"));
    }
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ready->set_value(Status{});

    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
            fire(callbacks_.stop);
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == kUserInput) {
            const DWORD now = ::GetTickCount();
            if (last_user_fire_ == 0 || now - last_user_fire_ >= 500) {
                last_user_fire_ = now;
                // What took over, for support: a key (virtual-key code) or a mouse message, never the typed text.
                const auto what = static_cast<unsigned>(msg.wParam);
                log::info(kLog, (what & 0x10000 ? "user input: key 0x" + hex_of(what & 0xFFFF) : "user input: mouse 0x" + hex_of(what)) +
                                    " detail " + std::to_string(static_cast<long long>(msg.lParam)));
                fire(callbacks_.user_input);
            }
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::UnhookWindowsHookEx(mouse);
    ::UnhookWindowsHookEx(keyboard);
    ::UnregisterHotKey(nullptr, kHotkeyId);
    overlay_ = nullptr;
    ::DestroyWindow(hwnd);
    const std::lock_guard lock(g_mx);
    g_session = nullptr;
}

void ControlSession::fire(const std::function<void()>& cb) {
    // Callbacks cancel Runs and release input: never on the hook thread, whose hooks must answer at once.
    if (cb) std::thread(cb).detach();
}

void ControlSession::show(const std::wstring& banner, const Box& target) {
    {
        const std::lock_guard lock(mx_);
        banner_ = banner;
        target_ = target;
    }
    // Synchronous: the overlay shows the new target before the caller sends any input.
    if (HWND o = overlay_.load()) {
        DWORD_PTR r = 0;
        ::SendMessageTimeoutW(o, kUpdate, 0, 0, SMTO_BLOCK, 1000, &r);
    }
}

bool ControlSession::overlay_visible() const {
    HWND o = overlay_.load();
    if (!o || !::IsWindowVisible(o)) return false;
    DWORD affinity = 0;
    return ::GetWindowDisplayAffinity(o, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE;
}

void ControlSession::simulate_user_input() {
    if (thread_id_) ::PostThreadMessageW(thread_id_, kUserInput, 0, 0);
}

void ControlSession::paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = ::BeginPaint(hwnd, &ps);
    RECT client{};
    ::GetClientRect(hwnd, &client);
    HBRUSH key = ::CreateSolidBrush(kKey);
    ::FillRect(dc, &client, key);
    ::DeleteObject(key);
    std::wstring banner;
    Box t;
    {
        const std::lock_guard lock(mx_);
        banner = banner_;
        t = target_;
    }
    POINT origin{0, 0};
    ::ClientToScreen(hwnd, &origin);
    const UINT dpi = ::GetDpiForWindow(hwnd);
    const int thick = std::max(3, static_cast<int>(4 * dpi / 96));
    if (!t.empty()) {
        RECT r{t.x - origin.x - thick, t.y - origin.y - thick, t.x - origin.x + t.w + thick, t.y - origin.y + t.h + thick};
        HBRUSH frame = ::CreateSolidBrush(kFrame);
        RECT edges[4] = {{r.left, r.top, r.right, r.top + thick}, {r.left, r.bottom - thick, r.right, r.bottom},
                         {r.left, r.top, r.left + thick, r.bottom}, {r.right - thick, r.top, r.right, r.bottom}};
        for (const RECT& e : edges) ::FillRect(dc, &e, frame);
        ::DeleteObject(frame);
    }
    if (!banner.empty()) {
        const int h = static_cast<int>(30 * dpi / 96);
        HFONT font = ::CreateFontW(-static_cast<int>(14 * dpi / 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = ::SelectObject(dc, font);
        SIZE text{};
        ::GetTextExtentPoint32W(dc, banner.c_str(), static_cast<int>(banner.size()), &text);
        const int w = text.cx + 2 * h;
        // Above the target when there is room, else inside its top edge.
        int x = t.empty() ? (client.right - w) / 2 : t.x - origin.x + (t.w - w) / 2;
        int y = t.empty() ? h : t.y - origin.y - thick - h;
        if (y < 0) y = t.y - origin.y + thick;
        x = std::max(0, std::min(x, static_cast<int>(client.right) - w));
        RECT b{x, y, x + w, y + h};
        HBRUSH bg = ::CreateSolidBrush(kBanner);
        ::FillRect(dc, &b, bg);
        ::DeleteObject(bg);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(255, 255, 255));
        ::DrawTextW(dc, banner.c_str(), static_cast<int>(banner.size()), &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::DeleteObject(font);
    }
    ::EndPaint(hwnd, &ps);
}

LRESULT CALLBACK ControlSession::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<ControlSession*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT:
        if (self) {
            self->paint(hwnd);
            return 0;
        }
        break;
    case kUpdate:
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::InvalidateRect(hwnd, nullptr, TRUE);
        ::UpdateWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// The hooks run on the session thread and do the least possible: they recognise the user's input and post a message.
LRESULT CALLBACK ControlSession::mouse_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        ControlSession* s = g_session;
        const bool physical = !(m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) && m->dwExtraInfo != InputSender::kTag;
        if (s && wp == WM_MOUSEMOVE) {
            // A moved mouse counts once it travelled a few pixels within half a second (a desk vibration does not take
            // over). The travel is measured from the last position of any event: the agent's moves warp the pointer,
            // and the next physical event reports a position next to where the agent left it.
            const POINT before = s->last_seen_.x == LONG_MIN ? m->pt : s->last_seen_;
            s->last_seen_ = m->pt;
            if (physical) {
                const DWORD now = m->time;
                if (now - s->last_travel_ > 500) s->travel_ = 0;
                s->last_travel_ = now;
                s->travel_ += std::abs(m->pt.x - before.x) + std::abs(m->pt.y - before.y);
                if (s->travel_ > 4) {
                    ::PostThreadMessageW(s->thread_id_, kUserInput, WM_MOUSEMOVE, static_cast<LPARAM>(s->travel_));
                    s->travel_ = 0;
                }
            }
        } else if (s && physical) {
            ::PostThreadMessageW(s->thread_id_, kUserInput, wp, static_cast<LPARAM>(m->flags));
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK ControlSession::keyboard_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        ControlSession* s = g_session;
        if (s && !(k->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) && k->dwExtraInfo != InputSender::kTag &&
            !part_of_hotkey(s->hotkey_, k->vkCode))
            ::PostThreadMessageW(s->thread_id_, kUserInput, 0x10000 | k->vkCode, static_cast<LPARAM>(k->flags));
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace wb::computer
