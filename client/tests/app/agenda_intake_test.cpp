// client/agenda intake adapters and the preparation tool rule (add-agenda tasks 2.3, 2.4, 4.1).
#include "app/agenda_intake.h"
#include "app/agenda_reply.h"
#include "base/time.h"

#include <gtest/gtest.h>

using namespace wb;

namespace {

meeting::MeetingRecord meeting_record() {
    meeting::MeetingRecord m;
    m.id = "mt-1";
    m.thread_id = "th-1";
    m.title = "Budget review";
    m.series_id = "series-7";
    m.event_id = "ev-1";
    m.attendees = {"petr@example.cz", "jana.novakova@example.cz", "martin@example.cz"};
    m.started_at = 1'000;
    m.ended_at = 5'000;
    return m;
}

runtime::ToolCallInfo call(const std::string& run, const std::string& parent, runtime::Risk risk) {
    runtime::ToolCallInfo c;
    c.run.run_id = run;
    c.run.parent_run_id = parent;
    c.tool = "files.write";
    c.risk = risk;
    return c;
}

}  // namespace

TEST(AgendaIntake, AMeetingBecomesASpeakerLabelledItemLinkedToItsThread) {
    const json turns = json::array({{{"speaker", "Petr Svoboda"}, {"text", "Jana, please send the estimate."}},
                                    {{"speaker", "Jana Novakova"}, {"text", "Will do."}}});
    const auto s = agenda_meeting_item(meeting_record(), turns);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->kind, "meeting");
    EXPECT_EQ(s->id, "mt-1");
    EXPECT_EQ(s->conversation, "series-7");
    EXPECT_EQ(s->text, "Petr Svoboda: Jana, please send the estimate.\nJana Novakova: Will do.\n");
    EXPECT_FALSE(s->user_addressed);  // a meeting addresses nobody: admission needs the user's name
    EXPECT_FALSE(s->author_is_user);
    EXPECT_EQ(s->recipients, 3);
    EXPECT_EQ(s->at_ms, 5'000);
    EXPECT_EQ(s->link, "#/threads/th-1");

    auto single = meeting_record();
    single.series_id.clear();
    single.thread_id.clear();
    single.ended_at = 0;
    const auto t = agenda_meeting_item(single, turns);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->conversation, "ev-1");
    EXPECT_EQ(t->at_ms, 1'000);
    EXPECT_TRUE(t->link.empty());
}

TEST(AgendaIntake, AMeetingWithoutTranscriptTextIsNotOffered) {
    EXPECT_FALSE(agenda_meeting_item(meeting_record(), json::array()));
}

TEST(AgendaIntake, ALongTranscriptKeepsItsStartAndEndOnCharacterBoundaries) {
    std::string text = "START ";
    while (text.size() < 3 * agenda::kMaxItemText) text += "Jana řekla, že pošle odhad nákladů. ";
    text += " END";
    const std::string b = agenda_bounded_transcript(text);
    EXPECT_LE(b.size(), agenda::kMaxItemText);
    EXPECT_EQ(b.rfind("START ", 0), 0u);
    EXPECT_EQ(b.substr(b.size() - 4), " END");
    EXPECT_NE(b.find("\n[...]\n"), std::string::npos);
    const size_t tail = b.find("\n[...]\n") + 7;
    EXPECT_NE(static_cast<unsigned char>(b[tail]) & 0xC0, 0x80u);  // the end part does not start inside a character
    EXPECT_EQ(agenda_bounded_transcript("short"), "short");
}

TEST(AgendaIntake, ADirectChatAddressesTheUserAndATeamChatDoesNot) {
    const json payload = {{"chat", "c-1"},
                          {"message", {{"id", "msg-1"}, {"text", "Can you send me the slides?"}, {"sender", "petr"},
                                       {"sender_name", "Petr Svoboda"}, {"at", "2026-11-17T09:30:00Z"}}}};
    const json direct = {{"kind", "direct"}, {"members", json::array({"petr", "jana"})}};
    const auto d = agenda_chat_item(payload, direct, "jana", 42);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->kind, "chat");
    EXPECT_EQ(d->id, "msg-1");
    EXPECT_EQ(d->conversation, "c-1");
    EXPECT_EQ(d->title, "Petr Svoboda");  // a direct Chat without a name is titled by its peer
    EXPECT_TRUE(d->user_addressed);
    EXPECT_FALSE(d->author_is_user);
    EXPECT_EQ(d->recipients, 2);
    EXPECT_EQ(d->at_ms, *parse_iso8601_ms("2026-11-17T09:30:00Z"));
    EXPECT_EQ(d->link, "#/chats/c-1");

    const json team = {{"kind", "team"}, {"name", "Falcon"}, {"members", json::array({"petr", "jana", "martin", "eva"})}};
    const auto t = agenda_chat_item(payload, team, "jana", 42);
    ASSERT_TRUE(t);
    EXPECT_FALSE(t->user_addressed);
    EXPECT_EQ(t->title, "Falcon");
    EXPECT_EQ(t->recipients, 4);
}

TEST(AgendaIntake, TheUsersOwnAndEmptyChatMessagesAreHandled) {
    json payload = {{"message", {{"id", "msg-2"}, {"chat", "c-2"}, {"text", "I'll send it Friday."}, {"sender", "JANA"}}}};
    const auto own = agenda_chat_item(payload, json(), "jana", 42);
    ASSERT_TRUE(own);
    EXPECT_TRUE(own->author_is_user);
    EXPECT_EQ(own->conversation, "c-2");  // the Chat from the message when the push names none
    EXPECT_EQ(own->title, "Chat");
    EXPECT_EQ(own->at_ms, 42);  // no timestamp: the time it arrived
    payload["message"]["text"] = "   ";
    EXPECT_FALSE(agenda_chat_item(payload, json(), "jana", 42));
    EXPECT_FALSE(agenda_chat_item(json{{"chat", "c-2"}}, json(), "jana", 42));
}

TEST(AgendaIntake, PreparationRunsAndTheirChildRunsMayOnlyRead) {
    const auto prep = [](const std::string& run) { return run == "prep-1"; };
    using K = runtime::ToolDecision::Kind;
    EXPECT_EQ(agenda_preparation_decision(call("prep-1", "", runtime::Risk::read), prep).kind, K::allow);
    for (const auto risk : {runtime::Risk::write_local, runtime::Risk::external, runtime::Risk::destructive}) {
        const auto d = agenda_preparation_decision(call("prep-1", "", risk), prep);
        EXPECT_EQ(d.kind, K::deny);
        EXPECT_NE(d.reason.find("only reads"), std::string::npos);
        EXPECT_EQ(agenda_preparation_decision(call("child-1", "prep-1", risk), prep).kind, K::deny);
        EXPECT_EQ(agenda_preparation_decision(call("other", "", risk), prep).kind, K::allow);
    }
}

// ---- add-local-outlook-and-teams (tasks 4.2, 4.3) ----

namespace {

teams::StoredTeamsMessage teams_msg(const std::string& id, const std::string& who, const std::string& text, long long at, bool mine = false) {
    teams::StoredTeamsMessage m;
    m.conversation_id = "19:grp@thread.v2";
    m.id = id;
    m.sender_name = who;
    m.sender_email = mine ? "jana@example.cz" : "petr@example.cz";
    m.text = text;
    m.at_ms = at;
    m.mine = mine;
    return m;
}

}  // namespace

TEST(AgendaIntake, ATeamsMessageCarriesItsConversationAsContext) {
    teams::StoredConversation c{"19:grp@thread.v2", "group", "Falcon", 0};
    const auto m = teams_msg("m3", "Petr Svoboda", "Jana, can you send the test report by Thursday?", 3'000);
    const std::vector<teams::StoredTeamsMessage> before = {teams_msg("m1", "Petr Svoboda", "The tests finished.", 1'000),
                                                           teams_msg("m2", "Jana Novakova", "Great.", 2'000, true), m};
    const auto s = agenda_teams_item(m, c, "Falcon", before, true);
    EXPECT_EQ(s.kind, "teams");
    EXPECT_EQ(s.id, "19:grp@thread.v2|m3");
    EXPECT_EQ(s.conversation, "19:grp@thread.v2");
    EXPECT_EQ(s.title, "Falcon");
    EXPECT_EQ(s.text, "Earlier in this conversation:\nPetr Svoboda: The tests finished.\nJana Novakova: Great.\n\nNew message:\n"
                      "Petr Svoboda: Jana, can you send the test report by Thursday?");
    EXPECT_TRUE(s.user_addressed);
    EXPECT_FALSE(s.author_is_user);
    EXPECT_EQ(s.recipients, 8);
    EXPECT_EQ(s.at_ms, 3'000);
    EXPECT_EQ(s.link, "teams:19:grp@thread.v2");

    c.type = "chat";
    const auto own = agenda_teams_item(teams_msg("m4", "", "I'll do it", 4'000, true), c, "Petr", {}, false);
    EXPECT_TRUE(own.author_is_user);
    EXPECT_EQ(own.recipients, 1);
    EXPECT_EQ(own.text, "Someone: I'll do it");
    EXPECT_EQ(own.author, "jana@example.cz");  // no display name: the address
}

TEST(AgendaIntake, AReplyToAMailSourceIsPrefilledAsAReply) {
    const json item = {{"id", "a1"}, {"title", "Send the Q4 slides"},
                       {"sources", json::array({{{"kind", "meeting"}, {"id", "mt"}}, {{"kind", "mail"}, {"id", "M1"}}})}};
    mail::StoredMessage m;
    m.id = "M1";
    m.subject = "Q4 slides";
    m.from = {"Petr Svoboda", "petr@example.cz"};
    const auto mail = [&](const std::string& id) -> std::optional<mail::StoredMessage> {
        if (id == "M1") return m;
        return std::nullopt;
    };
    json p = agenda_reply_prefill(item, "Hi Petr, attached.", mail, {});
    EXPECT_EQ(p["to"], json::array({"petr@example.cz"}));
    EXPECT_EQ(jstr(p, "subject"), "RE: Q4 slides");
    EXPECT_EQ(jstr(p, "reply_to"), "M1");
    EXPECT_EQ(jstr(p, "source"), "mail");
    EXPECT_EQ(jstr(p, "body"), "Hi Petr, attached.");
    m.subject = "Re: Q4 slides";
    EXPECT_EQ(jstr(agenda_reply_prefill(item, "", mail, {}), "subject"), "Re: Q4 slides");  // no second prefix
    m.withheld = true;
    p = agenda_reply_prefill(item, "x", mail, {});
    EXPECT_TRUE(p["to"].empty());  // a withheld source gives no recipient
    EXPECT_EQ(jstr(p, "subject"), "Send the Q4 slides");
    EXPECT_EQ(jstr(p, "source"), "");
}

TEST(AgendaIntake, AMailReplyCopiesTheOtherRecipientsButNeverTheUser) {
    const json item = {{"id", "a3"}, {"title", "Answer"}, {"sources", json::array({{{"kind", "mail"}, {"id", "M2"}}})}};
    mail::StoredMessage m;
    m.id = "M2";
    m.subject = "Plan";
    m.from = {"Petr", "petr@example.cz"};
    m.to = {{"Me", "Me@Example.cz"}, {"Eva", "eva@example.cz"}};
    m.cc = {{"Jan", "jan@example.cz"}, {"Petr again", "PETR@example.cz"}};
    const auto mail = [&](const std::string&) -> std::optional<mail::StoredMessage> { return m; };
    json p = agenda_reply_prefill(item, "x", mail, {}, "me@example.cz");
    EXPECT_EQ(p["to"], json::array({"petr@example.cz"}));
    EXPECT_EQ(p["cc"], json::array({"eva@example.cz", "jan@example.cz"}));  // reply-all, without the user or duplicates
    // The user's own mail: the reply goes to whom it was sent, not back to the user.
    m.from = {"Me", "me@example.cz"};
    m.to = {{"Eva", "eva@example.cz"}};
    m.cc = {};
    p = agenda_reply_prefill(item, "x", mail, {}, "me@example.cz");
    EXPECT_EQ(p["to"], json::array({"eva@example.cz"}));
    EXPECT_TRUE(p["cc"].empty());
}

TEST(AgendaIntake, AReplyToTheUsersOwnTeamsMessageHasNoRecipient) {
    const json item = {{"id", "a4"}, {"title", "Send specs"}, {"sources", json::array({{{"kind", "teams"}, {"id", "19:c|m4"}}})}};
    const auto teams = [](const std::string&) -> std::optional<TeamsReplySource> {
        return TeamsReplySource{teams_msg("m4", "", "I'll send the specs", 1, true), "Petr"};
    };
    const json p = agenda_reply_prefill(item, "Specs attached.", {}, teams);
    EXPECT_TRUE(p["to"].empty());
    EXPECT_EQ(jstr(p, "source"), "teams");
}

TEST(AgendaIntake, AReplyToATeamsSourceGoesToTheSendersAddress) {
    const json item = {{"id", "a2"}, {"title", "Send the test report"}, {"sources", json::array({{{"kind", "teams"}, {"id", "19:grp@thread.v2|m3"}}})}};
    const auto teams = [](const std::string& id) -> std::optional<TeamsReplySource> {
        if (id != "19:grp@thread.v2|m3") return std::nullopt;
        return TeamsReplySource{teams_msg("m3", "Petr Svoboda", "...", 1), "Falcon"};
    };
    const json p = agenda_reply_prefill(item, "Report attached.", {}, teams);
    EXPECT_EQ(p["to"], json::array({"petr@example.cz"}));
    EXPECT_EQ(jstr(p, "subject"), "RE: Falcon");
    EXPECT_EQ(jstr(p, "reply_to"), "");  // a new mail, not a mail reply
    EXPECT_EQ(jstr(p, "source"), "teams");
}
