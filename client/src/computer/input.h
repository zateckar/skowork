// Synthesized mouse and keyboard input, the fallback after UI Automation patterns (client/computer-use Element-First
// Actions; add-computer-use design D3). Typing uses Unicode key events, so it does not depend on the keyboard layout.
// Every pressed key and button is tracked, so a stop releases them all; each event carries kTag in its extra info.
#pragma once

#include "base/result.h"
#include "computer/classify.h"

#include <windows.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wb::computer {

enum class Button { left, right, middle };

class InputSender {
public:
    static constexpr ULONG_PTR kTag = 0x57425449;  // "WBTI"

    // `stopped` is checked between events: once it returns true no further input is sent.
    explicit InputSender(std::function<bool()> stopped = {}) : stopped_(std::move(stopped)) {}

    Status move(int x, int y);
    Status click(int x, int y, Button button = Button::left, int count = 1);
    Status type_text(const std::string& utf8);
    Status keys(const std::vector<KeyCombo>& combos);
    // Wheel notches at a point: positive vertical scrolls up (away from the user), positive horizontal to the right.
    Status wheel(int x, int y, int vertical, int horizontal);
    Status drag(int x1, int y1, int x2, int y2, std::chrono::milliseconds duration = std::chrono::milliseconds(400));

    // Releases every key and button this sender holds down. Safe to call from another thread at any time.
    void release_all();
    // Whether a key or button (virtual-key code; VK_LBUTTON ...) is held by this sender.
    [[nodiscard]] bool holding(unsigned vk);

private:
    Status send(std::vector<INPUT> events);
    Status key(unsigned vk, bool down);
    Status button(Button b, bool down);
    bool stop_now() const { return stopped_ && stopped_(); }

    std::function<bool()> stopped_;
    std::mutex mx_;           // serialises sending and releasing
    std::set<unsigned> held_;  // virtual-key codes (buttons as VK_LBUTTON, VK_RBUTTON, VK_MBUTTON)
};

}  // namespace wb::computer
