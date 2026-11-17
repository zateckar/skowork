// client/agenda core (add-agenda tasks 2.1, 2.2, 3.1): the detection parser, admission rules, due dates and merge matching.
#include <gtest/gtest.h>

#include "agenda/core.h"
#include "base/time.h"

using namespace wb;
using namespace wb::agenda;

namespace {

constexpr int kCet = 60;  // UTC+1 in November

long long ms(const char* iso) { return *parse_iso8601_ms(iso); }

// Tuesday 17 November 2026, 09:00 local (08:00 UTC).
const long long kSent = ms("2026-11-17T08:00:00Z");
const long long kNow = ms("2026-11-17T10:00:00Z");

Identity me() { return {"Jana Nováková", {"jana.novakova@example.cz"}}; }

SourceItem mail(const std::string& text) {
    SourceItem it;
    it.kind = "mail";
    it.id = "m1";
    it.conversation = "conv1";
    it.title = "Q4 slides";
    it.text = text;
    it.author = "Petr Svoboda";
    it.user_addressed = true;
    it.recipients = 1;
    it.at_ms = kSent;
    return it;
}

Candidate cand(const std::string& quote, const std::string& due = "2026-11-20", const std::string& phrase = "by Friday") {
    Candidate c;
    c.source_id = "m1";
    c.title = "Send the Q4 slides";
    c.action = "Send Petr the Q4 slides";
    c.due = due;
    c.due_phrase = phrase;
    c.due_kind = "inferred";
    c.responsibility = "requested";
    c.quote = quote;
    c.draft_kind = "reply";
    c.confidence = 0.9;
    return c;
}

std::string response(const std::string& candidates_json, const std::string& id = "m1") {
    return R"({"items":[{"id":")" + id + R"(","candidates":)" + candidates_json + "}]}";
}

const std::string kGood = R"([{"title":"Send the Q4 slides","action":"Send Petr the slides","due":"2026-11-20","due_phrase":"by Friday",)"
                          R"("due_kind":"inferred","responsibility":"requested","quote":"send me the Q4 slides","confidence":0.9,"draft_kind":"reply"}])";

}  // namespace

TEST(AgendaParse, AcceptsOneEntryPerItemInOrder) {
    const std::vector<SourceItem> items = {mail("x")};
    auto r = parse_detection(response(kGood), items);
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].source_id, "m1");
    EXPECT_EQ((*r)[0].due, "2026-11-20");
    EXPECT_EQ((*r)[0].draft_kind, "reply");
    EXPECT_TRUE(parse_detection(response("[]"), items));
}

TEST(AgendaParse, RejectsTheWholeBatchOnAnyDeviation) {
    const std::vector<SourceItem> items = {mail("x")};
    EXPECT_FALSE(parse_detection("not json", items));
    EXPECT_FALSE(parse_detection(response(kGood, "other"), items));                       // wrong id
    EXPECT_FALSE(parse_detection(R"({"items":[]})", items));                                // missing item
    EXPECT_FALSE(parse_detection(R"({"items":[],"extra":1})", items));                      // unknown field
    std::string extra_field = kGood;
    extra_field.insert(extra_field.size() - 2, R"(,"send":true)");
    EXPECT_FALSE(parse_detection(response(extra_field), items));                            // unknown candidate field
    std::string bad_kind = kGood;
    bad_kind.replace(bad_kind.find("\"inferred\""), 10, "\"maybe\"");
    EXPECT_FALSE(parse_detection(response(bad_kind), items));
    std::string bad_conf = kGood;
    bad_conf.replace(bad_conf.find("0.9"), 3, "1.5");
    EXPECT_FALSE(parse_detection(response(bad_conf), items));
    const std::string four = "[" + kGood.substr(1, kGood.size() - 2) + "," + kGood.substr(1, kGood.size() - 2) + "," +
                             kGood.substr(1, kGood.size() - 2) + "," + kGood.substr(1, kGood.size() - 2) + "]";
    EXPECT_FALSE(parse_detection(response(four), items));                                   // more than three
    EXPECT_FALSE(parse_detection(R"({"items":[{"id":"m1","id":"m1","candidates":[]}]})", items));  // duplicate key
}

TEST(AgendaAdmission, DirectRequestWithAWeekdayIsAdmitted) {
    const auto item = mail("Hi Jana,\nplease  send me the Q4 slides by Friday.\nPetr");
    Due due;
    EXPECT_EQ(admit(cand("send me the Q4 slides by Friday"), item, me(), kNow, kCet, 0.7, &due), Drop::none);
    EXPECT_FALSE(due.has_time);
    EXPECT_TRUE(due.inferred);
    EXPECT_EQ(due.at_ms, ms("2026-11-20T22:59:00Z"));  // the end of Friday, local time
}

TEST(AgendaAdmission, InventedQuoteIsDropped) {
    const auto item = mail("Hi Jana, see you at lunch.");
    EXPECT_EQ(admit(cand("send me the Q4 slides"), item, me(), kNow, kCet, 0.7, nullptr), Drop::quote);
}

TEST(AgendaAdmission, BroadcastThatDoesNotNameTheUserIsDropped) {
    auto item = mail("All departments submit their budgets by 1 December.");
    item.recipients = 40;
    item.user_addressed = true;
    auto c = cand("submit their budgets by 1 December", "2026-12-01", "by 1 December");
    c.due_kind = "explicit";
    EXPECT_EQ(admit(c, item, me(), kNow, kCet, 0.7, nullptr), Drop::broadcast);
    item.text = "All departments submit their budgets by 1 December. Nováková: please also add the battery project.";
    EXPECT_EQ(admit(c, item, me(), kNow, kCet, 0.7, nullptr), Drop::none);
}

TEST(AgendaAdmission, RequestToSomeoneElseIsDropped) {
    auto item = mail("Tomáš, send me the Q4 slides by Friday.");
    item.user_addressed = false;  // the user was only in Cc
    EXPECT_EQ(admit(cand("send me the Q4 slides by Friday"), item, me(), kNow, kCet, 0.7, nullptr), Drop::responsibility);
}

TEST(AgendaAdmission, SelfCommitmentNeedsTheUserAsAuthor) {
    auto item = mail("I'll send the estimate tomorrow.");
    auto c = cand("I'll send the estimate tomorrow", "2026-11-18", "tomorrow");
    c.responsibility = "self_committed";
    EXPECT_EQ(admit(c, item, me(), kNow, kCet, 0.7, nullptr), Drop::responsibility);
    item.author_is_user = true;
    EXPECT_EQ(admit(c, item, me(), kNow, kCet, 0.7, nullptr), Drop::none);
}

TEST(AgendaAdmission, LowConfidenceIsDropped) {
    const auto item = mail("please send me the Q4 slides by Friday");
    auto c = cand("send me the Q4 slides by Friday");
    c.confidence = 0.5;
    EXPECT_EQ(admit(c, item, me(), kNow, kCet, 0.7, nullptr), Drop::confidence);
}

TEST(AgendaDueDates, PastAndContradictingDatesAreDropped) {
    const auto item = mail("send me the Q4 slides by last Monday");
    EXPECT_EQ(admit(cand("send me the Q4 slides by last Monday", "2026-11-16", "by last Monday"), item, me(), kNow, kCet, 0.7, nullptr),
              Drop::date);  // before the mail was sent
    // "by Friday" with a Thursday date contradicts its phrase.
    const auto item2 = mail("send me the Q4 slides by Friday");
    EXPECT_EQ(admit(cand("send me the Q4 slides by Friday", "2026-11-19"), item2, me(), kNow, kCet, 0.7, nullptr), Drop::date);
    // Admitted on Tuesday, evaluated after Friday ended: past.
    EXPECT_EQ(admit(cand("send me the Q4 slides by Friday"), item2, me(), ms("2026-11-21T08:00:00Z"), kCet, 0.7, nullptr), Drop::past);
    // A malformed date.
    EXPECT_EQ(admit(cand("send me the Q4 slides by Friday", "next friday"), item2, me(), kNow, kCet, 0.7, nullptr), Drop::date);
}

TEST(AgendaDueDates, CzechAndGermanPhrasesAreChecked) {
    Candidate c = cand("q", "2026-11-20", "do pátku");
    EXPECT_TRUE(resolve_due(c, kSent, kCet));
    c.due_phrase = "bis Freitag";
    EXPECT_TRUE(resolve_due(c, kSent, kCet));
    c.due_phrase = "zítra";
    EXPECT_FALSE(resolve_due(c, kSent, kCet));
    c.due = "2026-11-18";
    EXPECT_TRUE(resolve_due(c, kSent, kCet));
    c.due = "2026-11-18T14:30";
    c.due_phrase = "morgen um 14:30";  // digits: the date-time is taken as stated
    const auto d = resolve_due(c, kSent, kCet);
    ASSERT_TRUE(d);
    EXPECT_TRUE(d->has_time);
    EXPECT_EQ(d->at_ms, ms("2026-11-18T13:30:00Z"));
}

TEST(AgendaMerge, TitleSimilarityAndMergeResponse) {
    EXPECT_GT(title_similarity("Send the Q4 slides", "Send Q4 slides to Petr"), 0.4);
    EXPECT_LT(title_similarity("Send the Q4 slides", "Book the venue"), 0.1);
    EXPECT_EQ(*parse_merge(R"({"same":"a1"})", {"a1", "a2"}), std::optional<std::string>("a1"));
    EXPECT_FALSE(parse_merge(R"({"same":null})", {"a1"})->has_value());
    EXPECT_FALSE(parse_merge(R"({"same":"zz"})", {"a1"}));
    EXPECT_FALSE(parse_merge(R"({"same":"a1","why":"x"})", {"a1"}));
}

TEST(AgendaDetection, DataCarriesLocalDatesAndWeekdays) {
    const json d = detection_data({mail("hello")}, me(), kNow, kCet);
    EXPECT_EQ(d["today"], "2026-11-17");
    EXPECT_EQ(d["items"][0]["local_date"], "2026-11-17T09:00");
    EXPECT_EQ(d["items"][0]["weekday"], "Tuesday");
    EXPECT_EQ(d["items"][0]["id"], "m1");
    EXPECT_NE(detection_instructions().find("untrusted"), std::string::npos);
}
