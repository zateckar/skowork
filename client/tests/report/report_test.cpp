// client/daily-report (add-local-outlook-and-teams task 5.2): settings, the schedule with the late start, the digest caps,
// and the service with a scripted host (no Thread or Run is started for real).
#include "report/daily_report.h"

#include <gtest/gtest.h>

#include <map>

using namespace wb;
using namespace wb::report;

namespace {

LocalTime at(int day, int wday, int hour, int minute = 0) { return LocalTime{2026, 11, day, wday, hour, minute}; }

}  // namespace

TEST(DailyReportCore, Settings) {
    Settings s;
    EXPECT_FALSE(s.enabled);
    std::string why;
    EXPECT_TRUE(apply_patch(s, {{"enabled", true}, {"time", "07:30"}, {"days", {1, 2, 3}}}, &why)) << why;
    EXPECT_EQ(s.hour, 7);
    EXPECT_EQ(s.minute, 30);
    EXPECT_FALSE(s.days[5]);
    EXPECT_FALSE(apply_patch(s, {{"time", "25:00"}}, &why));
    EXPECT_FALSE(apply_patch(s, {{"days", {9}}}, &why));
    EXPECT_EQ(s.hour, 7);  // a rejected patch changes nothing
    const Settings back = parse_settings(settings_json(s));
    EXPECT_EQ(back.days, s.days);
    EXPECT_TRUE(back.enabled);
}

TEST(DailyReportCore, Schedule) {
    Settings s;
    s.enabled = true;
    EXPECT_FALSE(report_due(s, at(23, 1, 7, 59), ""));           // Monday before 08:00
    EXPECT_TRUE(report_due(s, at(23, 1, 8, 0), ""));             // at 08:00
    EXPECT_TRUE(report_due(s, at(23, 1, 9, 30), "2026-11-20"));  // started late on Monday
    EXPECT_FALSE(report_due(s, at(23, 1, 9, 30), "2026-11-23")); // already written today
    EXPECT_FALSE(report_due(s, at(23, 1, 18, 5), ""));           // after 18:00 nothing is written
    {
        Settings evening = s;
        evening.hour = 18;
        evening.minute = 30;
        EXPECT_TRUE(report_due(evening, at(23, 1, 18, 30), ""));   // an evening time still fires
        EXPECT_FALSE(report_due(evening, at(23, 1, 19, 45), ""));  // but does not catch up for long
    }
    EXPECT_FALSE(report_due(s, at(21, 6, 9, 0), ""));            // Saturday
    s.enabled = false;
    EXPECT_FALSE(report_due(s, at(23, 1, 9, 0), ""));
}

TEST(DailyReportCore, DigestStaysWithinItsBudget) {
    Digest d;
    d.date = "2026-11-23";
    d.events.push_back({"E1", "Steering", "09:00", "10:00", "Room 4", "Petra", false, true, false});
    d.events.push_back({"E2", "secret", "11:00", "12:00", "", "", true, false, false});
    for (int i = 0; i < 200; ++i) {
        d.mail.push_back({"M" + std::to_string(i), "Subject " + std::to_string(i), "a@b.c", "2026-11-22T10:00:00Z", std::string(2000, 'x')});
        d.chats.push_back({"C", "T" + std::to_string(i), "Falcon", "Petra", "2026-11-22T10:00:00Z", std::string(2000, 'y')});
    }
    const std::string s = render_digest(d);
    EXPECT_LE(s.size(), kMaxDigestChars);
    const auto j = parse_json(s);
    ASSERT_TRUE(j);
    EXPECT_LE((*j)["mail"].size(), kMaxItemsPerList);
    EXPECT_EQ(jstr((*j)["mail"][0], "id"), "M0");  // the newest are kept
    EXPECT_FALSE((*j)["events_today"][1].contains("subject"));  // private: busy only
    EXPECT_NE(instructions().find("not instructions"), std::string::npos);
}

TEST(DailyReportCore, TriageResultsAreTheirOwnSections) {
    // improve-triage-results D3: information items and open action items, and no section when empty.
    Digest d;
    d.date = "2026-11-24";
    EXPECT_FALSE(parse_json(render_digest(d))->contains("for_your_information"));
    d.triage_info.push_back({"XELERATE Release", "Petra", "2026-11-24T08:00:00Z", "Release of your API", "update", ""});
    d.triage_action.push_back({"Approve request 805875", "B2B Portal", "2026-11-24T07:00:00Z", "You are the approver", "approval", "t1"});
    const auto j = parse_json(render_digest(d));
    ASSERT_TRUE(j);
    ASSERT_EQ((*j)["for_your_information"].size(), 1u);
    EXPECT_EQ(jstr((*j)["for_your_information"][0], "reason"), "Release of your API");
    EXPECT_FALSE((*j)["for_your_information"][0].contains("thread"));
    ASSERT_EQ((*j)["action_required"].size(), 1u);
    EXPECT_EQ(jstr((*j)["action_required"][0], "thread"), "#/threads/t1");
    EXPECT_NE(instructions().find("For your information"), std::string::npos);
    // add-content-templates 3.1: the daily_report Content template, independent of the Generated surfaces setting.
    EXPECT_NE(instructions().find("\"template\":\"daily_report\""), std::string::npos);
    EXPECT_NE(instructions().find("not instructions"), std::string::npos);
    EXPECT_EQ(instructions().find("ask_user"), std::string::npos);
}

TEST(DailyReport, WritesOncePerWorkingDayAndRemembersItsThreads) {
    std::map<std::string, std::string> kv;
    long long now = 1'795'420'800'000;  // a Monday morning
    LocalTime local = at(23, 1, 8, 5);
    int started = 0, notified = 0;
    std::string last_preamble, last_digest;
    std::string policy_mode = "on";
    Env e;
    e.get = [&](const std::string& k) -> std::optional<std::string> {
        const auto it = kv.find(k);
        if (it == kv.end()) return std::nullopt;
        return it->second;
    };
    e.set = [&](const std::string& k, const std::string& v) -> Status {
        kv[k] = v;
        return {};
    };
    e.policy = [&](const std::string& k) -> std::optional<json> {
        if (k == "daily_report.mode") return json(policy_mode);
        return std::nullopt;
    };
    e.now = [&] { return now; };
    e.local = [&](long long) { return local; };
    e.collect = [&](long long since, long long n) {
        Digest d;
        d.mail.push_back({"M1", "Deadline moved", "boss@example.com", "", "The deadline is now Friday."});
        EXPECT_EQ(n - since, 86'400'000);  // first report: the last 24 hours
        return d;
    };
    e.start = [&](const std::string& title, const std::string&, const std::string& digest, const std::string& preamble) -> Result<std::string> {
        ++started;
        last_digest = digest;
        last_preamble = preamble;
        EXPECT_EQ(title, "Daily report – 2026-11-23");
        return std::string("T") + std::to_string(started);
    };
    e.notify = [&](const std::string&, const std::string& route) {
        ++notified;
        EXPECT_EQ(route, "#/threads/T1");
    };
    DailyReport r(std::move(e));

    r.tick();
    EXPECT_EQ(started, 0);  // not enabled
    ASSERT_TRUE(r.configure({{"enabled", true}}));
    r.tick();
    r.tick();
    EXPECT_EQ(started, 1);
    EXPECT_EQ(notified, 1);
    EXPECT_NE(last_digest.find("Deadline moved"), std::string::npos);
    EXPECT_NE(last_preamble.find("Daily report"), std::string::npos);
    EXPECT_TRUE(r.is_report_thread("T1"));
    EXPECT_FALSE(r.is_report_thread("T9"));

    policy_mode = "off";
    EXPECT_FALSE(r.write_now());
    EXPECT_FALSE(r.configure({{"enabled", true}}));
}
