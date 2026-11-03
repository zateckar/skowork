// Pause Background Work and Background Work Deferral (client/desktop-host). One gate that every background
// producer asks before starting or continuing a unit of work.
#pragma once

#include "base/time.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace wb {

enum class WorkKind { indexing, transcription, sync, trigger };

struct PowerInputs {
    bool battery_saver = false;
    bool presentation = false;  // full-screen app, D3D full screen or presentation mode
    bool operator==(const PowerInputs&) const = default;
};

class PowerProbe {
public:
    virtual ~PowerProbe() = default;
    virtual PowerInputs read() = 0;
};

// Reads GetSystemPowerStatus (battery saver) and SHQueryUserNotificationState (presentation).
class SystemPowerProbe final : public PowerProbe {
public:
    PowerInputs read() override;
};

class BackgroundGate {
public:
    explicit BackgroundGate(const Clock& clock);

    void set_background_on_battery(bool allowed);  // policy.ui.background_on_battery
    void update(const PowerInputs& inputs);
    void pause_for(std::int64_t seconds);  // 0 resumes
    void resume() { pause_for(0); }

    [[nodiscard]] bool paused() const;
    [[nodiscard]] std::int64_t paused_until() const;  // unix seconds, 0 when not paused
    [[nodiscard]] PowerInputs inputs() const;

    // Why work of this kind must not run now, or an empty string. User-started work is never held back.
    [[nodiscard]] std::string hold_reason(WorkKind kind, bool user_started = false) const;
    [[nodiscard]] bool may_run(WorkKind kind, bool user_started = false) const {
        return hold_reason(kind, user_started).empty();
    }

    // Blocks a worker thread until the work may run or `stop` returns true. Returns false when stopped.
    bool wait_until_allowed(WorkKind kind, const std::function<bool()>& stop);

    void on_change(std::function<void()> fn);

private:
    void notify();

    const Clock& clock_;
    mutable std::mutex mx_;
    std::condition_variable cv_;
    PowerInputs inputs_;
    bool on_battery_allowed_ = false;
    std::int64_t paused_until_ = 0;
    std::function<void()> changed_;
};

}  // namespace wb
