// The user decision log (client/evaluation User Decision Log; add-evaluation-export design D8).
#include <gtest/gtest.h>

#include "eval/decision_log.h"
#include "support/graph_mock.h"

using namespace wb;
using eval::Decision;
using eval::DecisionLog;

namespace {

constexpr long long kDay = 86'400'000LL;
constexpr long long kNow = 1'790'000'000'000LL;

Decision decision(const std::string& kind, const std::string& item, long long at, json before = json::object(),
                  json after = json::object()) {
    Decision d;
    d.at_ms = at;
    d.step = "triage";
    d.kind = kind;
    d.item_ref = item;
    d.result_ref = "triage:mail/" + item;
    d.before = std::move(before);
    d.after = std::move(after);
    d.instruction_id = "0123456789ab";
    return d;
}

}  // namespace

TEST(DecisionLog, RecordsAndReadsBack) {
    MailDb db;
    DecisionLog log(db.store->db());
    ASSERT_TRUE(log.record(decision("no_action", "mail:m1", kNow, {{"action", "required"}}, {{"suggestion", "no_action"}})));
    const auto all = log.for_item("mail:m1");
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].kind, "no_action");
    EXPECT_EQ(all[0].step, "triage");
    EXPECT_EQ(all[0].at_ms, kNow);
    EXPECT_EQ(all[0].before["action"], "required");
    EXPECT_EQ(all[0].after["suggestion"], "no_action");
    EXPECT_EQ(all[0].instruction_id, "0123456789ab");
    EXPECT_EQ(all[0].result_ref, "triage:mail/mail:m1");
}

TEST(DecisionLog, LaterDecisionKeepsEarlier) {
    MailDb db;
    DecisionLog log(db.store->db());
    ASSERT_TRUE(log.record(decision("dismiss", "mail:m1", kNow)));
    ASSERT_TRUE(log.record(decision("start", "mail:m1", kNow + 1000)));
    const auto all = log.for_item("mail:m1");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].kind, "dismiss");
    EXPECT_EQ(all[1].kind, "start");
}

TEST(DecisionLog, BetweenIsOrderedAndBounded) {
    MailDb db;
    DecisionLog log(db.store->db());
    ASSERT_TRUE(log.record(decision("dismiss", "mail:a", kNow - 2 * kDay)));
    ASSERT_TRUE(log.record(decision("start", "mail:b", kNow - kDay)));
    ASSERT_TRUE(log.record(decision("no_action", "mail:c", kNow)));
    const auto mid = log.between(kNow - kDay, kNow);
    ASSERT_EQ(mid.size(), 2u);
    EXPECT_EQ(mid[0].item_ref, "mail:b");
    EXPECT_EQ(mid[1].item_ref, "mail:c");
}

TEST(DecisionLog, ForgetRemovesEntries) {
    MailDb db;
    DecisionLog log(db.store->db());
    ASSERT_TRUE(log.record(decision("dismiss", "mail:m1", kNow)));
    ASSERT_TRUE(log.record(decision("dismiss", "mail:m2", kNow)));
    Decision rename = decision("speaker_rename", "meeting:mtg1", kNow);
    rename.step = "meeting";
    rename.result_ref = "meeting:mtg1/s1";
    ASSERT_TRUE(log.record(rename));
    ASSERT_TRUE(log.forget_item("mail:m1"));
    EXPECT_TRUE(log.for_item("mail:m1").empty());
    EXPECT_EQ(log.for_item("mail:m2").size(), 1u);
    ASSERT_TRUE(log.forget_item("meeting:mtg1"));
    EXPECT_TRUE(log.for_item("meeting:mtg1").empty());
}

TEST(DecisionLog, PruneAfter365Days) {
    MailDb db;
    DecisionLog log(db.store->db());
    ASSERT_TRUE(log.record(decision("dismiss", "mail:old", kNow - 366 * kDay)));
    ASSERT_TRUE(log.record(decision("dismiss", "mail:new", kNow - 10 * kDay)));
    EXPECT_EQ(log.prune(kNow), 1);
    EXPECT_TRUE(log.for_item("mail:old").empty());
    EXPECT_EQ(log.for_item("mail:new").size(), 1u);
}

TEST(DecisionLog, TruncatesLargeValues) {
    MailDb db;
    DecisionLog log(db.store->db());
    ASSERT_TRUE(log.record(decision("fact_edit", "mail:m1", kNow, {{"text", std::string(10'000, 'x')}})));
    const auto all = log.for_item("mail:m1");
    ASSERT_EQ(all.size(), 1u);
    EXPECT_LE(all[0].before.dump().size(), 2048u);
    EXPECT_TRUE(all[0].before.is_string());  // a value over the limit is kept as a shortened string
}

TEST(DecisionLog, ForgetDuringQuery) {
    MailDb db;
    DecisionLog log(db.store->db());
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(log.record(decision("dismiss", "mail:m" + std::to_string(i), kNow + i)));
    const auto first = log.between(kNow, kNow + 1);
    ASSERT_EQ(first.size(), 2u);
    ASSERT_TRUE(log.forget_item("mail:m3"));
    const auto second = log.between(kNow + 2, kNow + 3);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].item_ref, "mail:m2");
}

TEST(DecisionLog, ItemRefs) {
    EXPECT_EQ(eval::item_ref_mail("m1"), "mail:m1");
    EXPECT_EQ(eval::item_ref_teams("c1", "x"), "teams:c1/x");
    EXPECT_EQ(eval::item_ref_calendar("e1"), "calendar:e1");
    EXPECT_EQ(eval::item_ref_meeting("mtg"), "meeting:mtg");
}

// Review minor 10: an Agenda decision is forgotten with any of its item's sources, not only the first.
TEST(DecisionLog, AgendaEntriesGoWithAnyOfTheirSources) {
    MailDb db;
    DecisionLog log(db.store->db());
    Decision d = decision("agenda_done", "mail:a", kNow);
    d.step = "agenda";
    d.result_ref = "agenda:item1";
    d.before = {{"state", "open"}, {"sources", {"mail:a", "teams:c/m"}}};
    ASSERT_TRUE(log.record(d));
    ASSERT_TRUE(log.forget_item("teams:c/m"));
    EXPECT_TRUE(log.for_item("mail:a").empty());
}
