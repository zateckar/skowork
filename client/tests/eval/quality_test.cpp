// Daily quality counters (client/telemetry Quality Counters; add-evaluation-export design D9).
#include <gtest/gtest.h>

#include "eval/decision_log.h"
#include "eval/quality.h"
#include "support/graph_mock.h"
#include "telemetry/catalogue.gen.h"

#include <set>

using namespace wb;
using eval::QualityCounters;

namespace {

constexpr long long kDay = 86'400'000LL;
constexpr long long kNow = 1'790'000'000'000LL;  // 2026-09-21

using Emitted = std::vector<std::vector<telemetry::Field>>;

std::function<void(const std::vector<telemetry::Field>&)> into(Emitted& out) {
    return [&out](const std::vector<telemetry::Field>& f) { out.push_back(f); };
}

long long value_of(const std::vector<telemetry::Field>& fields, telemetry::Attr a) {
    for (const auto& f : fields)
        if (f.attr == a) return std::get<long long>(f.value);
    return -1;
}

std::string text_of(const std::vector<telemetry::Field>& fields, telemetry::Attr a) {
    for (const auto& f : fields)
        if (f.attr == a) return std::get<std::string>(f.value);
    return {};
}

}  // namespace

TEST(Quality, UtcDay) {
    EXPECT_EQ(eval::utc_day(0), "1970-01-01");
    EXPECT_EQ(eval::utc_day(kNow), "2026-09-21");
    EXPECT_EQ(eval::utc_day(kNow + kDay), "2026-09-22");
}

TEST(Quality, FlushEmitsPreviousDaysOnly) {
    MailDb db;
    QualityCounters q(db.store->db());
    q.add(eval::utc_day(kNow - kDay), "triage", "dismissed", "0123456789ab", 6, "request", "required");
    q.add(eval::utc_day(kNow), "triage", "dismissed", "0123456789ab", 2, "request", "required");
    Emitted out;
    EXPECT_EQ(q.flush(into(out), kNow, true), 1);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(value_of(out[0], telemetry::Attr::quality_value), 6);
    EXPECT_EQ(text_of(out[0], telemetry::Attr::quality_counter), "dismissed");
    EXPECT_EQ(text_of(out[0], telemetry::Attr::quality_day), eval::utc_day(kNow - kDay));  // the day counted, not the day sent
    const auto left = q.rows();
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].day, eval::utc_day(kNow));
}

TEST(Quality, SurvivesRestartAndIsNeverSentTwice) {
    MailDb db;
    {
        QualityCounters q(db.store->db());
        q.add(eval::utc_day(kNow - kDay), "agenda", "agenda_done", "0123456789ab");
    }
    QualityCounters again(db.store->db());
    Emitted out;
    EXPECT_EQ(again.flush(into(out), kNow, true), 1);
    EXPECT_EQ(again.flush(into(out), kNow, true), 0);
    EXPECT_EQ(out.size(), 1u);
}

TEST(Quality, UnknownCountersAndValuesAreNotKept) {
    MailDb db;
    QualityCounters q(db.store->db());
    q.add("2026-09-20", "triage", "subject_line", "0123456789ab");
    q.add("2026-09-20", "mail", "dismissed", "0123456789ab");
    q.add("2026-09-20", "triage", "dismissed", "not-an-id", 1, "Petr Novak", "maybe");
    const auto rows = q.rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].instruction_id, "unknown");
    EXPECT_EQ(rows[0].category, "none");
    EXPECT_EQ(rows[0].action, "none");
}

TEST(Quality, CollectCountsDecisionsOncePerDay) {
    MailDb db;
    eval::DecisionLog log(db.store->db());
    eval::Decision d;
    d.at_ms = kNow - kDay;
    d.step = "triage";
    d.kind = "dismiss";
    d.item_ref = "mail:m1";
    d.result_ref = "triage:mail/m1";
    d.before = {{"category", "request"}, {"action", "required"}};
    d.instruction_id = "0123456789ab";
    ASSERT_TRUE(log.record(d));
    QualityCounters q(db.store->db());
    q.collect(kNow);
    q.collect(kNow);
    const auto rows = q.rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].counter, "dismissed");
    EXPECT_EQ(rows[0].category, "request");
    EXPECT_EQ(rows[0].value, 1);
}

TEST(Quality, UntouchedCountedOnce) {
    MailDb db;
    ASSERT_TRUE(db.store->db().exec("INSERT INTO triage_items(scope, item_id, revision, state, suggestion, queued_at, updated_at, "
                                    "action, category) VALUES('mail', 'm1', 'r1', 'evaluated', 'open', 1, 1, 'required', 'request')"));
    QualityCounters q(db.store->db());
    q.collect(kNow + 9 * kDay);
    q.collect(kNow + 10 * kDay);
    long long untouched = 0;
    for (const auto& r : q.rows())
        if (r.counter == "untouched_7d") untouched += r.value;
    EXPECT_EQ(untouched, 1);
}

TEST(Quality, OffLevelSendsNothing) {
    MailDb db;
    QualityCounters q(db.store->db());
    q.add(eval::utc_day(kNow - kDay), "triage", "dismissed", "0123456789ab");
    q.add(eval::utc_day(kNow - 9 * kDay), "triage", "dismissed", "0123456789ab");
    Emitted out;
    EXPECT_EQ(q.flush(into(out), kNow, false), 0);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(q.rows().empty());  // review minor 11: counts made while telemetry is off are never sent later
}

TEST(Quality, AttributesAllowList) {
    eval::QualityRow r{"2026-09-20", "triage", "dismissed", "request", "required", "0123456789ab", 3};
    const auto fields = eval::quality_fields(r);
    std::set<telemetry::Attr> got, want;
    for (const auto& f : fields) got.insert(f.attr);
    for (const auto a : telemetry::def(telemetry::Event::quality_daily).attrs) want.insert(a);
    EXPECT_EQ(got, want);
    for (const auto& f : fields) {
        EXPECT_EQ(telemetry::def(f.attr).level, telemetry::Level::metadata);
        if (const auto* s = std::get_if<std::string>(&f.value)) {
            EXPECT_EQ(s->find('@'), std::string::npos);
            EXPECT_EQ(s->find(' '), std::string::npos);
            EXPECT_LE(s->size(), 24u);
        }
    }
}

// Review minor 12: marks of finished days, gone items and gone meetings are pruned.
TEST(Quality, MarksArePruned) {
    MailDb db;
    auto& d = db.store->db();
    ASSERT_TRUE(d.exec("INSERT INTO quality_marks(kind, ref) VALUES('collected', '2026-01-01'), ('untouched', 'mail|gone'), "
                       "('unnamed', 'mtg_gone/s1')"));
    QualityCounters q(d);
    q.collect(kNow);
    auto c = d.prepare("SELECT count(*) FROM quality_marks WHERE ref IN ('2026-01-01', 'mail|gone', 'mtg_gone/s1')");
    ASSERT_TRUE(c.step());
    EXPECT_EQ(c.col_int(0), 0);
}
