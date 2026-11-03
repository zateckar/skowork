#include "host/power.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>

namespace wb {

PowerInputs SystemPowerProbe::read() {
    PowerInputs in;
    SYSTEM_POWER_STATUS sps{};
    if (::GetSystemPowerStatus(&sps)) in.battery_saver = sps.SystemStatusFlag == 1;
    QUERY_USER_NOTIFICATION_STATE st{};
    if (SUCCEEDED(::SHQueryUserNotificationState(&st)))
        in.presentation = st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
    return in;
}

BackgroundGate::BackgroundGate(const Clock& clock) : clock_(clock) {}

void BackgroundGate::set_background_on_battery(bool allowed) {
    {
        const std::lock_guard lock(mx_);
        if (on_battery_allowed_ == allowed) return;
        on_battery_allowed_ = allowed;
    }
    notify();
}

void BackgroundGate::update(const PowerInputs& inputs) {
    {
        const std::lock_guard lock(mx_);
        if (inputs_ == inputs) return;
        inputs_ = inputs;
    }
    notify();
}

void BackgroundGate::pause_for(std::int64_t seconds) {
    {
        const std::lock_guard lock(mx_);
        paused_until_ = seconds > 0 ? clock_.unix_seconds() + seconds : 0;
    }
    notify();
}

bool BackgroundGate::paused() const {
    const std::lock_guard lock(mx_);
    return paused_until_ > clock_.unix_seconds();
}

std::int64_t BackgroundGate::paused_until() const {
    const std::lock_guard lock(mx_);
    return paused_until_ > clock_.unix_seconds() ? paused_until_ : 0;
}

PowerInputs BackgroundGate::inputs() const {
    const std::lock_guard lock(mx_);
    return inputs_;
}

std::string BackgroundGate::hold_reason(WorkKind kind, bool user_started) const {
    if (user_started) return {};
    const std::lock_guard lock(mx_);
    if (paused_until_ > clock_.unix_seconds()) return "paused";
    if (kind == WorkKind::trigger) return {};  // Trigger firings are paused but never deferred
    if (inputs_.battery_saver && !on_battery_allowed_) return "battery_saver";
    if (inputs_.presentation) return "presentation";
    return {};
}

bool BackgroundGate::wait_until_allowed(WorkKind kind, const std::function<bool()>& stop) {
    std::unique_lock lock(mx_);
    for (;;) {
        if (stop && stop()) return false;
        lock.unlock();
        const bool ok = may_run(kind);
        lock.lock();
        if (ok) return true;
        // Re-evaluated on every change and at least once a second (pause expiry is time-based).
        cv_.wait_for(lock, std::chrono::seconds(1));
    }
}

void BackgroundGate::on_change(std::function<void()> fn) {
    const std::lock_guard lock(mx_);
    changed_ = std::move(fn);
}

void BackgroundGate::notify() {
    std::function<void()> fn;
    {
        const std::lock_guard lock(mx_);
        fn = changed_;
    }
    cv_.notify_all();
    if (fn) fn();
}

}  // namespace wb
