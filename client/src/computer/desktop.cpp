#include "computer/desktop.h"

#include "base/strings.h"
#include "platform/win.h"

#include <dwmapi.h>
#include <tlhelp32.h>

#include <thread>

namespace wb::computer {

namespace {

Box frame_of(HWND hwnd) {
    RECT r{};
    if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) ::GetWindowRect(hwnd, &r);
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

bool cloaked(HWND hwnd) {
    DWORD c = 0;
    return SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &c, sizeof c)) && c != 0;
}

DWORD integrity_of(HANDLE token) {
    DWORD size = 0;
    ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    if (size == 0 || !::GetTokenInformation(token, TokenIntegrityLevel, buf.data(), size, &size)) return 0;
    auto* til = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    const UCHAR n = *::GetSidSubAuthorityCount(til->Label.Sid);
    return n ? *::GetSidSubAuthority(til->Label.Sid, n - 1) : 0;
}

bool process_running(std::string_view exe) {
    unique_handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap || snap.get() == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = ::Process32FirstW(snap.get(), &e); ok; ok = ::Process32NextW(snap.get(), &e))
        if (iequals(narrow(e.szExeFile), exe)) return true;
    return false;
}

}  // namespace

std::string process_name(DWORD pid) {
    unique_handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD n = static_cast<DWORD>(std::size(path));
    if (!::QueryFullProcessImageNameW(h.get(), 0, path, &n)) return {};
    std::wstring_view p(path, n);
    if (const auto slash = p.find_last_of(L"\\/"); slash != std::wstring_view::npos) p = p.substr(slash + 1);
    return narrow(p);
}

Result<WindowInfo> window_info(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return make_error("window_gone", "the window no longer exists");
    WindowInfo w;
    w.hwnd = hwnd;
    wchar_t title[512];
    const int n = ::GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    w.title = narrow(std::wstring_view(title, n > 0 ? static_cast<size_t>(n) : 0));
    ::GetWindowThreadProcessId(hwnd, &w.pid);
    w.process = process_name(w.pid);
    w.box = frame_of(hwnd);
    w.minimized = ::IsIconic(hwnd) != FALSE;
    return w;
}

std::vector<WindowInfo> list_windows() {
    std::vector<HWND> handles;
    ::EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* v = reinterpret_cast<std::vector<HWND>*>(lp);
            if (!::IsWindowVisible(h) || ::GetWindow(h, GW_OWNER) || cloaked(h)) return TRUE;
            if (::GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            if (::GetWindowTextLengthW(h) == 0) return TRUE;
            v->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&handles));
    std::vector<WindowInfo> out;
    for (HWND h : handles)
        if (auto w = window_info(h)) out.push_back(std::move(*w));
    return out;
}

std::optional<std::string> unreachable_desktop() {
    // The input desktop is "Default" while the user's desktop has the input; it is "Winlogon" (and not open to this
    // process) for the secure desktop and the lock screen.
    HDESK d = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    std::wstring name;
    if (d) {
        wchar_t buf[128];
        DWORD n = 0;
        if (::GetUserObjectInformationW(d, UOI_NAME, buf, sizeof buf, &n)) name = buf;
        ::CloseDesktop(d);
    }
    if (d && _wcsicmp(name.c_str(), L"Default") == 0) {
        // The lock screen app can also run on the default desktop.
        DWORD pid = 0;
        if (HWND fg = ::GetForegroundWindow()) ::GetWindowThreadProcessId(fg, &pid);
        if (pid && iequals(process_name(pid), "LockApp.exe")) return "the lock screen";
        return std::nullopt;
    }
    if (process_running("consent.exe")) return "the secure desktop (a UAC prompt)";
    return "the lock screen";
}

bool runs_elevated(DWORD pid) {
    static const DWORD own = [] {
        HANDLE t = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &t)) return DWORD{0};
        const DWORD l = integrity_of(t);
        ::CloseHandle(t);
        return l;
    }();
    unique_handle p(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!p) return true;
    HANDLE t = nullptr;
    if (!::OpenProcessToken(p.get(), TOKEN_QUERY, &t)) return true;
    const DWORD level = integrity_of(t);
    ::CloseHandle(t);
    return level > own;
}

Status bring_to_front(HWND hwnd) {
    if (!::IsWindow(hwnd)) return make_error("window_gone", "the window no longer exists");
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    // A dialog the window owns counts as the window being in front.
    const auto in_front = [hwnd] {
        HWND fg = ::GetForegroundWindow();
        return fg && (fg == hwnd || ::GetAncestor(fg, GA_ROOTOWNER) == ::GetAncestor(hwnd, GA_ROOTOWNER));
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (in_front()) return {};
        if (!::SetForegroundWindow(hwnd)) {
            // Windows lets a process set the foreground window right after it sent input itself: a lone Alt press
            // and release (no menu opens, because nothing else is pressed in between).
            INPUT in[2]{};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            ::SendInput(2, in, sizeof(INPUT));
            ::SetForegroundWindow(hwnd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
    }
    if (in_front()) return {};
    return make_error("foreground", "the window could not be brought to the front");
}

}  // namespace wb::computer
