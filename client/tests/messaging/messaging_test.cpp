// client/user-messaging (add-user-messaging 2.1, 2.3): the messaging service against an in-process fake of
// control-plane/user-messaging (catch-up, pushes, unread counts, the outbox, own Presence, policy off), and the
// messages.* tools in a Run under the real policy handler (untrusted provenance, send waits for approval).
#include "base/strings.h"
#include "inbox/inbox_store.h"
#include "messaging/messaging.h"
#include "support/runtime_harness.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::messaging;

namespace {

// control-plane/user-messaging as design.md "API" describes it, for the user alice.
struct FakeServer {
    struct Msg {
        std::string id;
        long long seq;
        std::string sender, text, at;
    };
    struct Chat {
        std::string id, kind, name;
        std::vector<std::string> members;
        std::vector<Msg> msgs;
        std::map<std::string, long long> read;
    };
    std::mutex mx;
    bool online = true;
    std::map<std::string, Chat> chats;
    std::map<std::string, std::string> names = {{"alice", "Alice Tester"}, {"bob", "Bob Tester"}, {"carol", "Carol Tester"}};
    std::map<std::string, std::string> presence = {{"alice", "online"}, {"bob", "online"}, {"carol", "away"}};
    std::vector<std::tuple<std::string, std::string, json, bool>> calls;  // method, path, body, installation header
    int n = 0;

    json message(const Chat& c, const Msg& m) {
        return {{"id", m.id}, {"chat", c.id}, {"seq", m.seq}, {"sender", m.sender}, {"sender_name", names[m.sender]}, {"text", m.text}, {"at", m.at}};
    }
    json view(const Chat& c) {
        json members = json::array();
        for (const auto& u : c.members) members.push_back({{"user_name", u}, {"display_name", names[u]}, {"presence", presence[u]}});
        const long long last = c.msgs.empty() ? 0 : c.msgs.back().seq;
        const long long read = c.read.count("alice") ? c.read.at("alice") : 0;
        return {{"id", c.id}, {"kind", c.kind}, {"name", c.kind == "team" ? json(c.name) : json(nullptr)}, {"members", members},
                {"last_seq", last}, {"read_seq", read}, {"unread", 0}, {"last_message", c.msgs.empty() ? json(nullptr) : message(c, c.msgs.back())},
                {"created_at", "2026-11-01T08:00:00.000Z"}};
    }
    // Adds a message as the server would store it; returns its push payload.
    json post(const std::string& chat, const std::string& sender, const std::string& text, std::string id = {}) {
        const std::lock_guard l(mx);
        Chat& c = chats[chat];
        if (id.empty()) id = "srv" + std::to_string(++n) + "xxxxxx";
        const long long seq = c.msgs.empty() ? 1 : c.msgs.back().seq + 1;
        c.msgs.push_back({id, seq, sender, text, "2026-11-01T09:00:" + std::to_string(10 + seq % 50) + ".000Z"});
        if (sender == "alice") c.read["alice"] = seq;
        return {{"chat", chat}, {"message", message(c, c.msgs.back())}};
    }
    int count(const std::string& method, const std::string& prefix) {
        const std::lock_guard l(mx);
        int k = 0;
        for (const auto& [m, p, b, i] : calls)
            if (m == method && starts_with(p, prefix)) ++k;
        return k;
    }

    Result<json> api(const std::string& method, const std::string& path, const json& body, bool installation) {
        const std::lock_guard l(mx);
        if (!online) return make_error("offline", "The control plane is not connected.");
        calls.emplace_back(method, path, body, installation);
        const auto parts = split(path.substr(0, path.find('?')), '/');
        const std::string query = path.find('?') == std::string::npos ? "" : path.substr(path.find('?') + 1);
        if (path == "chats" && method == "GET") {
            json list = json::array();
            for (auto& [id, c] : chats)
                if (std::find(c.members.begin(), c.members.end(), "alice") != c.members.end()) list.push_back(view(c));
            return json{{"chats", list}};
        }
        if (path == "chats" && method == "POST") {
            Chat c;
            c.id = "chat-" + std::to_string(++n);
            c.kind = jstr(body, "kind");
            c.members = {"alice"};
            if (c.kind == "direct") {
                for (auto& [id, x] : chats)
                    if (x.kind == "direct" && std::find(x.members.begin(), x.members.end(), jstr(body, "user")) != x.members.end()) return view(x);
                if (!names.count(jstr(body, "user"))) return make_error("USER_UNAVAILABLE", "cannot be messaged");
                c.members.push_back(jstr(body, "user"));
            } else {
                c.name = jstr(body, "name");
                for (const auto& u : body["members"]) c.members.push_back(u.get<std::string>());
            }
            chats[c.id] = c;
            return view(chats[c.id]);
        }
        if (starts_with(path, "directory")) {
            json users = json::array();
            const std::string q = query.size() > 2 ? query.substr(2) : "";
            for (const auto& [u, d] : names)
                if (u != "alice" && starts_with(u, q)) users.push_back({{"user_name", u}, {"display_name", d}, {"presence", presence[u]}});
            return json{{"users", users}};
        }
        if (path == "presence") {
            if (body.contains("state")) presence["alice"] = jstr(body, "state") == "idle" ? "away" : "online";
            if (body.contains("busy")) presence["alice"] = jbool(body, "busy") ? "busy" : "online";
            return json{{"presence", presence["alice"]}, {"busy", jbool(body, "busy")}};
        }
        if (parts.size() >= 2 && parts[0] == "chats") {
            if (!chats.count(parts[1])) return make_error("NOT_FOUND", "No such Chat.");
            Chat& c = chats[parts[1]];
            if (parts.size() == 3 && parts[2] == "messages" && method == "GET") {
                long long after = 0;
                if (const auto a = query.find("after="); a != std::string::npos) after = std::atoll(query.c_str() + a + 6);
                json list = json::array();
                for (const auto& m : c.msgs)
                    if (m.seq > after) list.push_back(message(c, m));
                return json{{"messages", list}};
            }
            if (parts.size() == 3 && parts[2] == "messages" && method == "POST") {
                for (const auto& m : c.msgs)
                    if (m.id == jstr(body, "id")) return message(c, m);  // idempotent
                if (jstr(body, "text").size() > 8192) return make_error("TOO_LARGE", "The message is longer than 8 KB.");
                const long long seq = c.msgs.empty() ? 1 : c.msgs.back().seq + 1;
                c.msgs.push_back({jstr(body, "id"), seq, "alice", jstr(body, "text"), "2026-11-01T10:00:00.000Z"});
                c.read["alice"] = seq;
                return message(c, c.msgs.back());
            }
            if (parts.size() == 3 && parts[2] == "read") {
                c.read["alice"] = jint(body, "seq");
                return json{{"read_seq", c.read["alice"]}};
            }
            if (parts.size() == 2 && method == "PATCH") {
                c.name = jstr(body, "name");
                return view(c);
            }
            if (parts.size() == 3 && parts[2] == "members" && method == "POST") {
                for (const auto& u : body["users"]) c.members.push_back(u.get<std::string>());
                return view(c);
            }
            if (parts.size() == 4 && parts[2] == "members" && method == "DELETE") {
                std::erase(c.members, "alice");
                return json{{"left", c.id}};
            }
        }
        return make_error("http_404", "unknown " + method + " " + path);
    }
};

struct Rig {
    TempRoot tmp;
    std::unique_ptr<Database> db;
    FakeServer server;
    std::atomic<bool> enabled{true};
    std::mutex mx;
    std::vector<json> events;
    std::unique_ptr<MessagingService> svc;

    Rig() {
        auto root = DataRoot::open(tmp.path);
        EXPECT_TRUE(root);
        KeyStore keys(root->keys());
        EXPECT_TRUE(keys.load());
        auto k = keys.get_or_create("app.db");
        MigrationOutcome o;
        auto opened = open_and_migrate(root->file("app.db"), *k, app_migrations(), o);
        EXPECT_TRUE(opened);
        db = std::move(*opened);
        make();
    }
    void make() {
        MessagingEnv e;
        e.api = [this](const std::string& m, const std::string& p, const json& b, bool i) { return server.api(m, p, b, i); };
        e.connected = [this] {
            const std::lock_guard l(server.mx);
            return server.online;
        };
        e.enabled = [this] { return enabled.load(); };
        e.me = [] { return std::string("alice"); };
        e.changed = [this](const json& ev) {
            const std::lock_guard l(mx);
            events.push_back(ev);
        };
        svc = std::make_unique<MessagingService>(*db, std::move(e));
    }
    void set_online(bool on) {
        const std::lock_guard l(server.mx);
        server.online = on;
    }
    long long last_unread() {
        const std::lock_guard l(mx);
        return events.empty() ? -1 : jint(events.back(), "unread");
    }
    json find(const std::string& id) {
        for (const auto& c : svc->chats())
            if (jstr(c, "id") == id) return c;
        return nullptr;
    }
};

void seed_bob(Rig& r) {
    FakeServer::Chat c;
    c.id = "chat-bob";
    c.kind = "direct";
    c.members = {"alice", "bob"};
    r.server.chats[c.id] = c;
    r.server.post("chat-bob", "bob", "Hello **Alice**");
    r.server.post("chat-bob", "bob", "Are you there?");
}

}  // namespace

// Catch-up after the connection comes up, push handling (duplicates, gaps), unread counts and the read position.
TEST(Messaging, CatchUpPushesUnreadAndReadPosition) {
    Rig r;
    seed_bob(r);
    r.svc->connection_changed(true);
    json c = r.find("chat-bob");
    ASSERT_TRUE(c.is_object()) << r.svc->chats().dump();
    EXPECT_EQ(jint(c, "unread"), 2);
    EXPECT_EQ(jstr(c["peer"], "user_name"), "bob");
    EXPECT_EQ(jstr(c["peer"], "presence"), "online");
    EXPECT_EQ(jstr(c["last_message"], "text"), "Are you there?");
    EXPECT_EQ(r.svc->unread_total(), 2);
    EXPECT_EQ(r.last_unread(), 2);

    // A pushed message counts; the same push twice is stored once.
    const json p3 = r.server.post("chat-bob", "bob", "Third");
    r.svc->on_push("chat-message", p3);
    r.svc->on_push("chat-message", p3);
    EXPECT_EQ(r.svc->messages("chat-bob").size(), 3u);
    EXPECT_EQ(jint(r.find("chat-bob"), "unread"), 3);
    EXPECT_EQ(r.last_unread(), 3);

    // A missed push: the next one shows the gap and the missing message is fetched.
    r.server.post("chat-bob", "bob", "Fourth (push lost)");
    r.svc->on_push("chat-message", r.server.post("chat-bob", "bob", "Fifth"));
    const json msgs = r.svc->messages("chat-bob");
    ASSERT_EQ(msgs.size(), 5u) << msgs.dump();
    EXPECT_EQ(jstr(msgs[3], "text"), "Fourth (push lost)");
    EXPECT_EQ(jint(r.find("chat-bob"), "unread"), 5);

    // Opening the Chat clears it locally and on the server.
    ASSERT_TRUE(r.svc->open("chat-bob"));
    EXPECT_EQ(jint(r.find("chat-bob"), "unread"), 0);
    EXPECT_EQ(r.server.chats["chat-bob"].read["alice"], 5);
    EXPECT_EQ(r.last_unread(), 0);

    // Presence of the other user.
    r.svc->on_push("presence-changed", {{"user_name", "bob"}, {"presence", "away"}});
    EXPECT_EQ(jstr(r.find("chat-bob")["peer"], "presence"), "away");

    // A Chat this installation has not seen: the push brings the list.
    FakeServer::Chat team;
    team.id = "chat-team";
    team.kind = "team";
    team.name = "Brakes";
    team.members = {"alice", "bob", "carol"};
    r.server.chats[team.id] = team;
    r.svc->on_push("chat-message", r.server.post("chat-team", "carol", "Welcome"));
    EXPECT_EQ(jint(r.find("chat-team"), "unread"), 1);
    EXPECT_EQ(r.svc->unread_total(), 1);

    // The cache survives a restart (the encrypted app.db), offline.
    r.set_online(false);
    r.make();
    EXPECT_EQ(r.svc->messages("chat-bob").size(), 5u);
    EXPECT_EQ(r.svc->unread_total(), 1);
}

// client/work-inbox Chat Entries and Chat Entry Triage (add-chats-in-inbox 1.1): the messaging service keeps chat_inbox
// current, and the inbox lists Chats with the Threads in Unread, All, folders and the Archive, never in Action or
// Running; a colleague's message un-archives; folder deletion; one order and paging across both kinds; policy off.
TEST(MessagingInbox, ChatsAreInboxEntriesWithTriage) {
    Rig r;
    seed_bob(r);  // two unread messages from bob, the newest at 09:00:12
    FakeServer::Chat team;
    team.id = "chat-team";
    team.kind = "team";
    team.name = "Falcon team";
    team.members = {"alice", "bob", "carol"};
    r.server.chats[team.id] = team;
    r.server.post("chat-team", "carol", "Kick-off\nat nine");  // 09:00:11
    {
        auto st = r.db->prepare("INSERT INTO threads(id,title,created_at,updated_at,origin_kind,origin_ref,last_activity,unread) "
                                "VALUES('thr_a','Budget review','2026-11-01T08:00:00.000Z','2026-11-01T08:00:00.000Z','user','null',"
                                "'2026-11-01T09:00:11.500Z',1)");
        ASSERT_TRUE(st.run());
    }
    r.svc->connection_changed(true);
    inbox::InboxStore store(*r.db, system_clock());
    const auto ids = [&](const std::string& view) {
        std::vector<std::string> out;
        auto page = store.list(view);
        EXPECT_TRUE(page) << view;
        if (page)
            for (const auto& e : page->entries) out.push_back(e.id);
        return out;
    };
    using V = std::vector<std::string>;

    // Unread and All: both kinds in one order by last activity; Action and Running hold no Chats.
    EXPECT_EQ(ids("unread"), (V{"chat:chat-bob", "thr_a", "chat:chat-team"}));
    EXPECT_EQ(ids("all"), (V{"chat:chat-bob", "thr_a", "chat:chat-team"}));
    EXPECT_EQ(ids("chats"), (V{"chat:chat-bob", "chat:chat-team"}));
    EXPECT_TRUE(ids("action").empty());
    EXPECT_TRUE(ids("running").empty());
    EXPECT_EQ(store.counts().unread, 3);
    const auto bob = store.entry("chat:chat-bob");
    ASSERT_TRUE(bob);
    EXPECT_EQ(bob->kind, "chat");
    EXPECT_EQ(bob->chat_kind, "direct");
    EXPECT_EQ(bob->title, "Bob Tester");
    EXPECT_EQ(bob->presence, "online");
    EXPECT_EQ(bob->preview, "Are you there?");
    EXPECT_EQ(bob->unread_count, 2);
    EXPECT_TRUE(bob->unread);
    const auto falcon = store.entry("chat:chat-team");
    ASSERT_TRUE(falcon);
    EXPECT_EQ(falcon->title, "Falcon team");
    EXPECT_EQ(falcon->chat_kind, "team");

    // Paging one entry at a time keeps the order across both kinds.
    V paged;
    std::string cursor;
    for (int i = 0; i < 5; ++i) {
        auto page = store.list("all", cursor, 1);
        ASSERT_TRUE(page);
        for (const auto& e : page->entries) paged.push_back(e.id);
        cursor = page->next_cursor;
        if (cursor.empty()) break;
    }
    EXPECT_EQ(paged, ids("all"));

    // Filed in a folder: listed there, still in Unread, no longer in the Inbox.
    auto folder = store.create_folder("Project Falcon");
    ASSERT_TRUE(folder);
    ASSERT_TRUE(store.move("chat:chat-team", folder->id));
    EXPECT_EQ(ids("folder:" + folder->id), (V{"chat:chat-team"}));
    EXPECT_EQ(ids("inbox"), (V{"chat:chat-bob", "thr_a"}));
    EXPECT_EQ(ids("unread").back(), "chat:chat-team");

    // Archived: in the Archive only; a colleague's message brings it back to its folder and to Unread.
    ASSERT_TRUE(store.set_archived("chat:chat-team", true));
    EXPECT_EQ(ids("archive"), (V{"chat:chat-team"}));
    EXPECT_EQ(ids("all"), (V{"chat:chat-bob", "thr_a"}));
    EXPECT_EQ(store.counts().unread, 2);
    r.svc->on_push("chat-message", r.server.post("chat-team", "carol", "Agenda attached"));
    EXPECT_TRUE(ids("archive").empty());
    EXPECT_EQ(ids("folder:" + folder->id), (V{"chat:chat-team"}));
    EXPECT_EQ(ids("unread").front(), "chat:chat-team");  // the newest activity
    EXPECT_EQ(store.entry("chat:chat-team")->preview, "Agenda attached");
    // My own message does not un-archive.
    ASSERT_TRUE(store.set_archived("chat:chat-team", true));
    ASSERT_TRUE(r.svc->send("chat-team", "Thanks"));
    EXPECT_EQ(ids("archive"), (V{"chat:chat-team"}));
    ASSERT_TRUE(store.set_archived("chat:chat-team", false));

    // Deleting the folder moves the Chat to the Inbox.
    ASSERT_TRUE(store.delete_folder(folder->id));
    const V inbox_after = ids("inbox");
    EXPECT_NE(std::find(inbox_after.begin(), inbox_after.end(), "chat:chat-team"), inbox_after.end());

    // Pinned first; read state is not set by hand; Chats are neither renamed nor deleted here.
    ASSERT_TRUE(store.set_pinned("chat:chat-bob", true));
    EXPECT_EQ(ids("all").front(), "chat:chat-bob");
    EXPECT_TRUE(store.entry("chat:chat-bob")->pinned);
    EXPECT_EQ(store.set_unread("chat:chat-bob", true).error().code, "invalid_params");
    EXPECT_EQ(store.rename("chat:chat-bob", "x").error().code, "invalid_params");
    EXPECT_EQ(store.set_archived("chat:nope", true).error().code, "not_found");

    // Opening the Chat reads it: it leaves Unread.
    ASSERT_TRUE(r.svc->open("chat-bob"));
    EXPECT_EQ(store.entry("chat:chat-bob")->unread_count, 0);
    const V unread_after = ids("unread");
    EXPECT_EQ(std::count(unread_after.begin(), unread_after.end(), "chat:chat-bob"), 0);

    // Leaving removes the entry.
    ASSERT_TRUE(r.svc->leave("chat-team"));
    EXPECT_FALSE(store.entry("chat:chat-team"));
    EXPECT_EQ(ids("chats"), (V{"chat:chat-bob"}));

    // policy.messaging.mode off: no Chat entries, and Unread counts Threads only.
    store.set_chats_listed([] { return false; });
    EXPECT_EQ(ids("all"), (V{"thr_a"}));
    EXPECT_TRUE(ids("chats").empty());
    EXPECT_FALSE(store.entry("chat:chat-bob"));
    EXPECT_EQ(store.counts().unread, 1);
}

// Offline Sending: written offline, shown pending, sent once and in order when the connection is back.
TEST(Messaging, OfflineMessagesAreQueuedAndSentOnceInOrder) {
    Rig r;
    seed_bob(r);
    r.svc->connection_changed(true);
    r.set_online(false);
    r.svc->connection_changed(false);
    auto a = r.svc->send("chat-bob", "First offline");
    auto b = r.svc->send("chat-bob", "Second offline");
    ASSERT_TRUE(a && b);
    EXPECT_TRUE(jbool(*a, "pending"));
    json msgs = r.svc->messages("chat-bob");
    ASSERT_EQ(msgs.size(), 4u);
    EXPECT_TRUE(jbool(msgs[2], "pending"));
    EXPECT_EQ(jstr(msgs[3], "text"), "Second offline");
    EXPECT_TRUE(jbool(r.find("chat-bob")["last_message"], "pending"));
    r.svc->tick(0);  // offline: nothing is called
    EXPECT_EQ(r.server.count("POST", "chats/chat-bob/messages"), 0);
    EXPECT_FALSE(r.svc->send("chat-bob", std::string(8193, 'x')));

    r.set_online(true);
    r.svc->connection_changed(true);
    EXPECT_EQ(r.server.count("POST", "chats/chat-bob/messages"), 2);
    const auto& sent = r.server.chats["chat-bob"].msgs;
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(sent[2].text, "First offline");
    EXPECT_EQ(sent[3].text, "Second offline");
    EXPECT_EQ(sent[2].id, jstr(*a, "id"));
    msgs = r.svc->messages("chat-bob");
    ASSERT_EQ(msgs.size(), 4u);
    for (const auto& m : msgs) EXPECT_FALSE(jbool(m, "pending")) << m.dump();
    // Its own push comes back: still one message.
    r.svc->on_push("chat-message", {{"chat", "chat-bob"}, {"message", r.server.message(r.server.chats["chat-bob"], sent[3])}});
    EXPECT_EQ(r.svc->messages("chat-bob").size(), 4u);
    r.svc->tick(0);
    EXPECT_EQ(r.server.count("POST", "chats/chat-bob/messages"), 2);
    // Replying reads the Chat, as on the control plane (sending moves the sender's read position).
    EXPECT_EQ(jint(r.find("chat-bob"), "unread"), 0);
    // Online, a message is sent at once.
    auto c = r.svc->send("chat-bob", "Online now");
    ASSERT_TRUE(c);
    EXPECT_FALSE(jbool(*c, "pending"));
    EXPECT_EQ(jint(*c, "seq"), 5);
}

// Own Presence: active and idle through PUT /presence with the installation, on a change only; busy until cleared.
TEST(Messaging, PresenceIdleAfterTenMinutesAndBusyUntilCleared) {
    Rig r;
    r.svc->connection_changed(true);
    r.svc->tick(5'000);
    r.svc->tick(9 * 60'000);
    r.svc->tick(kIdleAfterMs + 1'000);
    r.svc->tick(kIdleAfterMs + 31'000);
    r.svc->tick(2'000);
    std::vector<std::string> states;
    for (const auto& [m, p, body, inst] : r.server.calls)
        if (p == "presence" && body.contains("state")) {
            states.push_back(jstr(body, "state"));
            EXPECT_TRUE(inst);
        }
    EXPECT_EQ(states, (std::vector<std::string>{"active", "idle", "active"}));
    EXPECT_EQ(r.server.presence["alice"], "online");
    r.svc->tick(kIdleAfterMs);
    EXPECT_EQ(r.server.presence["alice"], "away");

    ASSERT_TRUE(r.svc->set_busy(true));
    EXPECT_EQ(r.server.presence["alice"], "busy");
    EXPECT_TRUE(r.svc->busy());
    // Busy stays across a restart and is reported again after reconnecting.
    r.make();
    EXPECT_TRUE(r.svc->busy());
    r.server.presence["alice"] = "online";
    r.svc->connection_changed(true);
    EXPECT_EQ(r.server.presence["alice"], "busy");
    ASSERT_TRUE(r.svc->set_busy(false));
    EXPECT_FALSE(r.svc->busy());
}

// policy.messaging.mode off: no calls to the control plane, no tools, and sending says why.
TEST(Messaging, ModeOffCallsNothingAndOffersNoTools) {
    Rig r;
    seed_bob(r);
    r.enabled = false;
    r.svc->connection_changed(true);
    r.svc->tick(0);
    EXPECT_TRUE(r.server.calls.empty());
    EXPECT_EQ(jbool(r.svc->status(), "enabled"), false);
    const auto s = r.svc->send("chat-bob", "hi");
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, "MESSAGING_OFF");
    for (const auto& t : r.svc->tools()) EXPECT_FALSE(runtime::tool_offered(t)) << t.id;
    r.enabled = true;
    for (const auto& t : r.svc->tools()) EXPECT_TRUE(runtime::tool_offered(t)) << t.id;
}

// Directory and Chat management: a direct Chat from the directory, a team Chat with picked members, rename, add, leave.
TEST(Messaging, DirectoryDirectAndTeamChats) {
    Rig r;
    r.svc->connection_changed(true);
    auto found = r.svc->directory("ca");
    ASSERT_TRUE(found);
    ASSERT_EQ((*found)["users"].size(), 1u);
    EXPECT_EQ(jstr((*found)["users"][0], "presence"), "away");
    auto d = r.svc->create_direct("carol");
    ASSERT_TRUE(d) << d.error().message;
    EXPECT_EQ(jstr((*d)["peer"], "user_name"), "carol");
    EXPECT_FALSE(r.svc->create_direct("nobody"));
    auto t = r.svc->create_team("Brakes", {"bob", "carol"});
    ASSERT_TRUE(t);
    EXPECT_EQ((*t)["members"].size(), 3u);
    const std::string team = jstr(*t, "id");
    ASSERT_TRUE(r.svc->rename(team, "Brake team"));
    EXPECT_EQ(jstr(r.find(team), "name"), "Brake team");
    r.server.names["dave"] = "Dave Tester";
    ASSERT_TRUE(r.svc->add_members(team, {"dave"}));
    EXPECT_EQ(r.find(team)["members"].size(), 4u);
    ASSERT_TRUE(r.svc->leave(team));
    EXPECT_TRUE(r.find(team).is_null());
    EXPECT_EQ(r.svc->chats().size(), 1u);
}

// 2.3: the tools' Risk classes, message content as untrusted data with the sender as provenance, and messages.send
// waiting for the user's approval under the default policy (external: approve) before anything is sent.
TEST(MessagingTools, UntrustedProvenanceAndSendWaitsForApproval) {
    Rig r;
    seed_bob(r);
    r.svc->connection_changed(true);
    const auto defs = r.svc->tools();
    std::map<std::string, const runtime::ToolDef*> by;
    for (const auto& d : defs) by[d.id] = &d;
    ASSERT_EQ(by.size(), 4u);
    EXPECT_EQ(by["messages.find_user"]->risk, runtime::Risk::read);
    EXPECT_EQ(by["messages.list_chats"]->risk, runtime::Risk::read);
    EXPECT_EQ(by["messages.read"]->risk, runtime::Risk::read);
    EXPECT_EQ(by["messages.send"]->risk, runtime::Risk::external);

    runtime::ToolContext ctx;
    auto out = by["messages.read"]->run(ctx, {{"user", "bob"}});
    ASSERT_TRUE(out.ok) << out.content;
    ASSERT_TRUE(out.provenance);
    EXPECT_FALSE(out.provenance->trusted);
    EXPECT_EQ(out.provenance->source_kind, "message");
    EXPECT_EQ(out.provenance->source_id, "bob");
    const json body = json::parse(out.content);
    ASSERT_EQ(body["messages"].size(), 2u);
    EXPECT_EQ(jstr(body["messages"][0]["provenance"], "sender"), "bob");
    EXPECT_FALSE(jbool(body["messages"][0]["provenance"], "trusted", true));
    EXPECT_EQ(r.server.chats["chat-bob"].read["alice"], 2);  // reading moves the read position
    out = by["messages.list_chats"]->run(ctx, json::object());
    ASSERT_TRUE(out.ok && out.provenance);
    EXPECT_FALSE(out.provenance->trusted);

    // In a Run under the real policy handler with no tools.risk set: messages.send needs approval.
    Env env;
    policy::PolicyBundle b;
    b.sub = "alice";
    b.ver = 1;
    b.exp = 4102444800;
    auto p = std::make_shared<const policy::EffectivePolicy>(b);
    policy::EnforcerEnv e;
    e.policy = [p] { return p; };
    e.workspace = narrow(env.tmp.path.wstring());
    e.data_root = narrow(env.tmp.path.wstring());
    env.policy_hook = std::make_shared<policy::PolicyEnforcer>(e);
    env.make_runtime();
    for (auto d : r.svc->tools()) env.add_tool(std::move(d));
    env.start();
    env.model.push_calls({{"f1", "find_tools", {{"query", "messages read send chat colleague"}}}});
    env.model.push_calls({{"c1", "messages.read", {{"user", "bob"}}}});
    env.model.push_calls({{"c2", "messages.send", {{"chat", "chat-bob"}, {"text", "Yes, I am here."}, {"rationale", "The user asked to answer Bob."}}}});
    env.model.push_text("Sent.");
    const auto t = env.thread();
    const auto run = env.run(t, "Answer Bob's last message");
    ASSERT_TRUE(env.wait_for([&] { return !env.rt->pending_actions(t).empty(); }))
        << env.tool_content(run, "c1") << "\n" << to_string(env.rec(run).state) << " " << env.rec(run).reason;
    const auto actions = env.rt->pending_actions(t);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(jstr(actions[0].payload, "tool"), "messages.send") << actions[0].payload.dump();
    // The approval lists the message content the agent read as untrusted context from bob.
    EXPECT_NE(actions[0].payload.dump().find("bob"), std::string::npos) << actions[0].payload.dump();
    EXPECT_EQ(r.server.count("POST", "chats/chat-bob/messages"), 0);
    ASSERT_TRUE(env.rt->resolve_approval(actions[0].id, runtime::ApprovalResolution::approve()));
    ASSERT_TRUE(env.wait(run, runtime::RunState::succeeded));
    EXPECT_EQ(r.server.count("POST", "chats/chat-bob/messages"), 1);
    EXPECT_EQ(r.server.chats["chat-bob"].msgs.back().text, "Yes, I am here.");
}
