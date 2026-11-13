// Messaging in the desktop app (client/user-messaging; add-user-messaging design D4): the Chats and their recent
// messages cached in app.db, push messages applied as they come, catch-up after a reconnect, the outbox of messages
// written offline, the user's own Presence (idle after 10 minutes without input, busy until cleared) and the
// messages.* tools. The control plane's API is control-plane/user-messaging (design.md "API").
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "runtime/tools.h"
#include "storage/database.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wb::messaging {

inline constexpr size_t kMaxMessageBytes = 8192;           // the control plane's limit
inline constexpr long long kIdleAfterMs = 10 * 60 * 1000;  // Own Presence: idle after 10 minutes
inline constexpr int kKeptPerChat = 500;                   // recent messages cached per Chat

struct MessagingEnv {
    // A call to /api/v1/messaging/<path> with the user's token. `installation` adds the X-Skowork-Installation header.
    // Errors: "offline" while the control plane is not connected, the API's code (MESSAGING_OFF, NOT_FOUND,
    // USER_UNAVAILABLE, TOO_LARGE, ...) or http_<status>.
    std::function<Result<json>(const std::string& method, const std::string& path, const json& body, bool installation)> api;
    std::function<bool()> connected;  // the control channel is connected
    std::function<bool()> enabled;    // policy.messaging.mode is not off
    std::function<std::string()> me;  // the signed-in user's name (preferred_username)
    std::function<void(const json& event)> changed;  // {unread, chat?}: the UI and the tray update
};

class MessagingService {
public:
    // `db` is app.db (migration 1400 has the cache tables).
    MessagingService(Database& db, MessagingEnv env);

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::string me() const;

    // The control channel came up or went down: catch-up, the outbox and Presence after it came up.
    void connection_changed(bool up);
    // A push message (chat-message, chat-changed, presence-changed). Call off the channel thread.
    void on_push(const std::string& type, const json& payload);
    // Every 30 seconds with the time since the last keyboard or mouse input: reports active or idle on a change and
    // retries what failed (catch-up, the outbox, read positions).
    void tick(long long idle_ms);

    // Catch-up: the Chat list, then the messages after each Chat's newest cached one; then the outbox.
    Status sync();

    // ---- what the view and the tools read (from the cache)
    [[nodiscard]] json chats();                                   // newest activity first, with unread counts
    [[nodiscard]] json chat(const std::string& id);               // one Chat, or null
    [[nodiscard]] json messages(const std::string& chat, int limit = 200);  // oldest first, pending ones last
    [[nodiscard]] long long unread_total();
    [[nodiscard]] json status();  // {enabled, connected, me, busy, unread}

    // ---- what the user and the agent do
    // Fetches what is new in the Chat and moves the read position to its newest message.
    Status open(const std::string& chat);
    // Queues the message and sends it now when connected. {id, chat, pending, seq?}
    Result<json> send(const std::string& chat, const std::string& text);
    Result<json> directory(const std::string& query);
    Result<json> create_direct(const std::string& user);
    Result<json> create_team(const std::string& name, const std::vector<std::string>& members);
    Result<json> rename(const std::string& chat, const std::string& name);
    Result<json> add_members(const std::string& chat, const std::vector<std::string>& users);
    Status leave(const std::string& chat);
    Status set_busy(bool busy);
    [[nodiscard]] bool busy();

    // messages.find_user, messages.list_chats, messages.read (read) and messages.send (external); not offered while
    // messaging is off.
    std::vector<runtime::ToolDef> tools();

private:
    Status flush_outbox();
    Status fetch_after(const std::string& chat, long long after);
    void store_chat(const json& view);
    void store_message(const json& m);
    void remove_chat(const std::string& id);
    void refresh_inbox();  // the Chats' Inbox entries (chat_inbox, migration 1410)
    void trim_cache(const std::string& chat);
    long long newest_seq(const std::string& chat);
    void flush_reads();
    std::string state(const std::string& key, const std::string& fallback = {});
    void set_state(const std::string& key, const std::string& value);
    json chat_view(const json& data);
    void notify(const std::string& chat = {});

    Database& db_;
    MessagingEnv env_;
    std::mutex sync_mx_;  // one catch-up or outbox flush at a time
    std::atomic<bool> synced_{false};  // a catch-up succeeded since the connection came up
    std::mutex presence_mx_;
    std::string reported_;  // the activity state last reported ("" after a reconnect)
};

}  // namespace wb::messaging
