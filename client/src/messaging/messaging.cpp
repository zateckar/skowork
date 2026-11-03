#include "messaging/messaging.h"

#include "base/strings.h"
#include "base/time.h"
#include "base/url.h"
#include "platform/crypto.h"
#include "platform/log.h"

#include <algorithm>
#include <map>
#include <set>

namespace wb::messaging {
namespace {

constexpr char kLog[] = "messaging";

// Errors that mean "try again later" rather than "this cannot be sent".
bool transient(const Error& e) {
    return e.code == "offline" || e.code == "network" || e.code == "timeout" || e.code == "unauthorized" ||
           starts_with(e.code, "http_5") || e.code == "http_429" || e.code == "MESSAGING_OFF";
}

std::string lower(const std::string& s) { return to_lower(s); }

}  // namespace

MessagingService::MessagingService(Database& db, MessagingEnv env) : db_(db), env_(std::move(env)) {
    // The Chats cached before this version get their Inbox entries, also while offline (once the user is known, since
    // a direct Chat's entry names the other user).
    if (!me().empty()) refresh_inbox();
}

bool MessagingService::enabled() const { return !env_.enabled || env_.enabled(); }
bool MessagingService::connected() const { return env_.connected && env_.connected(); }
std::string MessagingService::me() const { return env_.me ? env_.me() : std::string{}; }

void MessagingService::notify(const std::string& chat) {
    refresh_inbox();
    if (!env_.changed) return;
    json e = {{"unread", unread_total()}};
    if (!chat.empty()) e["chat"] = chat;
    env_.changed(e);
}

// ---- the cache -------------------------------------------------------------------------------------------------------

std::string MessagingService::state(const std::string& key, const std::string& fallback) {
    auto st = db_.prepare("SELECT value FROM msg_state WHERE key = ?");
    st.bind(1, key);
    return st.step() ? st.col_text(0) : fallback;
}

void MessagingService::set_state(const std::string& key, const std::string& value) {
    auto st = db_.prepare("INSERT INTO msg_state(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    st.bind(1, key).bind(2, value);
    (void)st.run();
}

void MessagingService::store_chat(const json& view) {
    const std::string id = jstr(view, "id");
    if (id.empty()) return;
    json data = view;
    const std::lock_guard lock(db_.mutex());
    // A read position not yet sent keeps the local one.
    auto cur = db_.prepare("SELECT data, read_dirty FROM msg_chats WHERE id = ?");
    cur.bind(1, id);
    bool dirty = false;
    if (cur.step()) {
        dirty = cur.col_int(1) != 0;
        if (auto old = parse_json(cur.col_text(0)); old && dirty)
            data["read_seq"] = std::max(jint(*old, "read_seq"), jint(data, "read_seq"));
    }
    const json* last = jobj(data, "last_message");
    const std::string at = last ? jstr(*last, "at") : jstr(data, "created_at");
    auto st = db_.prepare("INSERT INTO msg_chats(id, data, last_at, read_dirty) VALUES(?, ?, ?, ?) "
                          "ON CONFLICT(id) DO UPDATE SET data = excluded.data, last_at = excluded.last_at");
    st.bind(1, id).bind(2, data.dump()).bind(3, at).bind(4, dirty ? 1 : 0);
    (void)st.run();
}

void MessagingService::store_message(const json& m) {
    const std::string chat = jstr(m, "chat"), id = jstr(m, "id");
    if (chat.empty() || id.empty()) return;
    const std::lock_guard lock(db_.mutex());
    // A queued message the control plane accepted stops being pending; a message seen twice is stored once.
    auto st = db_.prepare("INSERT INTO msg_messages(chat, id, seq, sender, sender_name, text, at, pending, queued_at, error) "
                          "VALUES(?, ?, ?, ?, ?, ?, ?, 0, 0, '') ON CONFLICT(chat, id) DO UPDATE SET seq = excluded.seq, "
                          "sender = excluded.sender, sender_name = excluded.sender_name, at = excluded.at, pending = 0, error = ''");
    st.bind(1, chat).bind(2, id).bind(3, jint(m, "seq")).bind(4, jstr(m, "sender")).bind(5, jstr(m, "sender_name"))
        .bind(6, jstr(m, "text")).bind(7, jstr(m, "at"));
    (void)st.run();
    // The Chat's newest message and sequence number.
    auto cur = db_.prepare("SELECT data FROM msg_chats WHERE id = ?");
    cur.bind(1, chat);
    if (!cur.step()) return;
    auto data = parse_json(cur.col_text(0));
    if (!data) return;
    const long long seq = jint(m, "seq");
    if (seq >= jint(*data, "last_seq")) {
        (*data)["last_seq"] = seq;
        (*data)["last_message"] = m;
        // My own message moves my read position (the control plane does the same).
        const bool mine = iequals(jstr(m, "sender"), me());
        if (mine) (*data)["read_seq"] = std::max(jint(*data, "read_seq"), seq);
        auto up = db_.prepare("UPDATE msg_chats SET data = ?, last_at = ? WHERE id = ?");
        up.bind(1, data->dump()).bind(2, jstr(m, "at")).bind(3, chat);
        (void)up.run();
        // A colleague's new message brings an archived Chat back to its folder or the Inbox (add-chats-in-inbox D1).
        if (!mine) {
            auto un = db_.prepare("UPDATE chat_inbox SET archived = 0 WHERE chat_id = ?");
            un.bind(1, chat);
            (void)un.run();
        }
    }
}

// The Inbox entries of the Chats (client/work-inbox Chat Entries; add-chats-in-inbox design D1): what each entry shows,
// written whenever the cache changed. Folder, pin and archive state stay; a Chat that is gone loses its entry.
void MessagingService::refresh_inbox() {
    const std::lock_guard lock(db_.mutex());
    const json list = chats();
    std::set<std::string> ids;
    for (const auto& c : list) {
        const std::string id = jstr(c, "id");
        if (id.empty()) continue;
        ids.insert(id);
        const bool team = jstr(c, "kind") == "team";
        std::string title = jstr(c, "name"), presence;
        if (const json* p = jobj(c, "peer"); p && !team) {
            title = jstr(*p, "display_name", jstr(*p, "user_name"));
            presence = jstr(*p, "presence", "offline");
        }
        if (title.empty())
            for (const auto& mem : c.value("members", json::array()))
                if (!iequals(jstr(mem, "user_name"), me())) title += (title.empty() ? "" : ", ") + jstr(mem, "display_name", jstr(mem, "user_name"));
        std::string preview, at;
        if (const json* l = jobj(c, "last_message"); l && l->is_object()) {
            preview = jstr(*l, "text");
            at = jstr(*l, "at");
        }
        if (preview.size() > 200) preview = utf8_truncate(preview, 200);
        if (at.empty()) at = jstr(c, "created_at");
        auto st = db_.prepare("INSERT INTO chat_inbox(chat_id, kind, title, preview, presence, last_activity, unread) VALUES(?, ?, ?, ?, ?, ?, ?) "
                              "ON CONFLICT(chat_id) DO UPDATE SET kind = excluded.kind, title = excluded.title, preview = excluded.preview, "
                              "presence = excluded.presence, last_activity = excluded.last_activity, unread = excluded.unread");
        st.bind(1, id).bind(2, team ? "team" : "direct").bind(3, title).bind(4, preview).bind(5, presence).bind(6, at).bind(7, jint(c, "unread"));
        (void)st.run();
    }
    std::vector<std::string> gone;
    {
        auto st = db_.prepare("SELECT chat_id FROM chat_inbox");
        while (st.step())
            if (!ids.count(st.col_text(0))) gone.push_back(st.col_text(0));
    }
    for (const auto& id : gone) {
        auto st = db_.prepare("DELETE FROM chat_inbox WHERE chat_id = ?");
        st.bind(1, id);
        (void)st.run();
    }
}

void MessagingService::remove_chat(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    for (const char* sql : {"DELETE FROM msg_messages WHERE chat = ?", "DELETE FROM msg_chats WHERE id = ?"}) {
        auto st = db_.prepare(sql);
        st.bind(1, id);
        (void)st.run();
    }
}

// Keeps the newest kKeptPerChat sent messages of a Chat; queued ones always stay.
void MessagingService::trim_cache(const std::string& chat) {
    auto st = db_.prepare("DELETE FROM msg_messages WHERE chat = ? AND pending = 0 AND seq <= "
                          "(SELECT seq FROM msg_messages WHERE chat = ? AND pending = 0 ORDER BY seq DESC LIMIT 1 OFFSET ?)");
    st.bind(1, chat).bind(2, chat).bind(3, kKeptPerChat);
    (void)st.run();
}

long long MessagingService::newest_seq(const std::string& chat) {
    auto st = db_.prepare("SELECT COALESCE(MAX(seq), 0) FROM msg_messages WHERE chat = ? AND pending = 0");
    st.bind(1, chat);
    return st.step() ? st.col_int(0) : 0;
}

json MessagingService::chat_view(const json& data) {
    json c = data;
    const std::string id = jstr(c, "id");
    const std::string mine = lower(me());
    auto st = db_.prepare("SELECT COUNT(*) FROM msg_messages WHERE chat = ? AND pending = 0 AND seq > ? AND lower(sender) != ?");
    st.bind(1, id).bind(2, jint(c, "read_seq")).bind(3, mine);
    c["unread"] = st.step() ? st.col_int(0) : 0;
    // The newest message, pending ones included, for the list.
    auto last = db_.prepare("SELECT id, seq, sender, sender_name, text, at, pending FROM msg_messages WHERE chat = ? "
                            "ORDER BY pending DESC, CASE WHEN pending = 1 THEN queued_at ELSE seq END DESC LIMIT 1");
    last.bind(1, id);
    if (last.step())
        c["last_message"] = {{"id", last.col_text(0)}, {"chat", id}, {"seq", last.col_int(1)}, {"sender", last.col_text(2)},
                             {"sender_name", last.col_text(3)}, {"text", last.col_text(4)}, {"at", last.col_text(5)},
                             {"pending", last.col_int(6) != 0}};
    // For a direct Chat, the other user and their Presence.
    if (jstr(c, "kind") == "direct")
        for (const auto& m : c.value("members", json::array()))
            if (lower(jstr(m, "user_name")) != mine) {
                c["peer"] = m;
                break;
            }
    return c;
}

json MessagingService::chats() {
    json out = json::array();
    const std::lock_guard lock(db_.mutex());
    std::vector<json> rows;
    {
        auto st = db_.prepare("SELECT data FROM msg_chats ORDER BY last_at DESC");
        while (st.step())
            if (auto d = parse_json(st.col_text(0))) rows.push_back(std::move(*d));
    }
    std::vector<json> views;
    for (const auto& d : rows) views.push_back(chat_view(d));
    // Pending messages count as the newest activity.
    std::stable_sort(views.begin(), views.end(), [](const json& a, const json& b) {
        const json* la = jobj(a, "last_message");
        const json* lb = jobj(b, "last_message");
        const bool pa = la && jbool(*la, "pending"), pb = lb && jbool(*lb, "pending");
        if (pa != pb) return pa;
        return (la ? jstr(*la, "at") : jstr(a, "created_at")) > (lb ? jstr(*lb, "at") : jstr(b, "created_at"));
    });
    for (auto& v : views) out.push_back(std::move(v));
    return out;
}

json MessagingService::chat(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("SELECT data FROM msg_chats WHERE id = ?");
    st.bind(1, id);
    if (!st.step()) return nullptr;
    auto d = parse_json(st.col_text(0));
    return d ? chat_view(*d) : json(nullptr);
}

json MessagingService::messages(const std::string& chat, int limit) {
    json out = json::array();
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("SELECT id, seq, sender, sender_name, text, at, pending, error FROM ("
                          "SELECT * FROM msg_messages WHERE chat = ? ORDER BY pending DESC, "
                          "CASE WHEN pending = 1 THEN queued_at ELSE seq END DESC LIMIT ?) "
                          "ORDER BY pending, CASE WHEN pending = 1 THEN queued_at ELSE seq END");
    st.bind(1, chat).bind(2, std::clamp(limit, 1, kKeptPerChat));
    while (st.step()) {
        json m = {{"id", st.col_text(0)}, {"chat", chat}, {"seq", st.col_int(1)}, {"sender", st.col_text(2)},
                  {"sender_name", st.col_text(3)}, {"text", st.col_text(4)}, {"at", st.col_text(5)}, {"pending", st.col_int(6) != 0}};
        if (const std::string e = st.col_text(7); !e.empty()) m["error"] = e;
        out.push_back(std::move(m));
    }
    return out;
}

long long MessagingService::unread_total() {
    long long n = 0;
    for (const auto& c : chats()) n += jint(c, "unread");
    return n;
}

bool MessagingService::busy() { return state("busy", "0") == "1"; }

json MessagingService::status() {
    return {{"enabled", enabled()}, {"connected", connected()}, {"me", me()}, {"busy", busy()}, {"unread", enabled() ? unread_total() : 0}};
}

// ---- catch-up and the outbox ---------------------------------------------------------------------------------------

Status MessagingService::fetch_after(const std::string& chat, long long after) {
    for (int page = 0; page < 20; ++page) {
        auto r = env_.api("GET", "chats/" + url_encode(chat) + "/messages?after=" + std::to_string(after) + "&limit=500", nullptr, false);
        if (!r) return r.error();
        const json list = r->value("messages", json::array());
        for (const auto& m : list) {
            store_message(m);
            after = std::max(after, jint(m, "seq"));
        }
        if (list.size() < 500) break;
    }
    trim_cache(chat);
    return {};
}

Status MessagingService::sync() {
    if (!enabled() || !connected()) return make_error("offline", "Messaging is not connected.");
    const std::lock_guard lock(sync_mx_);
    auto r = env_.api("GET", "chats", nullptr, false);
    if (!r) return r.error();
    std::set<std::string> listed;
    for (const auto& c : r->value("chats", json::array())) {
        const std::string id = jstr(c, "id");
        listed.insert(id);
        store_chat(c);
        const long long have = newest_seq(id), last = jint(c, "last_seq");
        // A Chat seen for the first time loads its last kKeptPerChat messages only.
        if (last > have)
            if (Status s = fetch_after(id, have == 0 ? std::max(0LL, last - kKeptPerChat) : have); !s) return s;
    }
    // Chats the user left or was removed from.
    std::vector<std::string> gone;
    {
        auto st = db_.prepare("SELECT id FROM msg_chats");
        while (st.step())
            if (!listed.count(st.col_text(0))) gone.push_back(st.col_text(0));
    }
    for (const auto& id : gone) {
        // A Chat with queued messages stays until they are sent or fail.
        auto p = db_.prepare("SELECT COUNT(*) FROM msg_messages WHERE chat = ? AND pending = 1");
        p.bind(1, id);
        if (p.step() && p.col_int(0) > 0) continue;
        remove_chat(id);
    }
    flush_reads();
    if (Status s = flush_outbox(); !s && transient(s.error())) return s;
    synced_ = true;
    notify();
    return {};
}

// Queued messages go in the order they were written; a message the control plane refuses is marked failed and the
// next one is tried; a connection problem stops the flush until the next attempt.
Status MessagingService::flush_outbox() {
    for (;;) {
        std::string chat, id, text;
        {
            auto st = db_.prepare("SELECT chat, id, text FROM msg_messages WHERE pending = 1 AND error = '' ORDER BY queued_at, rowid LIMIT 1");
            if (!st.step()) return {};
            chat = st.col_text(0);
            id = st.col_text(1);
            text = st.col_text(2);
        }
        auto r = env_.api("POST", "chats/" + url_encode(chat) + "/messages", {{"id", id}, {"text", text}}, false);
        if (!r) {
            if (transient(r.error())) return r.error();
            log::warn(kLog, "a queued message was refused: " + r.error().code);
            auto up = db_.prepare("UPDATE msg_messages SET error = ? WHERE chat = ? AND id = ?");
            up.bind(1, r.error().message.empty() ? r.error().code : r.error().message).bind(2, chat).bind(3, id);
            (void)up.run();
            notify(chat);
            continue;
        }
        store_message(*r);
        notify(chat);
    }
}

// Read positions moved while offline.
void MessagingService::flush_reads() {
    std::vector<std::pair<std::string, long long>> dirty;
    {
        auto st = db_.prepare("SELECT id, data FROM msg_chats WHERE read_dirty = 1");
        while (st.step())
            if (auto d = parse_json(st.col_text(1))) dirty.emplace_back(st.col_text(0), jint(*d, "read_seq"));
    }
    for (const auto& [id, seq] : dirty) {
        auto r = env_.api("POST", "chats/" + url_encode(id) + "/read", {{"seq", seq}}, false);
        if (!r && transient(r.error())) return;
        auto st = db_.prepare("UPDATE msg_chats SET read_dirty = 0 WHERE id = ?");
        st.bind(1, id);
        (void)st.run();
    }
}

void MessagingService::connection_changed(bool up) {
    synced_ = false;
    {
        const std::lock_guard lock(presence_mx_);
        reported_.clear();
    }
    if (!up || !enabled()) {
        notify();
        return;
    }
    if (Status s = sync(); !s) log::info(kLog, "catch-up: " + s.error().message);
    if (busy()) (void)env_.api("PUT", "presence", {{"busy", true}}, false);
}

void MessagingService::tick(long long idle_ms) {
    if (!enabled() || !connected()) return;
    const std::string now = idle_ms >= kIdleAfterMs ? "idle" : "active";
    bool report = false;
    {
        const std::lock_guard lock(presence_mx_);
        report = now != reported_;
    }
    if (report) {
        if (auto r = env_.api("PUT", "presence", {{"state", now}}, true); r) {
            const std::lock_guard lock(presence_mx_);
            reported_ = now;
        }
    }
    bool pending = false;
    {
        auto st = db_.prepare("SELECT 1 FROM msg_messages WHERE pending = 1 AND error = '' UNION SELECT 1 FROM msg_chats WHERE read_dirty = 1 LIMIT 1");
        pending = st.step();
    }
    if (!synced_ || pending) (void)sync();
}

// ---- pushes ----------------------------------------------------------------------------------------------------------

void MessagingService::on_push(const std::string& type, const json& payload) {
    if (!enabled()) return;
    if (type == "chat-message") {
        const json* m = jobj(payload, "message");
        const std::string chat = jstr(payload, "chat", m ? jstr(*m, "chat") : std::string{});
        if (!m || chat.empty()) return;
        json msg = *m;
        msg["chat"] = chat;
        bool known = false;
        {
            const std::lock_guard lock(db_.mutex());
            auto st = db_.prepare("SELECT 1 FROM msg_chats WHERE id = ?");
            st.bind(1, chat);
            known = st.step();
        }
        if (!known) {
            // A Chat this installation has not seen yet: the list and its messages.
            (void)sync();
            notify(chat);
            return;
        }
        const long long have = newest_seq(chat);
        store_message(msg);
        // A gap (a push that never came): fetch what is missing.
        if (have > 0 && jint(msg, "seq") > have + 1 && connected()) {
            const std::lock_guard lock(sync_mx_);
            (void)fetch_after(chat, have);
        }
        trim_cache(chat);
        notify(chat);
    } else if (type == "chat-changed") {
        (void)sync();
        notify(jstr(payload, "chat"));
    } else if (type == "presence-changed") {
        const std::string user = lower(jstr(payload, "user_name"));
        const std::string presence = jstr(payload, "presence");
        if (user.empty() || presence.empty()) return;
        bool changed = false;
        {
            const std::lock_guard lock(db_.mutex());
            std::vector<std::pair<std::string, json>> rows;
            {
                auto st = db_.prepare("SELECT id, data FROM msg_chats");
                while (st.step())
                    if (auto d = parse_json(st.col_text(1))) rows.emplace_back(st.col_text(0), std::move(*d));
            }
            for (auto& [id, d] : rows) {
                bool hit = false;
                if (d.contains("members") && d["members"].is_array())
                    for (auto& mem : d["members"])
                        if (lower(jstr(mem, "user_name")) == user && jstr(mem, "presence") != presence) {
                            mem["presence"] = presence;
                            hit = true;
                        }
                if (!hit) continue;
                changed = true;
                auto up = db_.prepare("UPDATE msg_chats SET data = ? WHERE id = ?");
                up.bind(1, d.dump()).bind(2, id);
                (void)up.run();
            }
        }
        if (changed) notify();
    }
}

// ---- what the user and the agent do ----------------------------------------------------------------------------------

Status MessagingService::open(const std::string& chat) {
    json c = this->chat(chat);
    if (c.is_null()) return make_error("not_found", "No such Chat.");
    if (connected() && enabled()) {
        const std::lock_guard lock(sync_mx_);
        (void)fetch_after(chat, newest_seq(chat));
        c = this->chat(chat);
    }
    const long long last = std::max(jint(c, "last_seq"), newest_seq(chat));
    if (jint(c, "read_seq") >= last) return {};
    {
        const std::lock_guard lock(db_.mutex());
        auto cur = db_.prepare("SELECT data FROM msg_chats WHERE id = ?");
        cur.bind(1, chat);
        if (cur.step())
            if (auto d = parse_json(cur.col_text(0))) {
                (*d)["read_seq"] = last;
                auto up = db_.prepare("UPDATE msg_chats SET data = ?, read_dirty = 1 WHERE id = ?");
                up.bind(1, d->dump()).bind(2, chat);
                (void)up.run();
            }
    }
    if (connected() && enabled()) flush_reads();
    notify(chat);
    return {};
}

Result<json> MessagingService::send(const std::string& chat, const std::string& raw) {
    if (!enabled()) return make_error("MESSAGING_OFF", "Messaging is turned off by your organisation's policy.");
    const std::string text = trim(raw);
    if (text.empty()) return make_error("invalid", "The message is empty.");
    if (text.size() > kMaxMessageBytes) return make_error("TOO_LARGE", "The message is longer than 8 KB.");
    if (this->chat(chat).is_null()) return make_error("not_found", "No such Chat.");
    const std::string id = random_urlsafe(16);  // 22 characters of [A-Za-z0-9_-]
    {
        const std::lock_guard lock(db_.mutex());
        long long order = 0;
        {
            auto st = db_.prepare("SELECT COALESCE(MAX(queued_at), 0) FROM msg_messages");
            if (st.step()) order = st.col_int(0);
        }
        auto st = db_.prepare("INSERT INTO msg_messages(chat, id, seq, sender, sender_name, text, at, pending, queued_at, error) "
                              "VALUES(?, ?, 0, ?, '', ?, ?, 1, ?, '')");
        st.bind(1, chat).bind(2, id).bind(3, me()).bind(4, text).bind(5, iso8601_utc_now())
            .bind(6, std::max(order + 1, unix_millis_now()));
        if (Status s = st.run(); !s) return s.error();
    }
    notify(chat);
    if (connected()) {
        const std::lock_guard lock(sync_mx_);
        (void)flush_outbox();
    }
    for (const auto& m : messages(chat, kKeptPerChat))
        if (jstr(m, "id") == id) {
            json out = {{"id", id}, {"chat", chat}, {"pending", jbool(m, "pending")}};
            if (!jbool(m, "pending")) out["seq"] = jint(m, "seq");
            if (m.contains("error")) out["error"] = m["error"];
            return out;
        }
    return json{{"id", id}, {"chat", chat}, {"pending", true}};
}

Result<json> MessagingService::directory(const std::string& query) {
    if (!enabled()) return make_error("MESSAGING_OFF", "Messaging is turned off by your organisation's policy.");
    return env_.api("GET", "directory?q=" + url_encode(trim(query)), nullptr, false);
}

Result<json> MessagingService::create_direct(const std::string& user) {
    if (!enabled()) return make_error("MESSAGING_OFF", "Messaging is turned off by your organisation's policy.");
    auto r = env_.api("POST", "chats", {{"kind", "direct"}, {"user", trim(user)}}, false);
    if (!r) return r;
    store_chat(*r);
    {
        const std::lock_guard lock(sync_mx_);
        (void)fetch_after(jstr(*r, "id"), newest_seq(jstr(*r, "id")));
    }
    notify(jstr(*r, "id"));
    return chat(jstr(*r, "id"));
}

Result<json> MessagingService::create_team(const std::string& name, const std::vector<std::string>& members) {
    if (!enabled()) return make_error("MESSAGING_OFF", "Messaging is turned off by your organisation's policy.");
    auto r = env_.api("POST", "chats", {{"kind", "team"}, {"name", trim(name)}, {"members", members}}, false);
    if (!r) return r;
    store_chat(*r);
    notify(jstr(*r, "id"));
    return chat(jstr(*r, "id"));
}

Result<json> MessagingService::rename(const std::string& id, const std::string& name) {
    auto r = env_.api("PATCH", "chats/" + url_encode(id), {{"name", trim(name)}}, false);
    if (!r) return r;
    store_chat(*r);
    notify(id);
    return chat(id);
}

Result<json> MessagingService::add_members(const std::string& id, const std::vector<std::string>& users) {
    auto r = env_.api("POST", "chats/" + url_encode(id) + "/members", {{"users", users}}, false);
    if (!r) return r;
    store_chat(*r);
    notify(id);
    return chat(id);
}

Status MessagingService::leave(const std::string& id) {
    auto r = env_.api("DELETE", "chats/" + url_encode(id) + "/members/me", nullptr, false);
    if (!r && r.error().code != "NOT_FOUND") return r.error();
    remove_chat(id);
    notify();
    return {};
}

Status MessagingService::set_busy(bool on) {
    set_state("busy", on ? "1" : "0");
    if (connected() && enabled())
        if (auto r = env_.api("PUT", "presence", {{"busy", on}}, false); !r) return r.error();
    notify();
    return {};
}

}  // namespace wb::messaging
