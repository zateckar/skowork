// client/teams-chats (add-local-outlook-and-teams tasks 3.3, 3.4): the local Teams copy, its sync, events, exclusions,
// window and tools, with the cache read replaced by a scripted Extraction (the decoders are covered by teams_tests).
#include "support/graph_mock.h"

#include "teams/teams_service.h"

#include <algorithm>

using namespace wb;
using namespace wb::teams;

namespace {

constexpr long long kHour = 3'600'000, kDay = 86'400'000;
const std::string kMe = "8:orgid:me", kPetra = "8:orgid:petra";

struct TeamsFixture {
    TeamsFixture() {
        now = unix_millis_now();
        TeamsEnv e;
        e.db = &db.store->db();
        e.clock = &system_clock();
        e.policy = [this](const std::string& k) -> std::optional<json> {
            if (policy.contains(k)) return policy[k];
            return std::nullopt;
        };
        e.background = false;
        e.account = [] { return std::string("jan@example.com"); };
        e.read = [this](const std::filesystem::path&) -> Result<Extraction> {
            ++reads;
            return cache;
        };
        svc = std::make_unique<TeamsService>(std::move(e));
        svc->on_event([this](const TeamsEvent& ev) { events.push_back(ev); });
        cache.user_mri = kMe;
        cache.profiles = {{kMe, "Jan Novák", "jan@example.com"}, {kPetra, "Petra Malá", "petra@example.com"}};
        cache.conversations = {{"19:a_b@unq.gbl.spaces", "chat", "", now}, {"19:grp@thread.v2", "group", "Project Falcon", now}};
    }
    TeamsMessage& say(const std::string& conv, const std::string& id, const std::string& who, const std::string& text, long long at) {
        TeamsMessage m;
        m.id = id;
        m.conversation_id = conv;
        m.sender_mri = who;
        m.sender_name = who == kMe ? "Jan Novák" : "Petra Malá";
        m.text = text;
        m.at_ms = at;
        m.version = "1";
        cache.messages.push_back(m);
        return cache.messages.back();
    }
    std::vector<TeamsEvent> take() { return std::exchange(events, {}); }

    long long now = 0;
    MailDb db;
    json policy = json::object();
    Extraction cache;
    int reads = 0;
    std::unique_ptr<TeamsService> svc;
    std::vector<TeamsEvent> events;
};

int count(const std::vector<TeamsEvent>& v, const std::string& kind) {
    return static_cast<int>(std::count_if(v.begin(), v.end(), [&](const TeamsEvent& e) { return e.kind == kind; }));
}

}  // namespace

TEST(TeamsService, OptInAndPolicy) {
    TeamsFixture fx;
    EXPECT_FALSE(fx.svc->enabled());
    EXPECT_FALSE(fx.svc->sync_now());
    EXPECT_EQ(fx.reads, 0);  // nothing is read before the user turns it on
    fx.policy["teams.mode"] = "off";
    EXPECT_FALSE(fx.svc->configure({{"enabled", true}}));
    fx.policy.erase("teams.mode");
    ASSERT_TRUE(fx.svc->configure({{"enabled", true}}));
    EXPECT_TRUE(fx.svc->enabled());
}

TEST(TeamsService, BackfillThenNewEditedDeletedAndMentions) {
    TeamsFixture fx;
    fx.say("19:a_b@unq.gbl.spaces", "1", kPetra, "Hi Jan", fx.now - 2 * kHour);
    fx.say("19:grp@thread.v2", "2", kPetra, "Supplier B is signed", fx.now - kHour);
    ASSERT_TRUE(fx.svc->configure({{"enabled", true}}));
    auto r = fx.svc->sync_now();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->messages, 2);
    EXPECT_TRUE(fx.take().empty());  // the first read is a backfill

    fx.say("19:grp@thread.v2", "3", kPetra, "@Jan can you send the test report by Thursday?", fx.now).mention_mris = {kMe};
    fx.say("19:grp@thread.v2", "4", kPetra, "lunch?", fx.now);
    fx.say("19:grp@thread.v2", "5", kMe, "Sure", fx.now);
    fx.cache.messages[0].version = "2";  // edited
    fx.cache.messages[1].deleted = true;
    ASSERT_TRUE(fx.svc->sync_now());
    const auto ev = fx.take();
    EXPECT_EQ(count(ev, "teams.message"), 3);
    EXPECT_EQ(count(ev, "teams.updated"), 1);
    EXPECT_EQ(count(ev, "teams.deleted"), 1);
    const auto mention = std::find_if(ev.begin(), ev.end(), [](const TeamsEvent& e) { return e.message_id == "3"; });
    ASSERT_NE(mention, ev.end());
    EXPECT_TRUE(mention->addressed);
    const auto lunch = std::find_if(ev.begin(), ev.end(), [](const TeamsEvent& e) { return e.message_id == "4"; });
    EXPECT_FALSE(lunch->addressed);
    const auto own = std::find_if(ev.begin(), ev.end(), [](const TeamsEvent& e) { return e.message_id == "5"; });
    EXPECT_TRUE(own->mine);
    const auto edited = std::find_if(ev.begin(), ev.end(), [](const TeamsEvent& e) { return e.kind == "teams.updated"; });
    EXPECT_TRUE(edited->addressed);  // a one-to-one chat
    EXPECT_FALSE(fx.svc->message("19:grp@thread.v2", "2"));

    // Read again with nothing new: no events.
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_TRUE(fx.take().empty());
    const auto stored = fx.svc->message("19:grp@thread.v2", "3");
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->mentions_me);
    EXPECT_EQ(stored->sender_email, "petra@example.com");
}

TEST(TeamsService, ExclusionsAndWindow) {
    TeamsFixture fx;
    fx.policy["teams.sync_days"] = 7;
    fx.say("19:grp@thread.v2", "old", kPetra, "ancient", fx.now - 10 * kDay);
    fx.say("19:grp@thread.v2", "g1", kPetra, "group text", fx.now - kHour);
    fx.say("19:a_b@unq.gbl.spaces", "c1", kPetra, "private chat", fx.now - kHour);
    ASSERT_TRUE(fx.svc->configure({{"enabled", true}}));
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_FALSE(fx.svc->message("19:grp@thread.v2", "old"));
    ASSERT_TRUE(fx.svc->exclude("19:a_b@unq.gbl.spaces", true));
    EXPECT_FALSE(fx.svc->message("19:a_b@unq.gbl.spaces", "c1"));
    EXPECT_EQ(count(fx.take(), "teams.deleted"), 1);
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_FALSE(fx.svc->message("19:a_b@unq.gbl.spaces", "c1"));  // stays excluded
    EXPECT_TRUE(fx.svc->search("private", "", "", 0, 0, 10).empty());

    fx.policy["teams.excluded_types"] = json::array({"group"});
    fx.say("19:grp@thread.v2", "g2", kPetra, "more group text", fx.now);
    ASSERT_TRUE(fx.svc->sync_now());
    EXPECT_FALSE(fx.svc->message("19:grp@thread.v2", "g2"));
}

TEST(TeamsService, UnrecognisedCacheLeavesTheCopy) {
    TeamsFixture fx;
    fx.say("19:grp@thread.v2", "g1", kPetra, "kept", fx.now - kHour);
    ASSERT_TRUE(fx.svc->configure({{"enabled", true}}));
    ASSERT_TRUE(fx.svc->sync_now());
    fx.cache = Extraction{};
    fx.cache.decode_errors = 12;
    auto r = fx.svc->sync_now();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "unrecognised");
    EXPECT_TRUE(fx.svc->message("19:grp@thread.v2", "g1"));
    EXPECT_FALSE(jstr(fx.svc->status(), "error").empty());
}

TEST(TeamsService, ToolsSearchAndRead) {
    TeamsFixture fx;
    fx.say("19:grp@thread.v2", "g1", kPetra, "The cost estimate is due Friday", fx.now - kHour);
    fx.say("19:grp@thread.v2", "g2", kMe, "Noted", fx.now - kHour + 60'000);
    ASSERT_TRUE(fx.svc->configure({{"enabled", true}}));
    ASSERT_TRUE(fx.svc->sync_now());
    std::map<std::string, runtime::ToolDef> tools;
    for (auto& t : fx.svc->tools()) tools[t.id] = std::move(t);
    ASSERT_TRUE(tools.count("teams.search") && tools.count("teams.read"));
    EXPECT_EQ(tools["teams.search"].risk, runtime::Risk::read);
    runtime::ToolContext ctx;
    auto out = tools["teams.search"].run(ctx, {{"query", "cost estim"}});
    ASSERT_TRUE(out.ok) << out.content;
    auto j = parse_json(out.content);
    ASSERT_EQ((*j)["results"].size(), 1u) << out.content;
    EXPECT_EQ(jstr((*j)["results"][0], "conversation"), "Project Falcon");
    ASSERT_TRUE(out.provenance);
    EXPECT_EQ(out.provenance->source_kind, "teams");
    out = tools["teams.read"].run(ctx, {{"conversation_id", "19:grp@thread.v2"}, {"message_id", "g1"}});
    ASSERT_TRUE(out.ok) << out.content;
    EXPECT_EQ((*parse_json(out.content))["messages"].size(), 2u);
    const std::string day = fx.svc->day_text("19:grp@thread.v2", fx.now - kDay, fx.now + kDay);
    EXPECT_NE(day.find("Petra Malá: The cost estimate is due Friday"), std::string::npos);
}

TEST(TeamsService, ForgetDeletesTheCopy) {
    TeamsFixture fx;
    fx.say("19:grp@thread.v2", "g1", kPetra, "x", fx.now);
    ASSERT_TRUE(fx.svc->configure({{"enabled", true}}));
    ASSERT_TRUE(fx.svc->sync_now());
    ASSERT_TRUE(fx.svc->forget());
    EXPECT_FALSE(fx.svc->enabled());
    EXPECT_EQ(jint(fx.svc->status(), "messages", -1), 0);
}
