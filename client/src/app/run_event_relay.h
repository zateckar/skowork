// The host's relay of live Run events to the window (client/work-inbox Live Feed Updates, desktop-host UI Thread Work
// Limit, design D3). The text and reasoning deltas of one Run merge into one event per interval, and the deltas of a
// Thread that is not open are dropped. Every other event goes out at once, after the Run's pending deltas, so the order
// of the Run's events is kept. The class does no locking: the caller holds one lock around each call.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "base/json.h"

namespace wb {

class RunEventRelay {
public:
    // At most one delta event per Run and interval (34 ms: at most 30 a second, design D3).
    explicit RunEventRelay(long long interval_ms = 34) : interval_(interval_ms) {}

    // Takes one event from the runtime. Returns the events to send now, in order. `thread_open` says whether the window
    // has the event's Thread open; a delta of another Thread is dropped.
    std::vector<json> offer(const json& e, long long now_ms, bool thread_open) {
        const std::string kind = jstr(e, "kind");
        const bool delta = kind == "text.delta" || kind == "reasoning.delta";
        const std::string run = jstr(e, "run_id");
        if (!delta) {
            std::vector<json> out = flush_run(run, now_ms);
            out.push_back(e);
            return out;
        }
        if (!thread_open) return {};
        std::vector<json> out;
        auto it = runs_.find(run);
        if (it != runs_.end() && it->second.queued && jstr(it->second.event, "kind") != kind) out = flush_run(run, now_ms);  // keep the order of kinds
        it = runs_.find(run);
        if (it == runs_.end()) it = runs_.emplace(run, Pending{}).first;
        Pending& p = it->second;
        if (!p.queued) {
            p.event = e;
            p.queued = true;
        } else {
            p.event["data"]["text"] = jstr(p.event["data"], "text") + jstr(e["data"], "text");
            p.event["at"] = jstr(e, "at");
        }
        if (now_ms - p.sent_at >= interval_) {
            auto sent = flush_run(run, now_ms);
            out.insert(out.end(), sent.begin(), sent.end());
        }
        return out;
    }

    // The pending deltas whose interval has passed, in order of their Runs.
    std::vector<json> due(long long now_ms) {
        std::vector<json> out;
        for (auto& [run, p] : runs_)
            if (p.queued && now_ms - p.sent_at >= interval_) {
                out.push_back(p.event);
                p.queued = false;
                p.sent_at = now_ms;
            }
        return out;
    }

    [[nodiscard]] bool pending() const {
        for (const auto& [run, p] : runs_)
            if (p.queued) return true;
        return false;
    }

    // When the next pending delta is due; only meaningful while pending() holds.
    [[nodiscard]] long long next_due() const {
        long long at = -1;
        for (const auto& [run, p] : runs_)
            if (p.queued && (at < 0 || p.sent_at + interval_ < at)) at = p.sent_at + interval_;
        return at;
    }

private:
    struct Pending {
        json event;
        bool queued = false;
        long long sent_at = -1000000000LL;  // when the Run's last event went out
    };

    std::vector<json> flush_run(const std::string& run, long long now_ms) {
        std::vector<json> out;
        const auto it = runs_.find(run);
        if (it == runs_.end() || !it->second.queued) return out;
        out.push_back(it->second.event);
        it->second.queued = false;
        it->second.sent_at = now_ms;
        return out;
    }

    long long interval_;
    std::map<std::string, Pending> runs_;
};

}  // namespace wb
