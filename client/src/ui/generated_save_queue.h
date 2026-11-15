#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "base/json.h"

namespace wb::ui {

// All methods and callbacks run on the UI thread. A queue is shared across views of one host-bound surface.
class GeneratedSaveQueue : public std::enable_shared_from_this<GeneratedSaveQueue> {
public:
    using Done = std::function<void(bool)>;
    using Send = std::function<void(const json&, Done)>;
    explicit GeneratedSaveQueue(Send send, std::function<void()> on_failure = {})
        : send_(std::move(send)), on_failure_(std::move(on_failure)) {}

    void save(json state) {
        latest_ = std::move(state);
        dirty_ = true;
        if (!pending_) dispatch();
    }
    void retry() { if (dirty_ && !pending_) dispatch(); }
    // The target was resolved, so late replies and coalesced drafts are no longer meaningful.
    void abandon() { ++sequence_; pending_ = false; dirty_ = false; latest_.reset(); }
    [[nodiscard]] const std::optional<json>& latest() const { return latest_; }
    [[nodiscard]] bool pending() const { return pending_; }
    [[nodiscard]] bool needs_retry() const { return dirty_ && !pending_; }

private:
    void dispatch() {
        if (!latest_ || !send_) return;
        const json sent = *latest_;
        dirty_ = false;
        pending_ = true;
        const auto sequence = ++sequence_;
        auto self = shared_from_this();
        send_(sent, [self, sequence, sent](bool ok) {
            if (!self->pending_ || self->sequence_ != sequence) return;
            self->pending_ = false;
            if (!ok) {
                self->dirty_ = true;
                if (self->on_failure_) self->on_failure_();
            }
            // Do not loop on a persistent failure; a newer edit is still worth attempting immediately.
            if (self->dirty_ && (ok || (self->latest_ && *self->latest_ != sent))) self->dispatch();
        });
    }
    Send send_;
    std::function<void()> on_failure_;
    std::optional<json> latest_;
    unsigned long long sequence_ = 0;
    bool dirty_ = false, pending_ = false;
};

}  // namespace wb::ui
