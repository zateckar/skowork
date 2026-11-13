// UI Automation for the native window (add-native-ui design D8): one fragment provider per exposed node of the
// element tree (the accessibility tree of layout.h), returned from WM_GETOBJECT. Roles map to control types; the
// node's capabilities map to the Invoke, Toggle, ExpandCollapse, SelectionItem, Selection, Scroll and Value
// patterns. New Action items are announced with a notification event.
//
// Providers refer to nodes by runtime id and look them up on each call, so a provider a client still holds after
// its node is gone answers "element not available". UI Automation calls the providers on its own threads; each call
// that reads or changes the element tree is sent to the window (kUiaCallMessage) and runs on the window's thread.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "ui/layout.h"

namespace wb::ui {

// A provider call sent to the window (wParam kUiaCallMagic, lParam a const std::function<void()>*); the window passes
// it to Accessibility::run_call.
inline constexpr UINT kUiaCallMessage = WM_APP + 0x42;
inline constexpr WPARAM kUiaCallMagic = 0x55494143;  // "UIAC"

// Implemented by the window.
class UiaSite {
public:
    virtual ~UiaSite() = default;
    virtual HWND hwnd() const = 0;
    virtual Tree& tree() = 0;
    virtual RECT screen_rect(const Rect& dip) const = 0;  // physical screen pixels
    virtual Node* node_at_screen(POINT screen) = 0;
    virtual Node* focus_target() = 0;  // the node UI Automation reports as focused (a list's selected row)
    virtual void focus(Node& n) = 0;
};

class Accessibility {
public:
    explicit Accessibility(UiaSite* site);
    ~Accessibility();
    Accessibility(const Accessibility&) = delete;
    Accessibility& operator=(const Accessibility&) = delete;

    LRESULT on_getobject(WPARAM wp, LPARAM lp);  // WM_GETOBJECT
    static bool run_call(WPARAM wp, LPARAM lp);   // kUiaCallMessage: runs a provider call; false when it is not one
    void detach();                                // before the window goes away
    void focus_changed(Node* n);
    void notify(const std::u16string& text, const std::u16string& activity_id = u"skowork");
    void property_changed_name(Node& n);

    struct State;

private:
    std::shared_ptr<State> s_;
};

// The UI Automation control type id for a role (UIA_*ControlTypeId).
long control_type_of(Role r);

}  // namespace wb::ui
