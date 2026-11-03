#include "ui/textfield.h"

#include <initguid.h>
#include <oleacc.h>
#include <richedit.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "platform/com.h"

#ifndef IMF_SPELLCHECKING
#define IMF_SPELLCHECKING 0x00000800
#endif

namespace wb::ui {

namespace {
constexpr wchar_t kProp[] = L"WorkbenchTextField";
constexpr float kPadX = 10.f, kPadY = 6.f;

// UI Automation's Name property, for dynamic annotation of the child window ({C3A6921B-4A99-44F1-BCA6-61187052C431}).
const GUID kUiaNameProperty = {0xc3a6921b, 0x4a99, 0x44f1, {0xbc, 0xa6, 0x61, 0x18, 0x70, 0x52, 0xc4, 0x31}};

COLORREF colorref(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

bool load_msftedit() {
    static const bool loaded = ::LoadLibraryW(L"Msftedit.dll") != nullptr;
    return loaded;
}

// Names the child window for screen readers (MSAA and UI Automation) through dynamic annotation.
void annotate_name(HWND h, const std::u16string& name) {
    com_ptr<IAccPropServices> svc;
    if (FAILED(::CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_IAccPropServices, svc.put_void()))) return;
    const std::wstring w(name.begin(), name.end());
    svc->SetHwndPropStr(h, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, w.c_str());
    svc->SetHwndPropStr(h, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, kUiaNameProperty, w.c_str());
}

bool modifier(int vk) { return ::GetKeyState(vk) < 0; }

std::vector<std::wstring> drop_paths(HDROP drop) {
    std::vector<std::wstring> out;
    const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        ::DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        out.push_back(std::move(p));
    }
    return out;
}
}  // namespace

TextField::TextField(Ui& ui, std::string id, std::u16string nm, Options o) : Node(std::move(id)), ui_(ui), opt_(std::move(o)) {
    role = Role::edit;
    focusable = true;
    name = std::move(nm);
    lines_ = std::max(1, opt_.min_lines);
    measure = [this](float w) { return Size{w, static_cast<float>(lines_) * line_height() + 2 * kPadY + 2}; };
    ui_.fields.push_back(this);
    if (ui_.host) create_window();
}

TextField::~TextField() {
    ui_.fields.erase(std::remove(ui_.fields.begin(), ui_.fields.end(), this), ui_.fields.end());
    if (edit_) {
        HWND h = edit_;
        ::RemovePropW(h, kProp);
        ::SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(base_proc_));
        edit_ = nullptr;
        ::DestroyWindow(h);
    }
}

float TextField::line_height() const { return std::ceil(ui_.measure_text(u"Ag", TextStyle::body, 1000.f, false).h); }

void TextField::create_window() {
    if (!load_msftedit()) return;
    DWORD style = WS_CHILD | WS_TABSTOP;
    style |= opt_.multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_NOHIDESEL) : ES_AUTOHSCROLL;
    // The field shares the host window's per-monitor DPI awareness, so its text scales with the window.
    const DPI_AWARENESS_CONTEXT prev = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    edit_ = ::CreateWindowExW(0, MSFTEDIT_CLASS, L"", style, 0, 0, 10, 10, ui_.host, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (prev) ::SetThreadDpiAwarenessContext(prev);
    if (!edit_) return;
    ::SetPropW(edit_, kProp, this);
    base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TextField::edit_proc)));
    ::SendMessageW(edit_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    ::SendMessageW(edit_, EM_SETEDITSTYLE, SES_USECTF, SES_USECTF);
    const LRESULT lang = ::SendMessageW(edit_, EM_GETLANGOPTIONS, 0, 0);
    ::SendMessageW(edit_, EM_SETLANGOPTIONS, 0, lang | IMF_SPELLCHECKING);
    ::SendMessageW(edit_, EM_EXLIMITTEXT, 0, opt_.max_length ? static_cast<LPARAM>(opt_.max_length) : 1000000);
    ::SendMessageW(edit_, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_DROPFILES);
    ::DragAcceptFiles(edit_, TRUE);
    annotate_name(edit_, name);
    apply_theme();
}

void TextField::apply_theme() {
    if (!edit_) return;
    const uint32_t text = ui_.color(Paint::text), bg = ui_.color(Paint::surface);
    if (text == applied_text_ && bg == applied_bg_) return;
    applied_text_ = text;
    applied_bg_ = bg;
    ::SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(colorref(bg)));
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 210;  // 14 DIPs = 10.5 pt, in twips
    cf.crTextColor = colorref(text);
    const std::wstring& face = ui_.text_family();
    std::wcsncpy(cf.szFaceName, face.c_str(), LF_FACESIZE - 1);
    ::SendMessageW(edit_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    ::SendMessageW(edit_, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
}

std::u16string TextField::text() const {
    if (!edit_) return offline_text_;
    // Measured as it is read, with CRLF line breaks (the control stores CR alone).
    GETTEXTLENGTHEX gl{GTL_USECRLF | GTL_PRECISE | GTL_NUMCHARS, 1200};
    const auto n = static_cast<size_t>(::SendMessageW(edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&gl), 0));
    std::u16string s(n + 1, u'\0');
    GETTEXTEX gt{static_cast<DWORD>((n + 1) * sizeof(char16_t)), GT_USECRLF, 1200, nullptr, nullptr};
    const auto got = static_cast<size_t>(::SendMessageW(edit_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&gt), reinterpret_cast<LPARAM>(s.data())));
    s.resize(std::min(got, n));
    // CRLF from the control becomes LF, as the rest of the client stores text.
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == u'\r') {
            out.push_back(u'\n');
            if (i + 1 < s.size() && s[i + 1] == u'\n') ++i;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

void TextField::set_text(const std::u16string& t) {
    if (!edit_) {
        offline_text_ = t;
        invalidate();
        return;
    }
    SETTEXTEX st{ST_DEFAULT | ST_UNICODE, 1200};
    std::u16string z = t;
    ::SendMessageW(edit_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&st), reinterpret_cast<LPARAM>(z.c_str()));
    ::SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    update_height();
    invalidate();
}

void TextField::set_placeholder(std::u16string p) {
    opt_.placeholder = std::move(p);
    invalidate();
}

void TextField::select_all() {
    if (edit_) ::SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void TextField::insert(const std::u16string& t) {
    if (!edit_) {
        offline_text_ += t;
        if (on_change) on_change();
        return;
    }
    std::u16string z = t;
    ::SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(z.c_str()));
}

char16_t TextField::char_before_caret() const {
    if (!edit_) return offline_text_.empty() ? char16_t{0} : offline_text_.back();
    DWORD start = 0, end = 0;
    ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    const std::u16string t = text();
    return start == 0 || start > t.size() ? char16_t{0} : t[start - 1];
}

bool TextField::has_window_focus() const { return edit_ && ::GetFocus() == edit_; }

void TextField::update_height() {
    if (!opt_.multiline) return;
    int n = 1;
    if (edit_) n = static_cast<int>(::SendMessageW(edit_, EM_GETLINECOUNT, 0, 0));
    else n = 1 + static_cast<int>(std::count(offline_text_.begin(), offline_text_.end(), u'\n'));
    n = std::clamp(n, std::max(1, opt_.min_lines), std::max(1, opt_.max_lines));
    if (n != lines_) {
        lines_ = n;
        relayout();
    }
}

void TextField::on_command(UINT code) {
    if (code != EN_CHANGE) return;
    update_height();
    invalidate();
    if (on_change) on_change();
}

void TextField::on_focus(bool on) {
    invalidate();
    if (!on || !edit_ || focusing_ || hiding_ || ::GetFocus() == edit_) return;
    if (Tree* t = tree()) place(t->visible_rect(*this), true);
    ::SetFocus(edit_);
}

void TextField::place(const Rect& vis, bool allowed) {
    if (!edit_) return;
    apply_theme();
    const bool focused = tree() && tree()->focused() == this;
    const bool show = allowed && visible_rect_ok(vis) && (focused || has_window_focus() || !empty());
    const Rect& f = frame();
    const Rect inner = Rect{f.x + 1 + kPadX, f.y + 1 + kPadY, f.w - 2 - 2 * kPadX, f.h - 2 - 2 * kPadY}.intersect(vis);
    const float k = ui_.scale;
    const int x = static_cast<int>(std::lround(inner.x * k)), y = static_cast<int>(std::lround(inner.y * k));
    const int w = static_cast<int>(std::lround(inner.right() * k)) - x, h = static_cast<int>(std::lround(inner.bottom() * k)) - y;
    RECT cur{};
    ::GetWindowRect(edit_, &cur);
    ::MapWindowPoints(nullptr, ui_.host, reinterpret_cast<POINT*>(&cur), 2);
    const bool moved = cur.left != x || cur.top != y || cur.right - cur.left != w || cur.bottom - cur.top != h;
    if (!show) {
        if (shown_) {
            if (::GetFocus() == edit_) {
                hiding_ = true;  // the window's WM_SETFOCUS must not hand the keyboard straight back
                ::SetFocus(ui_.host);
                hiding_ = false;
            }
            ::ShowWindow(edit_, SW_HIDE);
            shown_ = false;
            invalidate();
        }
        return;
    }
    if (moved || !shown_) {
        ::SetWindowPos(edit_, nullptr, x, y, std::max(1, w), std::max(1, h), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!shown_) invalidate();
        shown_ = true;
    }
}

bool TextField::visible_rect_ok(const Rect& vis) const { return !vis.empty() && vis.h >= frame().h * 0.5f; }

void TextField::draw(Canvas& c) {
    const Rect& f = frame();
    const bool focused = tree() && tree()->focused() == this;
    c.round(f, 4.f, Paint::surface, focused ? Paint::accent : Paint::control_border);
    if (focused && !ui_.theme.high_contrast) c.fill({f.x + 2, f.bottom() - 2, f.w - 4, 2}, Paint::accent);
    if (edit_ && shown_) return;  // the child window draws the text
    const std::u16string t = text();
    const bool placeholder = t.empty();
    const std::u16string& shown = placeholder ? opt_.placeholder : t;
    if (shown.empty()) return;
    c.push_clip(f.inset(1));
    c.text(ui_.layout(shown, TextStyle::body, std::max(1.f, f.w - 2 * kPadX - 2), opt_.multiline), f.x + 1 + kPadX, f.y + 1 + kPadY,
           placeholder ? Paint::text_secondary : Paint::text);
    c.pop_clip();
}

LRESULT CALLBACK TextField::edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = static_cast<TextField*>(::GetPropW(h, kProp));
    if (!self) return ::DefWindowProcW(h, msg, wp, lp);
    return self->edit_message(msg, wp, lp);
}

LRESULT TextField::edit_message(UINT msg, WPARAM wp, LPARAM lp) {
    HWND h = edit_;
    WNDPROC base = base_proc_;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        swallow_char_ = false;
        KeyEvent e;
        e.key = static_cast<unsigned>(wp);
        e.shift = modifier(VK_SHIFT);
        e.ctrl = modifier(VK_CONTROL);
        e.alt = msg == WM_SYSKEYDOWN || modifier(VK_MENU);
        if (on_key_down) {
            // The handler may close what holds this field (Enter saves a dialog), destroying the field and its window
            // while they handle this key: it runs from a copy, and nothing of the field is touched once it is gone.
            const std::function<bool(const KeyEvent&)> handler = on_key_down;
            const std::weak_ptr<int> alive = alive_;
            if (handler(e)) {
                if (alive.lock()) swallow_char_ = true;
                return 0;
            }
            if (!alive.lock()) return 0;
        }
        // Files on the clipboard become attachments where the field takes files.
        if (e.ctrl && !e.alt && e.key == 'V' && on_files && ::IsClipboardFormatAvailable(CF_HDROP) && ::OpenClipboard(ui_.host)) {
            std::vector<std::wstring> paths;
            if (HANDLE d = ::GetClipboardData(CF_HDROP)) paths = drop_paths(static_cast<HDROP>(d));
            ::CloseClipboard();
            if (!paths.empty()) {
                on_files(paths);
                return 0;
            }
        }
        const bool window_key = (e.key == kKeyTab && !e.ctrl) || e.key == kKeyEscape || (e.key == kKeyEnter && !opt_.multiline) ||
                                (e.ctrl && !e.alt &&
                                 (e.key == kKeyN || e.key == kKeyF || e.key == kKeyB || e.key == kKeyComma || e.key == kKeySlash));
        if (window_key) {
            swallow_char_ = true;
            if (ui_.dispatch_key) ui_.dispatch_key(e);
            return 0;
        }
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        if (swallow_char_) {
            swallow_char_ = false;
            if (wp < 0x20 || msg == WM_SYSCHAR) return 0;
        }
        if (wp == L'\t') return 0;
        break;
    case WM_SETFOCUS: {
        const LRESULT r = ::CallWindowProcW(base, h, msg, wp, lp);
        if (Tree* t = tree(); t && t->focused() != this) {
            focusing_ = true;
            t->set_focus(this);
            focusing_ = false;
        }
        invalidate();
        return r;
    }
    case WM_KILLFOCUS: {
        const LRESULT r = ::CallWindowProcW(base, h, msg, wp, lp);
        invalidate();
        return r;
    }
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wp);
        const auto paths = drop_paths(drop);
        ::DragFinish(drop);
        if (on_files && !paths.empty()) on_files(paths);
        return 0;
    }
    case WM_NCDESTROY:
        ::RemovePropW(h, kProp);
        ::SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(base));
        edit_ = nullptr;
        shown_ = false;
        return ::CallWindowProcW(base, h, msg, wp, lp);
    default: break;
    }
    return ::CallWindowProcW(base, h, msg, wp, lp);
}

void TextField::on_notify(const NMHDR* hdr) {
    if (hdr->code != EN_DROPFILES) return;
    const auto* d = reinterpret_cast<const ENDROPFILES*>(hdr);
    const auto paths = drop_paths(static_cast<HDROP>(d->hDrop));
    if (on_files && !paths.empty()) on_files(paths);
}

}  // namespace wb::ui
