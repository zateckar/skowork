// The host's periodic scheduler and file-watch pass (desktop-host Background Work Off The UI Thread, design D5 of
// fix-feed-live-updates). The UI thread offers one pass per tick; the pass runs on the background pool, and no pass is
// queued while the previous one is queued or running (single flight). The only shared state is the busy flag.
#pragma once

#include <atomic>
#include <functional>
#include <utility>

namespace wb {

class PeriodicPass {
public:
    // post queues a closure on the background pool; work runs one pass, with calendar_due when the calendar offsets are
    // due too.
    PeriodicPass(std::function<void(std::function<void()>)> post, std::function<void(bool calendar_due)> work)
        : post_(std::move(post)), work_(std::move(work)) {}

    // Called on the UI thread. Queues one pass unless one is queued or running; returns whether it queued one.
    bool offer(bool calendar_due) {
        if (busy_.exchange(true)) return false;
        post_([this, calendar_due] {
            work_(calendar_due);
            busy_ = false;
        });
        return true;
    }

    // True while a pass is queued or running.
    [[nodiscard]] bool busy() const { return busy_.load(); }

private:
    std::function<void(std::function<void()>)> post_;
    std::function<void(bool)> work_;
    std::atomic<bool> busy_{false};
};

}  // namespace wb
