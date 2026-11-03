// The on-demand UI window as the host sees it (client/desktop-host On-Demand UI Window): opened at a route, closed,
// and fed with host events (status, navigate, run.event, inbox.changed). The native window (add-native-ui,
// app/native_window.h) implements it; the host does not depend on the shell.
#pragma once

#include <string>

#include "base/json.h"

namespace wb {

class AppWindow {
public:
    virtual ~AppWindow() = default;
    // Opens the window or brings it to the foreground, at a route such as "#/about" (empty: where it was).
    virtual void open(const std::string& route = {}) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;
    // Sends a host event to the window if it is open (main thread only).
    virtual void emit(const std::string& event, const json& data) = 0;
};

}  // namespace wb
