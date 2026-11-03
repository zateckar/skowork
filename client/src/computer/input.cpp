#include "computer/input.h"

#include "platform/win.h"

#include <cmath>
#include <thread>

namespace wb::computer {

namespace {

bool extended(unsigned vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_SNAPSHOT:
        return true;
    default:
        return false;
    }
}

INPUT key_event(unsigned vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = InputSender::kTag;
    return in;
}

INPUT unicode_event(wchar_t unit, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = unit;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    in.ki.dwExtraInfo = InputSender::kTag;
    return in;
}

// Absolute coordinates over the virtual desktop: Windows maps n to floor(n * width / 65536), so rounding up hits the
// pixel exactly.
INPUT move_event(int x, int y) {
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN), vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, ::GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(1, ::GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(std::ceil((x - vx) * 65536.0 / vw));
    in.mi.dy = static_cast<LONG>(std::ceil((y - vy) * 65536.0 / vh));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo = InputSender::kTag;
    return in;
}

unsigned vk_of(Button b) { return b == Button::left ? VK_LBUTTON : b == Button::right ? VK_RBUTTON : VK_MBUTTON; }

std::vector<unsigned> modifier_keys(unsigned mods) {
    std::vector<unsigned> v;
    if (mods & MOD_CONTROL) v.push_back(VK_CONTROL);
    if (mods & MOD_ALT) v.push_back(VK_MENU);
    if (mods & MOD_SHIFT) v.push_back(VK_SHIFT);
    if (mods & MOD_WIN) v.push_back(VK_LWIN);
    return v;
}

}  // namespace

Status InputSender::send(std::vector<INPUT> events) {
    if (events.empty()) return {};
    if (stop_now()) return make_error("stopped", "computer use was stopped");
    const UINT n = ::SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    if (n != events.size())
        return make_error("blocked", "Windows blocked the input (the window may run as administrator, or another desktop has the input)");
    return {};
}

Status InputSender::key(unsigned vk, bool down) {
    const std::lock_guard lock(mx_);
    if (Status s = send({key_event(vk, down)}); !s) return s;
    if (down) held_.insert(vk);
    else held_.erase(vk);
    return {};
}

Status InputSender::button(Button b, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = b == Button::left ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                    : b == Button::right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                                         : (down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
    in.mi.dwExtraInfo = kTag;
    const std::lock_guard lock(mx_);
    if (Status s = send({in}); !s) return s;
    if (down) held_.insert(vk_of(b));
    else held_.erase(vk_of(b));
    return {};
}

Status InputSender::move(int x, int y) {
    const std::lock_guard lock(mx_);
    return send({move_event(x, y)});
}

Status InputSender::click(int x, int y, Button b, int count) {
    if (Status s = move(x, y); !s) return s;
    for (int i = 0; i < std::max(1, count); ++i) {
        if (Status s = button(b, true); !s) return s;
        if (Status s = button(b, false); !s) return s;
    }
    return {};
}

Status InputSender::type_text(const std::string& utf8) {
    const std::wstring text = widen(utf8);
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') continue;
        if (c == L'\n' || c == L'\t') {
            const unsigned vk = c == L'\n' ? VK_RETURN : VK_TAB;
            if (Status s = key(vk, true); !s) return s;
            if (Status s = key(vk, false); !s) return s;
            continue;
        }
        const std::lock_guard lock(mx_);
        if (Status s = send({unicode_event(c, true), unicode_event(c, false)}); !s) return s;
        if (i % 32 == 31) std::this_thread::sleep_for(std::chrono::milliseconds(5));  // lets slow applications keep up
    }
    return {};
}

Status InputSender::keys(const std::vector<KeyCombo>& combos) {
    for (const KeyCombo& c : combos) {
        Status st;
        std::vector<unsigned> pressed;
        for (const unsigned m : modifier_keys(c.modifiers)) {
            if (st = key(m, true); !st) break;
            pressed.push_back(m);
        }
        if (st && (st = key(c.vk, true))) st = key(c.vk, false);
        for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) (void)key(*it, false);
        if (!st) {
            release_all();
            return st;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {};
}

Status InputSender::wheel(int x, int y, int vertical, int horizontal) {
    if (Status s = move(x, y); !s) return s;
    const auto notch = [&](DWORD flag, int n) -> Status {
        for (int i = 0; i < std::abs(n); ++i) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = flag;
            in.mi.mouseData = static_cast<DWORD>(n > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
            in.mi.dwExtraInfo = kTag;
            const std::lock_guard lock(mx_);
            if (Status s = send({in}); !s) return s;
        }
        return {};
    };
    if (Status s = notch(MOUSEEVENTF_WHEEL, vertical); !s) return s;
    return notch(MOUSEEVENTF_HWHEEL, horizontal);
}

Status InputSender::drag(int x1, int y1, int x2, int y2, std::chrono::milliseconds duration) {
    if (Status s = move(x1, y1); !s) return s;
    if (Status s = button(Button::left, true); !s) return s;
    const int steps = std::max(2, static_cast<int>(duration.count() / 16));
    for (int i = 1; i <= steps; ++i) {
        const int x = x1 + (x2 - x1) * i / steps, y = y1 + (y2 - y1) * i / steps;
        if (Status s = move(x, y); !s) {
            release_all();
            return s;
        }
        std::this_thread::sleep_for(duration / steps);
    }
    if (Status s = button(Button::left, false); !s) {
        release_all();
        return s;
    }
    return {};
}

void InputSender::release_all() {
    const std::lock_guard lock(mx_);
    std::vector<INPUT> ups;
    for (const unsigned vk : held_) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = vk == VK_LBUTTON ? MOUSEEVENTF_LEFTUP : vk == VK_RBUTTON ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_MIDDLEUP;
            in.mi.dwExtraInfo = kTag;
            ups.push_back(in);
        } else {
            ups.push_back(key_event(vk, false));
        }
    }
    held_.clear();
    // Sent even when stopped: releasing is what the stop asks for.
    if (!ups.empty()) ::SendInput(static_cast<UINT>(ups.size()), ups.data(), sizeof(INPUT));
}

bool InputSender::holding(unsigned vk) {
    const std::lock_guard lock(mx_);
    return held_.count(vk) > 0;
}

}  // namespace wb::computer
