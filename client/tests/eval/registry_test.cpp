// The registry of named instructions and settings that evaluation replays may override (add-evaluation-export design D3).
#include <gtest/gtest.h>

#include "agenda/core.h"
#include "eval/registry.h"
#include "knowledge/core.h"
#include "triage/classifier.h"
#include "triage/triage.h"

using wb::json;
using wb::eval::Step;
using wb::eval::Variant;

TEST(Registry, UnknownNameRejected) {
    auto v = Variant::parse(Step::triage, json{{"triage.instructons", "x"}});
    ASSERT_FALSE(v.ok());
    EXPECT_NE(v.error().message.find("triage.instructons"), std::string::npos);
}

TEST(Registry, OutOfRangeRejected) {
    EXPECT_FALSE(Variant::parse(Step::triage, json{{"triage.batch_size", 0}}).ok());
    EXPECT_FALSE(Variant::parse(Step::asr, json{{"asr.beam_paths", 9}}).ok());
    EXPECT_FALSE(Variant::parse(Step::diarize, json{{"diarize.clusters", "many"}}).ok());
    EXPECT_FALSE(Variant::parse(Step::triage, json{{"triage.instructions", 5}}).ok());
    EXPECT_TRUE(Variant::parse(Step::triage, json{{"triage.batch_size", 5}}).ok());
}

TEST(Registry, WrongStepRejected) {
    EXPECT_FALSE(Variant::parse(Step::triage, json{{"diarize.threshold", 0.6}}).ok());
    EXPECT_FALSE(Variant::parse(Step::triage, json::array()).ok());
}

TEST(Registry, DefaultsMatchLive) {
    EXPECT_EQ(Variant::defaults(Step::triage).text("triage.instructions"), wb::triage::classification_instructions());
    EXPECT_EQ(Variant::defaults(Step::triage).number("triage.batch_size"), static_cast<double>(wb::triage::kBatch));
    EXPECT_EQ(Variant::defaults(Step::triage).number("triage.body_limit"), static_cast<double>(wb::triage::kMaxEventText));
    EXPECT_EQ(Variant::defaults(Step::agenda).text("agenda.instructions"), wb::agenda::detection_instructions());
    EXPECT_EQ(Variant::defaults(Step::agenda).text("agenda.merge_instructions"), wb::agenda::merge_instructions());
    EXPECT_EQ(Variant::defaults(Step::knowledge).text("knowledge.extraction"), wb::knowledge::extraction_system_text());
    EXPECT_EQ(Variant::defaults(Step::knowledge).text("knowledge.score"), wb::knowledge::score_instructions());
    EXPECT_EQ(Variant::defaults(Step::knowledge).text("knowledge.resolve"), wb::knowledge::resolve_instructions());
    EXPECT_EQ(Variant::defaults(Step::diarize).number("diarize.threshold"), 0.5);
    EXPECT_EQ(Variant::defaults(Step::diarize).choice("diarize.clusters"), "auto");
    EXPECT_EQ(Variant::defaults(Step::asr).number("asr.hotword_score"), 2.0);
    EXPECT_EQ(Variant::defaults(Step::asr).number("asr.beam_paths"), 4.0);
    EXPECT_EQ(Variant::defaults(Step::asr).number("asr.vad_threshold"), 0.5);
}

TEST(Registry, OverrideApplies) {
    auto v = Variant::parse(Step::triage, json{{"triage.instructions", "Be brief."}, {"triage.batch_size", 5}});
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v->text("triage.instructions"), "Be brief.");
    EXPECT_EQ(v->number("triage.batch_size"), 5.0);
    EXPECT_EQ(v->number("triage.body_limit"), 8192.0);
}

TEST(Registry, InstructionIdStable) {
    const std::string a = Variant::defaults(Step::triage).instruction_id();
    EXPECT_EQ(a.size(), 12u);
    EXPECT_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(a, Variant::defaults(Step::triage).instruction_id());
    auto v = Variant::parse(Step::triage, json{{"triage.batch_size", 10}});
    ASSERT_TRUE(v.ok());
    EXPECT_NE(v->instruction_id(), a);
    EXPECT_EQ(Variant::parse(Step::triage, json::object())->instruction_id(), a);
}

TEST(Registry, StepNamesRoundTrip) {
    for (Step s : {Step::triage, Step::agenda, Step::knowledge, Step::diarize, Step::asr})
        EXPECT_EQ(wb::eval::parse_step(wb::eval::to_string(s)), s);
    EXPECT_FALSE(wb::eval::parse_step("triagee").has_value());
}
