// The host's relay of live Run events (client/work-inbox Live Feed Updates, design D3): deltas merge per interval, deltas
// of Threads that are not open are dropped, and other events keep their order behind the Run's pending deltas.
#include "app/run_event_relay.h"

#include <gtest/gtest.h>

using namespace wb;

namespace {
json delta(const std::string& run, const std::string& thread, const std::string& kind, const std::string& text) {
    return {{"kind", kind}, {"run_id", run}, {"thread_id", thread}, {"seq", 0}, {"at", ""}, {"data", {{"text", text}}}};
}
}  // namespace

// 2,000 deltas in 2 seconds: at most 60 events go to the window, and no text is lost.
TEST(RunEventRelay, TwoThousandDeltasInTwoSecondsSendAtMostSixtyEvents) {
    RunEventRelay relay(34);
    std::vector<json> sent;
    for (long long t = 0; t < 2000; ++t) {  // one delta per millisecond
        for (auto& e : relay.offer(delta("run_1", "thr_1", "text.delta", "x"), t, true)) sent.push_back(std::move(e));
        for (auto& e : relay.due(t)) sent.push_back(std::move(e));
    }
    for (auto& e : relay.due(3000)) sent.push_back(std::move(e));
    EXPECT_FALSE(relay.pending());
    EXPECT_LE(sent.size(), 60u);
    std::string text;
    for (const json& e : sent) text += jstr(e["data"], "text");
    EXPECT_EQ(text.size(), 2000u);
}

// The deltas of a Thread that is not open never reach the window; events of that Thread still do.
TEST(RunEventRelay, DeltasOfAThreadThatIsNotOpenAreDropped) {
    RunEventRelay relay(34);
    EXPECT_TRUE(relay.offer(delta("run_2", "thr_2", "text.delta", "hidden"), 0, false).empty());
    EXPECT_TRUE(relay.offer(delta("run_2", "thr_2", "reasoning.delta", "hidden"), 40, false).empty());
    EXPECT_FALSE(relay.pending());
    const json state = {{"kind", "run.state"}, {"run_id", "run_2"}, {"thread_id", "thr_2"}, {"seq", 3}, {"data", {{"state", "running"}}}};
    const auto out = relay.offer(state, 50, false);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["kind"], "run.state");
}

// A stored event goes out after the pending deltas of its Run, so the window sees the text before the answer.
TEST(RunEventRelay, PendingDeltasGoBeforeTheNextEventOfTheirRun) {
    RunEventRelay relay(34);
    ASSERT_EQ(relay.offer(delta("run_1", "thr_1", "text.delta", "Let "), 0, true).size(), 1u);  // at once: nothing sent recently
    EXPECT_TRUE(relay.offer(delta("run_1", "thr_1", "text.delta", "me"), 5, true).empty());  // held for the interval
    EXPECT_TRUE(relay.pending());
    EXPECT_EQ(relay.next_due(), 34);
    const json answer = {{"kind", "message"}, {"run_id", "run_1"}, {"thread_id", "thr_1"}, {"seq", 7}, {"data", {{"role", "assistant"}}}};
    const auto out = relay.offer(answer, 6, true);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0]["kind"], "text.delta");
    EXPECT_EQ(jstr(out[0]["data"], "text"), "me");
    EXPECT_EQ(out[1]["kind"], "message");
    EXPECT_FALSE(relay.pending());
}

// A different kind of delta flushes the pending one first, so the order of the Run's streams holds.
TEST(RunEventRelay, ReasoningAndTextOfOneRunKeepTheirOrder) {
    RunEventRelay relay(34);
    relay.offer(delta("run_1", "thr_1", "reasoning.delta", "a"), 0, true);       // sent at once
    EXPECT_TRUE(relay.offer(delta("run_1", "thr_1", "reasoning.delta", "b"), 1, true).empty());  // held
    const auto out = relay.offer(delta("run_1", "thr_1", "text.delta", "c"), 2, true);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["kind"], "reasoning.delta");
    EXPECT_EQ(jstr(out[0]["data"], "text"), "b");
}
