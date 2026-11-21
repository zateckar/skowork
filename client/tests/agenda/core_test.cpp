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

// Task 1.2: relative dates and time zones against a fixed table of source timestamps. A phrase that does not agree with
// the date drops the candidate; nothing is guessed.
TEST(AgendaDueDates, RelativePhrasesAgainstAFixedTable) {
    struct Row {
        const char* source;  // UTC
        int tz;
        const char* due;
        const char* phrase;
        bool ok;
    };
    const Row rows[] = {
        // Tuesday 17 November 2026, 09:00 CET.
        {"2026-11-17T08:00:00Z", 60, "2026-11-20", "by Friday", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-27", "by Friday", true},  // the Friday of next week
        {"2026-11-17T08:00:00Z", 60, "2026-11-19", "by Friday", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-24", "next week", true},
        {"2026-11-17T08:00:00Z", 60, "2026-12-08", "next week", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-17", "today", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-18T09:00", "tomorrow morning", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-19T09:00", "tomorrow morning", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-19", "the day after tomorrow", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-18", "the day after tomorrow", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-20", "do pátku", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-23", "v pondělí", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-25", "příští týden", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-18T08:00", "zítra ráno", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-19", "pozítří", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-18", "pozítří", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-20", "bis Freitag", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-24", "nächste Woche", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-18T08:00", "morgen früh", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-19", "morgen", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-23T09:00", "Montagmorgen", true},  // Monday morning, not tomorrow
        {"2026-11-17T08:00:00Z", 60, "2026-11-18T09:00", "Montagmorgen", false},
        {"2026-11-17T08:00:00Z", 60, "2026-11-17T11:00", "heute Morgen", true},  // this morning
        {"2026-11-17T08:00:00Z", 60, "2026-11-19", "übermorgen", true},
        {"2026-11-17T08:00:00Z", 60, "2026-11-18", "übermorgen", false},
        // Saturday 21 November, 00:30 CET: still Friday in UTC; "tomorrow" is Sunday.
        {"2026-11-20T23:30:00Z", 60, "2026-11-22", "tomorrow", true},
        {"2026-11-20T23:30:00Z", 60, "2026-11-21", "tomorrow", false},
        {"2026-11-20T23:30:00Z", 60, "2026-11-23", "Monday", true},
        // Friday 26 March 2027, 18:00 CET; summer time starts on Sunday 28 March.
        {"2027-03-26T17:00:00Z", 60, "2027-03-29", "on Monday", true},
        {"2027-03-26T17:00:00Z", 60, "2027-03-27", "tomorrow", true},
    };
    for (const auto& r : rows) {
        Candidate c = cand("q", r.due, r.phrase);
        EXPECT_EQ(resolve_due(c, ms(r.source), r.tz).has_value(), r.ok) << r.source << " " << r.due << " \"" << r.phrase << "\"";
    }
}

// The real gateway's small model wraps its JSON in one Markdown code fence (add-agenda task 1.1, 2026-11-19). Exactly
// one fenced block, labelled json or not and closed or not, is accepted; a fence with other text, or two blocks, rejects
// the batch.
TEST(AgendaParse, AcceptsOneFencedJsonBlockAndNothingElse) {
    const std::vector<SourceItem> items = {mail("x")};
    const std::string body = response(kGood);
    EXPECT_TRUE(parse_detection("\n\n```json\n" + body + "\n```\n", items));
    EXPECT_TRUE(parse_detection("```\n" + body + "\n```", items));
    EXPECT_FALSE(parse_detection("Here you go:\n```json\n" + body + "\n```", items));
    EXPECT_FALSE(parse_detection("```json\n" + body + "\n```\nDone.", items));
    EXPECT_FALSE(parse_detection("```json\n" + body + "\n```\n```json\n" + body + "\n```", items));
    EXPECT_FALSE(parse_detection("```python\n" + body + "\n```", items));
    EXPECT_TRUE(parse_detection("\n\n```json\n" + body, items));  // the real model sometimes omits the closing fence
    EXPECT_FALSE(parse_detection("```json\n" + body + "\nDone.", items));
}

// A self-commitment in a meeting belongs to the user only when the user speaks the quoted words (add-agenda task 1.1,
// 2026-11-19: the real model labels "Jana Nováková: I'll do it" as self_committed).
TEST(AgendaAdmission, SelfCommitmentInAMeetingNeedsTheUserAsSpeaker) {
    SourceItem m;
    m.kind = "meeting";
    m.id = "t1";
    m.text = "Tom\xC3\xA1\xC5\xA1 Kr\xC3\xA1l: We need numbers.\nJana Nov\xC3\xA1kov\xC3\xA1: I will prepare the cost comparison by Friday.\n"
             "Petr Dvo\xC5\x99\xC3\xA1k: I will update the FMEA by Friday.\n";
    m.at_ms = kNow - 3'600'000;
    Candidate c = cand("I will prepare the cost comparison by Friday.");
    c.source_id = "t1";
    c.responsibility = "self_committed";
    EXPECT_EQ(admit(c, m, me(), kNow, kCet, 0.7, nullptr), Drop::none);
    c.quote = "I will update the FMEA by Friday.";
    EXPECT_EQ(admit(c, m, me(), kNow, kCet, 0.7, nullptr), Drop::responsibility);
    SourceItem mail_item = mail("I will prepare the cost comparison by Friday.");  // in mail the author must be the user
    c.quote = "I will prepare the cost comparison by Friday.";
    c.source_id = mail_item.id;
    EXPECT_EQ(admit(c, mail_item, me(), kNow, kCet, 0.7, nullptr), Drop::responsibility);
}

// ---- Sending A Prepared Draft (add-local-outlook-and-teams task 4.3) ----

TEST(AgendaDraft, MarkdownHeadingSection) {
    const std::string answer =
        "## Brief\nPetra asked for the slides [1].\n\n## Draft\nHi Petra,\n\nthe Q4 slides are attached.\n\nJan\n\n## Sources\n[1] mail";
    EXPECT_EQ(extract_draft(answer), "Hi Petra,\n\nthe Q4 slides are attached.\n\nJan");
}

TEST(AgendaDraft, BoldHeadingFencesAndQuotes) {
    EXPECT_EQ(extract_draft("Brief text\n\n**Draft reply:**\n```\nDear team,\nagreed.\n```\n**Notes**\nx"), "Dear team,\nagreed.");
    EXPECT_EQ(extract_draft("### Návrh\n> Ahoj,\n> posílám.\n"), "Ahoj,\nposílám.");
    EXPECT_EQ(extract_draft("Draft: Sounds good, see you Monday."), "Sounds good, see you Monday.");
}

TEST(AgendaDraft, NoDraft) {
    EXPECT_EQ(extract_draft("## Brief\nNothing to send.\n## Drafting notes are not a draft"), "");
    EXPECT_EQ(extract_draft(""), "");
}
