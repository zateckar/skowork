// client/teams-chats in the host (add-local-outlook-and-teams design D4, D5): the user's opt-in, the read of a private
// snapshot of the new Teams cache, the local Teams copy with its sync window, exclusions and search index, the event
// feed for knowledge, the Agenda and the daily report, and the read-only tools teams.search and teams.read.
// Nothing is ever written to Teams or its cache.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "runtime/tools.h"
#include "storage/database.h"
#include "teams/teams_extract.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wb::teams {

using PolicyLookup = std::function<std::optional<json>(const std::string& key)>;

struct TeamsEnv {
    Database* db = nullptr;
    const Clock* clock = nullptr;
    PolicyLookup policy;
    std::filesystem::path local_app_data;  // %LOCALAPPDATA%, where the Teams package keeps its cache
    std::filesystem::path private_dir;     // the snapshot folder in the user's data root
    // The user's mail address (from client/mail-calendar), to recognise the user's own profile.
    std::function<std::string()> account;
    std::function<bool()> paused;   // Pause background work, battery saver, presentation mode
    std::function<void()> changed;  // settings or status changed (the UI reloads)
    // Reads one cache folder into an Extraction. Default: snapshot, LevelDB, IndexedDB, extract. Tests replace it.
    std::function<Result<Extraction>(const std::filesystem::path& cache)> read;
    bool background = true;
};

// teams.message | teams.updated | teams.deleted.
struct TeamsEvent {
    std::string kind, conversation_id, message_id;
    bool addressed = false;  // a one-to-one chat, or a mention of the user
    bool mine = false;
    long long at_ms = 0;
};

struct StoredTeamsMessage {
    std::string conversation_id, id, parent_id, sender_mri, sender_name, sender_email, text, version;
    long long at_ms = 0, edited_ms = 0;
    bool system = false, mine = false, mentions_me = false;
};

struct StoredConversation {
    std::string id, type, topic;
    long long last_activity = 0;
};

struct TeamsReport {
    int conversations = 0, messages = 0, published = 0, removed = 0;
};

class TeamsService {
public:
    explicit TeamsService(TeamsEnv env);
    ~TeamsService();
    TeamsService(const TeamsService&) = delete;
    TeamsService& operator=(const TeamsService&) = delete;

    void start();
    void stop();
    void request_sync();
    // One read of the cache now, on the calling thread. Fails "unavailable" while off or not enabled.
    Result<TeamsReport> sync_now();

    // ---- settings (Teams Chats Are Opt In, Teams Exclusions) ----
    [[nodiscard]] bool available();  // policy.teams.mode is on
    [[nodiscard]] bool enabled();    // available and turned on by the user
    json status();
    // {"enabled", "profile"}: any subset. Turning it on starts with a backfill (no events).
    Status configure(const json& params);
    Status exclude(const std::string& conversation_id, bool excluded);
    // Forget Teams chats: the local copy, its state and the user's exclusions.
    Status forget();

    // ---- the local copy ----
    std::optional<StoredConversation> conversation(const std::string& id);
    std::optional<StoredTeamsMessage> message(const std::string& conversation_id, const std::string& id);
    // Messages of a conversation in [from, to), oldest first, at most `limit`.
    std::vector<StoredTeamsMessage> messages(const std::string& conversation_id, long long from, long long to, size_t limit);
    // Newest first; empty text lists by time.
    std::vector<StoredTeamsMessage> search(const std::string& text, const std::string& conversation_id, const std::string& sender,
                                           long long since, long long until, size_t limit);
    // Messages from others since `since` that are addressed to the user or in conversations the user wrote in since
    // `active_since` (client/daily-report), newest first.
    std::vector<StoredTeamsMessage> relevant_since(long long since, long long active_since, size_t limit);
    // A conversation day as one text: "[hh:mm] Name: text" lines (client/knowledge-graph Teams Chat Source).
    std::string day_text(const std::string& conversation_id, long long day_start, long long day_end);
    std::vector<std::string> conversation_ids();
    // Knowledge items (Teams Chat Source): one per conversation and local day, id "<conversation>|<YYYY-MM-DD>".
    static std::string day_id(const std::string& conversation_id, long long at_ms);
    std::optional<json> knowledge_item(const std::string& day_id);
    std::vector<std::string> knowledge_ids();
    [[nodiscard]] std::string display_name(const StoredConversation& c);

    void on_event(std::function<void(const TeamsEvent&)> fn);
    std::vector<runtime::ToolDef> tools();

private:
    std::string state(const std::string& key, const std::string& fallback = {});
    Status put_state(const std::string& key, const std::string& value);
    std::vector<std::string> excluded_conversations();
    std::vector<std::string> excluded_types();
    long long policy_int(const std::string& key, long long fallback, long long min);
    std::filesystem::path chosen_profile();
    void loop();
    void publish(const std::vector<TeamsEvent>& events);

    TeamsEnv env_;
    std::mutex sync_mx_;
    std::mutex mx_;
    std::vector<std::function<void(const TeamsEvent&)>> listeners_;
    std::string last_error_;
    json last_counts_ = json::object();
    std::thread thread_;
    std::mutex wake_mx_;
    std::condition_variable wake_;
    bool wake_now_ = false;
    std::atomic<bool> stop_{false};
};

std::string conversation_label(const StoredConversation& c);

}  // namespace wb::teams
