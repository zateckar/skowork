// The host's periodic scheduler pass (fix-feed-live-updates 4.2, design D5 and D6): the pass runs off the UI thread, one
// pass at a time, and the UI thread's part of an offer stays under the D6 budget while a pass is slow.
#include "app/periodic_pass.h"
#include "host/dispatch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace wb;
using namespace std::chrono_literals;

namespace {
using clock_type = std::chrono::steady_clock;
double ms(clock_type::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

std::function<void(std::function<void()>)> on(ThreadPool& pool) {
    return [&pool](std::function<void()> fn) { pool.post(std::move(fn)); };
}

// Waits until the pass is no longer queued or running; false when that takes longer than 5 seconds.
bool settle(const PeriodicPass& pass) {
    for (int waited = 0; waited < 5000; waited += 1) {
        if (!pass.busy()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
}  // namespace

// The pass runs on a pool thread, not on the thread that offers it.
TEST(PeriodicPass, ThePassRunsOffTheUiThreadAndTheOfferReturnsAtOnce) {
    ThreadPool pool{2};
    std::promise<std::thread::id> ran;
    PeriodicPass pass(on(pool), [&ran](bool) { ran.set_value(std::this_thread::get_id()); });
    const auto ui = std::this_thread::get_id();
    ASSERT_TRUE(pass.offer(false));
    auto f = ran.get_future();
    ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
    EXPECT_NE(f.get(), ui);
    EXPECT_TRUE(settle(pass));
    pool.shutdown();
}

// While a pass is queued or running, further offers queue nothing; the next pass starts after the running one ended.
TEST(PeriodicPass, OnePassAtATimeAndAnOfferDuringAPassQueuesNothing) {
    ThreadPool pool{2};
    std::promise<void> started;
    auto started_f = started.get_future();
    std::promise<void> release;
    auto release_f = release.get_future().share();
    std::atomic<int> runs{0}, active{0}, max_active{0};
    std::atomic<bool> first{true};
    PeriodicPass pass(on(pool), [&](bool) {
        const int a = ++active;
        int seen = max_active.load();
        while (a > seen && !max_active.compare_exchange_weak(seen, a)) {
        }
        if (first.exchange(false)) {
            started.set_value();
            release_f.wait();
        }
        ++runs;
        --active;
    });
    ASSERT_TRUE(pass.offer(false));
    ASSERT_EQ(started_f.wait_for(5s), std::future_status::ready);
    int queued = 0;
    for (int i = 0; i < 50; ++i) queued += pass.offer(i % 15 == 0) ? 1 : 0;
    EXPECT_EQ(queued, 0);
    EXPECT_TRUE(pass.busy());
    release.set_value();
    EXPECT_TRUE(settle(pass));
    EXPECT_EQ(runs.load(), 1);

    EXPECT_TRUE(pass.offer(false));  // a new pass once the last one ended
    EXPECT_TRUE(settle(pass));
    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(max_active.load(), 1);  // never two passes at once
    pool.shutdown();
}

// 60 simulated seconds of ticks (one offer per simulated second, the calendar flag every 15th): every pass runs off the
// UI thread, one at a time, and every pass runs once. The work stands for the scheduler tick, whose store calls are the
// ones that must not run on the UI thread; the store-call count of the verify clause is the number of passes that ran on
// the UI thread, which is zero.
TEST(PeriodicPass, SixtySimulatedSecondsOfTicksRunOffTheUiThreadOneAtATime) {
    ThreadPool pool{2};
    const auto ui = std::this_thread::get_id();
    std::mutex mx;
    std::vector<std::thread::id> pass_threads;
    int calendar_passes = 0;
    std::atomic<int> active{0}, max_active{0};
    PeriodicPass pass(on(pool), [&](bool calendar_due) {
        const int a = ++active;
        int seen = max_active.load();
        while (a > seen && !max_active.compare_exchange_weak(seen, a)) {
        }
        {
            const std::lock_guard lock(mx);
            pass_threads.push_back(std::this_thread::get_id());
            calendar_passes += calendar_due ? 1 : 0;
        }
        --active;
    });
    int queued = 0;
    for (int sec = 0; sec < 60; ++sec) {
        queued += pass.offer(sec % 15 == 5) ? 1 : 0;
        EXPECT_TRUE(settle(pass)) << "second " << sec;  // the next tick comes a simulated second later
    }
    EXPECT_EQ(queued, 60);
    EXPECT_EQ(pass_threads.size(), 60u);
    const std::lock_guard lock(mx);
    EXPECT_EQ(std::count(pass_threads.begin(), pass_threads.end(), ui), 0);  // no pass on the UI thread
    EXPECT_EQ(calendar_passes, 4);  // seconds 5, 20, 35 and 50
    EXPECT_EQ(max_active.load(), 1);
    pool.shutdown();
}

// D6: each UI task finishes within 50 ms, and the 95th percentile is 16 ms or less. The offer is the UI thread's part of
// the pass; here the pass takes 150 ms, so the offers must not wait for it.
TEST(PeriodicPass, TheUiThreadPartStaysUnderTheD6Budget) {
    ThreadPool pool{2};
    PeriodicPass pass(on(pool), [](bool) { std::this_thread::sleep_for(150ms); });
    std::vector<double> offers;
    int queued = 0;
    for (int i = 0; i < 200; ++i) {
        const auto t0 = clock_type::now();
        queued += pass.offer(i % 15 == 0) ? 1 : 0;
        offers.push_back(ms(clock_type::now() - t0));
        std::this_thread::sleep_for(1ms);
    }
    std::sort(offers.begin(), offers.end());
    const double p95 = offers[offers.size() * 95 / 100];
    EXPECT_GE(queued, 1);
    EXPECT_LT(queued, 200);  // the slow pass held the later offers back
    EXPECT_LE(p95, 16.0);
    EXPECT_LT(offers.back(), 50.0);
    EXPECT_TRUE(settle(pass));
    pool.shutdown();
}
