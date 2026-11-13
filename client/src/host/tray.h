// Notification-area icon (client/desktop-host Tray Presence, App Icon). The icon is the Skowork icon from the
// executable's resources with a dot in the state's colour (improve-window-frame D4); without the resource it is a disc
// in that colour. The tooltip carries the detail.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wb {

enum class TrayState { disconnected, idle, running, action_required, paused };

const char* to_string(TrayState s);

// Picks the state shown in the tray. Order matters: paused and action-required are what the user must notice.
TrayState compute_tray_state(bool connected, bool paused, int action_items, int active_runs);

// With content telemetry in force the tooltip says so (client/telemetry Content Level); unread Chat messages are counted
// on a line of their own (client/user-messaging Chats View).
std::wstring tray_tooltip(TrayState s, int action_items, int active_runs, bool content_telemetry = false, int unread_messages = 0);

// The tray image for a state, size x size pixels of straight-alpha ARGB, top row first. `base` is the app icon at that
// size (empty: the disc in the state's colour). The state dot sits in the bottom-right corner (hollow when paused); the
// unread badge is a red dot in the top-right corner.
std::vector<std::uint32_t> tray_image(TrayState s, bool badge, const std::vector<std::uint32_t>& base, int size);

// The ARGB colour of a state's dot.
std::uint32_t tray_colour(TrayState s);

class Tray {
public:
    struct Callbacks {
        std::function<void()> open;
        std::function<void()> new_thread;
        std::function<void(int minutes)> pause;  // 0 = resume
        std::function<void()> status;
        std::function<int()> active_runs;  // used for the Quit confirmation
        std::function<void()> quit;
    };

    static constexpr UINT kCallbackMessage = WM_APP + 0x41;

    Tray(HWND owner, Callbacks callbacks);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // A dot on the icon and a tooltip line show unread Chat messages.
    void set(TrayState state, int action_items, int active_runs, bool paused, bool content_telemetry = false, int unread_messages = 0);
    [[nodiscard]] int unread_messages() const { return unread_; }
    [[nodiscard]] TrayState state() const { return state_; }
    [[nodiscard]] std::wstring tooltip() const { return tooltip_; }

    // Returns true when the message belonged to the tray (call from the owner window procedure).
    bool handle_message(UINT msg, WPARAM w, LPARAM l);

    // Asks for confirmation when Runs are active; returns true when the client should quit.
    bool confirm_quit();

private:
    void add_icon();
    void update_icon();
    void show_menu();

    HWND owner_;
    Callbacks cb_;
    TrayState state_ = TrayState::disconnected;
    std::wstring tooltip_;
    bool paused_ = false;
    int unread_ = 0;
    HICON icons_[5]{};
    HICON badged_[5]{};  // the same with the unread dot
    UINT taskbar_created_ = 0;
};

}  // namespace wb
