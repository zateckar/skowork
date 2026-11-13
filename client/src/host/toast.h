// Windows toast notifications (client/work-inbox Desktop Notifications; add-work-inbox design D6): shown through
// ToastNotificationManager with the AUMID registered by ensure_registration. The host process owns every toast it
// shows and handles a click through the toast's Activated event. Call from the main (UI) thread only.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wb {

struct ToastContent {
    std::string title, body;  // UTF-8
    std::string thread_id;    // handed to the activation callback
};

// The toast XML for `content` (exposed for tests).
std::wstring toast_xml(const ToastContent& content);

class ToastNotifier {
public:
    // `on_activated` is called on the main thread with the thread id of a clicked toast.
    explicit ToastNotifier(std::function<void(const std::string& thread_id)> on_activated);
    ~ToastNotifier();
    ToastNotifier(const ToastNotifier&) = delete;
    ToastNotifier& operator=(const ToastNotifier&) = delete;

    // Returns false when Windows refused the toast (no AUMID registration, notifications off); the reason is logged.
    bool show(const ToastContent& content);
    // Removes every Skowork toast from the Action Center (at quit).
    void clear();

    // Development endpoints (e2e tests): the Thread ids of the toasts shown, how many Skowork toasts Windows keeps
    // in the Action Center (-1 when unavailable), and the click handling of a toast for `thread_id`.
    [[nodiscard]] std::vector<std::string> shown_threads() const;
    [[nodiscard]] int history_count();
    // Windows' notification setting for Skowork: "enabled", "disabled_for_application", "disabled_for_user",
    // "disabled_by_group_policy", "disabled_by_manifest" or "unavailable".
    [[nodiscard]] std::string setting();
    void activate(const std::string& thread_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wb
