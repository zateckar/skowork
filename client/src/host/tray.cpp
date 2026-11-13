#include "host/tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wb {
namespace {

constexpr UINT kIconId = 1;

enum MenuId : UINT {
    kOpen = 100,
    kNewThread,
    kPause15,
    kPause60,
    kPause240,
    kResume,
    kStatus,
    kQuit,
};

// ARGB colours per state: grey, green, blue, orange, amber.
constexpr std::uint32_t kColours[5] = {0xFF8A8F98, 0xFF2E9E5B, 0xFF2F6FDE, 0xFFE8731A, 0xFFC9A227};

struct Px {
    double r = 0, g = 0, b = 0, a = 0;  // straight alpha, 0..1
};

// Paints an anti-aliased disc (a ring when hole > 0) over the image.
void disc(std::vector<Px>& img, int size, double cx, double cy, double r, std::uint32_t rgb, double hole = 0) {
    const double cr = ((rgb >> 16) & 0xFF) / 255.0, cg = ((rgb >> 8) & 0xFF) / 255.0, cb = (rgb & 0xFF) / 255.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double d = std::hypot(x - cx, y - cy);
            double cov = std::clamp(r - d + 0.5, 0.0, 1.0);
            if (hole > 0) cov *= std::clamp(d - hole + 0.5, 0.0, 1.0);
            if (cov <= 0) continue;
            Px& p = img[static_cast<size_t>(y * size + x)];
            const double a = cov + p.a * (1 - cov);
            p.r = (cr * cov + p.r * p.a * (1 - cov)) / a;
            p.g = (cg * cov + p.g * p.a * (1 - cov)) / a;
            p.b = (cb * cov + p.b * p.a * (1 - cov)) / a;
            p.a = a;
        }
}

HICON make_icon(const std::vector<std::uint32_t>& argb, int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP color = ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    std::memcpy(bits, argb.data(), argb.size() * sizeof(std::uint32_t));
    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&ii);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;
}

// The app icon (ICON 1 of the executable) at size x size, straight-alpha ARGB; empty without the resource.
std::vector<std::uint32_t> app_icon(int size) {
    auto* icon = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!icon) return {};
    std::vector<std::uint32_t> px;
    ICONINFO ii{};
    if (::GetIconInfo(icon, &ii)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        px.resize(static_cast<size_t>(size) * static_cast<size_t>(size));
        HDC dc = ::GetDC(nullptr);
        if (!ii.hbmColor || ::GetDIBits(dc, ii.hbmColor, 0, static_cast<UINT>(size), px.data(), &bi, DIB_RGB_COLORS) != size) px.clear();
        ::ReleaseDC(nullptr, dc);
        if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
        if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
    }
    ::DestroyIcon(icon);
    return px;
}

}  // namespace

std::uint32_t tray_colour(TrayState s) { return kColours[static_cast<int>(s)]; }

std::vector<std::uint32_t> tray_image(TrayState s, bool badge, const std::vector<std::uint32_t>& base, int size) {
    const size_t n = static_cast<size_t>(size) * static_cast<size_t>(size);
    std::vector<Px> img(n);
    const std::uint32_t colour = tray_colour(s) & 0xFFFFFF;
    const bool paused = s == TrayState::paused;
    if (base.size() == n) {
        for (size_t i = 0; i < n; ++i)
            img[i] = {((base[i] >> 16) & 0xFF) / 255.0, ((base[i] >> 8) & 0xFF) / 255.0, (base[i] & 0xFF) / 255.0, ((base[i] >> 24) & 0xFF) / 255.0};
        // The state dot in the bottom-right corner, set off from the icon by a dark rim.
        const double r = size * 0.24, c = size - r - 0.5;
        disc(img, size, c, c, r + 1.0, 0x131A17);
        disc(img, size, c, c, r, colour, paused ? r * 0.45 : 0);
    } else {
        // No app icon: the disc in the state's colour; a hollow ring marks the paused state.
        const double c = (size - 1) / 2.0, r = size / 2.0 - 0.5;
        disc(img, size, c, c, r, colour, paused ? r * 0.45 : 0);
    }
    // Unread Chat messages: a red dot with a white rim in the top right corner.
    if (badge) {
        const double br = size * 0.24, bx = size - br - 0.5, by = br + 0.5;
        disc(img, size, bx, by, br + 1.0, 0xFFFFFF);
        disc(img, size, bx, by, br, 0xD13438);
    }
    std::vector<std::uint32_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        const auto ch = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
        out[i] = (ch(img[i].a) << 24) | (ch(img[i].r) << 16) | (ch(img[i].g) << 8) | ch(img[i].b);
    }
    return out;
}
const char* to_string(TrayState s) {
    switch (s) {
    case TrayState::disconnected: return "disconnected";
    case TrayState::idle: return "idle";
    case TrayState::running: return "running";
    case TrayState::action_required: return "action_required";
    case TrayState::paused: return "paused";
    }
    return "unknown";
}

TrayState compute_tray_state(bool connected, bool paused, int action_items, int active_runs) {
    if (action_items > 0) return TrayState::action_required;
    if (paused) return TrayState::paused;
    if (!connected) return TrayState::disconnected;
    if (active_runs > 0) return TrayState::running;
    return TrayState::idle;
}

std::wstring tray_tooltip(TrayState s, int action_items, int active_runs, bool content_telemetry, int unread_messages) {
    std::wstring t = L"Skowork";
    switch (s) {
    case TrayState::disconnected: t += L" – disconnected"; break;
    case TrayState::idle: t += L" – ready"; break;
    case TrayState::running: t += L" – " + std::to_wstring(active_runs) + L" running"; break;
    case TrayState::action_required:
        t += L" – " + std::to_wstring(action_items) + (action_items == 1 ? L" item needs" : L" items need") +
             L" your action";
        break;
    case TrayState::paused: t += L" – background work paused"; break;
    }
    if (unread_messages > 0)
        t += L"\n" + std::to_wstring(unread_messages) + (unread_messages == 1 ? L" unread message" : L" unread messages");
    if (content_telemetry) t += L"\nContent telemetry active";
    return t;
}

Tray::Tray(HWND owner, Callbacks callbacks) : owner_(owner), cb_(std::move(callbacks)) {
    const int size = ::GetSystemMetrics(SM_CXSMICON);
    const std::vector<std::uint32_t> base = app_icon(size);
    for (int i = 0; i < 5; ++i) {
        icons_[i] = make_icon(tray_image(static_cast<TrayState>(i), false, base, size), size);
        badged_[i] = make_icon(tray_image(static_cast<TrayState>(i), true, base, size), size);
    }
    taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    tooltip_ = tray_tooltip(state_, 0, 0);
    add_icon();
}

Tray::~Tray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = owner_;
    nid.uID = kIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    for (HICON i : icons_)
        if (i) ::DestroyIcon(i);
    for (HICON i : badged_)
        if (i) ::DestroyIcon(i);
}

void Tray::add_icon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallbackMessage;
    nid.hIcon = (unread_ > 0 ? badged_ : icons_)[static_cast<int>(state_)];
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void Tray::update_icon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = (unread_ > 0 ? badged_ : icons_)[static_cast<int>(state_)];
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::set(TrayState state, int action_items, int active_runs, bool paused, bool content_telemetry, int unread_messages) {
    std::wstring tip = tray_tooltip(state, action_items, active_runs, content_telemetry, unread_messages);
    paused_ = paused;
    if (state == state_ && tip == tooltip_ && (unread_messages > 0) == (unread_ > 0)) return;
    state_ = state;
    unread_ = unread_messages;
    tooltip_ = std::move(tip);
    update_icon();
}

bool Tray::confirm_quit() {
    const int runs = cb_.active_runs ? cb_.active_runs() : 0;
    if (runs == 0) return true;
    const std::wstring text = std::to_wstring(runs) +
                              (runs == 1 ? L" task is still running." : L" tasks are still running.") +
                              L"\n\nQuitting stops them. Quit Skowork anyway?";
    return ::MessageBoxW(owner_, text.c_str(), L"Quit Skowork", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                                                                     MB_SETFOREGROUND) == IDYES;
}

void Tray::show_menu() {
    HMENU pause = ::CreatePopupMenu();
    ::AppendMenuW(pause, MF_STRING, kPause15, L"For 15 minutes");
    ::AppendMenuW(pause, MF_STRING, kPause60, L"For 1 hour");
    ::AppendMenuW(pause, MF_STRING, kPause240, L"For 4 hours");
    HMENU m = ::CreatePopupMenu();
    ::AppendMenuW(m, MF_STRING, kOpen, L"Open");
    ::AppendMenuW(m, MF_STRING, kNewThread, L"New thread");
    if (paused_)
        ::AppendMenuW(m, MF_STRING, kResume, L"Resume background work");
    else
        ::AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(pause), L"Pause background work");
    ::AppendMenuW(m, MF_STRING, kStatus, L"Status");
    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, MF_STRING, kQuit, L"Quit");
    ::SetMenuDefaultItem(m, kOpen, FALSE);
    POINT pt;
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(owner_);
    const UINT cmd = static_cast<UINT>(
        ::TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, owner_, nullptr));
    ::PostMessageW(owner_, WM_NULL, 0, 0);
    ::DestroyMenu(m);  // destroys the submenu too when attached
    if (paused_) ::DestroyMenu(pause);
    switch (cmd) {
    case kOpen: if (cb_.open) cb_.open(); break;
    case kNewThread: if (cb_.new_thread) cb_.new_thread(); break;
    case kPause15: if (cb_.pause) cb_.pause(15); break;
    case kPause60: if (cb_.pause) cb_.pause(60); break;
    case kPause240: if (cb_.pause) cb_.pause(240); break;
    case kResume: if (cb_.pause) cb_.pause(0); break;
    case kStatus: if (cb_.status) cb_.status(); break;
    case kQuit:
        if (confirm_quit() && cb_.quit) cb_.quit();
        break;
    default: break;
    }
}

bool Tray::handle_message(UINT msg, WPARAM, LPARAM l) {
    if (msg == taskbar_created_ && taskbar_created_ != 0) {
        add_icon();  // Explorer restarted
        return true;
    }
    if (msg != kCallbackMessage) return false;
    switch (LOWORD(l)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK:
        if (cb_.open) cb_.open();
        break;
    case WM_CONTEXTMENU: show_menu(); break;
    default: break;
    }
    return true;
}

}  // namespace wb
