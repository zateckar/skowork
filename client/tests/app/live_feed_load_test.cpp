// Load harness for live feed updates (fix-feed-live-updates 1.1, design D3, D4 and D6). A scripted stream of 2,000 text
// deltas over 20 seconds (one every 10 ms) alternates between a Run of the open Thread and a Run of a Thread that is not
// open. Four background requests are pending on the background pool for the whole stream. The deltas go through
// RunEventRelay and the window's path (apply_live_event, RepaintLimit, the feed fold), and every UI-thread task is timed
// with a steady clock; a repaint is one feed rebuild. "After" is the shipped configuration (34 ms relay and repaint
// limit, cached FeedFold). "Before" is the same stream with coalescing and the repaint limit set to 0, the window
// receiving the closed Thread's deltas (dropped there as before), and a fresh build_feed per repaint. The pre-change
// binary was not kept, so "before" models the removed path. The window's path takes no store handle, so it makes no
// store call; the harness has no store to count, and the count is zero by construction.
#include "app/run_event_relay.h"
#include "host/dispatch.h"
#include "shell/feed.h"
#include "shell/model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <vector>

using namespace wb;
using namespace wb::shell;

namespace {
using clock_type = std::chrono::steady_clock;

json delta(const std::string& run, const std::string& thread) {
    return {{"kind", "text.delta"}, {"run_id", run}, {"thread_id", thread}, {"seq", 0}, {"at", ""}, {"data", {{"text", "word "}}}};
}

struct Measure {
    std::vector<double> task_ms;  // one entry per UI-thread task
    int repaints = 0;             // feed rebuilds on the UI thread
    size_t closed_tasks = 0;      // UI-thread tasks for the Thread that is not open
    double p95() const {
        std::vector<double> s = task_ms;
        std::sort(s.begin(), s.end());
        return s.empty() ? 0.0 : s[s.size() * 95 / 100];
    }
    double max() const { return task_ms.empty() ? 0.0 : *std::max_element(task_ms.begin(), task_ms.end()); }
};

constexpr long long kStreamMs = 20000;  // 20 seconds of scripted time
constexpr long long kStepMs = 10;       // one delta every 10 ms: 2,000 deltas

Measure stream(bool after) {
    const long long interval = after ? 34 : 0;  // 0 turns coalescing and the repaint limit off
    RunEventRelay relay(interval);
    RepaintLimit limit(interval);
    FeedSnapshot snap;
    snap.entry.id = "t-open";
    RunInfo run;
    run.id = "r-open";
    run.state = "running";
    run.role = "main";
    for (long long seq = 1; seq <= 300; ++seq)  // a Thread with a history: the fold has work to save
        run.events.push_back(StoredEvent{seq, "", "message",
                                         {{"role", seq % 2 ? "user" : "assistant"}, {"content", "Message about the budget and the Q4 plan."}}});
    snap.runs.push_back(run);
    LiveText live;
    FeedFold fold;
    Measure m;

    // Four background requests pending for the whole stream: two run, two wait in the queue.
    ThreadPool background{2};
    std::promise<void> release;
    auto gate = release.get_future().share();
    for (int i = 0; i < 4; ++i) background.post([gate] { gate.wait(); });

    const auto rebuild = [&] {
        if (after) {
            (void)fold.build(snap, live);
        } else {
            (void)build_feed(snap, live);
        }
        ++m.repaints;
    };
    const auto deliver = [&](const json& e, long long now) {
        const auto t0 = clock_type::now();
        const LiveRunEvent ev = live_event_from(e);
        if (ev.thread_id != snap.entry.id) {
            ++m.closed_tasks;  // the window drops it at once
        } else {
            apply_live_event(snap, live, ev);
            if (limit.request(now)) rebuild();
        }
        m.task_ms.push_back(std::chrono::duration<double, std::milli>(clock_type::now() - t0).count());
    };
    const auto due_repaint = [&](long long now) {
        if (!limit.take_due(now)) return;
        const auto t0 = clock_type::now();
        rebuild();
        m.task_ms.push_back(std::chrono::duration<double, std::milli>(clock_type::now() - t0).count());
    };

    for (long long now = 0; now < kStreamMs; ++now) {
        if (now % kStepMs == 0) {
            const bool open = (now / kStepMs) % 2 == 0;
            // Before the change the window received every delta and dropped the closed Thread's ones itself.
            const json e = open ? delta("r-open", "t-open") : delta("r-closed", "t-closed");
            for (const json& out : relay.offer(e, now, open || !after)) deliver(out, now);
        }
        for (const json& out : relay.due(now)) deliver(out, now);
        due_repaint(now);
    }
    for (long long now = kStreamMs; now < kStreamMs + 200; ++now) {  // the last pending deltas and repaints
        for (const json& out : relay.due(now)) deliver(out, now);
        due_repaint(now);
    }
    release.set_value();
    background.shutdown();
    return m;
}
}  // namespace

// After the change: the closed Thread costs the UI thread nothing, every UI task meets D6, and the feed repaints at most
// 30 times a second (600 in the 20 seconds of the stream, plus one for the last pending repaint).
TEST(LiveFeedLoad, TwoThousandDeltasMeetTheD6LimitsAfterTheChange) {
    const Measure after = stream(true);
    std::printf("[1.1 after ] UI tasks %zu, p95 %.3f ms, max %.3f ms, repaints %d in 20 s (%.1f/s), closed-thread tasks %zu\n",
                after.task_ms.size(), after.p95(), after.max(), after.repaints, after.repaints / 20.0, after.closed_tasks);
    EXPECT_EQ(after.closed_tasks, 0u);
    EXPECT_LE(after.repaints, 601);
    EXPECT_LE(after.p95(), 16.0);
    EXPECT_LT(after.max(), 50.0);
}

// The same stream with coalescing, the repaint limit and the fold cache off: the repaint count and the UI task time are
// what the change removes. Only a sanity check of the switch is asserted; the numbers are recorded in design.md.
TEST(LiveFeedLoad, WithCoalescingOffEveryDeltaRepaints) {
    const Measure before = stream(false);
    std::printf("[1.1 before] UI tasks %zu, p95 %.3f ms, max %.3f ms, repaints %d in 20 s (%.1f/s), closed-thread tasks %zu\n",
                before.task_ms.size(), before.p95(), before.max(), before.repaints, before.repaints / 20.0, before.closed_tasks);
    EXPECT_GT(before.repaints, 601);
}
