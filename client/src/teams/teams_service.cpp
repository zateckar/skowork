#include "teams/teams_service.h"

#include "base/strings.h"
#include "platform/log.h"
#include "teams/indexeddb.h"
#include "teams/leveldb.h"
#include "teams/snapshot.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <set>

namespace wb::teams {
namespace {

using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;

constexpr char kLog[] = "teams";
constexpr long long kDayMs = 86'400'000;
constexpr size_t kMaxText = 20'000;

std::string iso(long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); }

Result<Extraction> default_read(const std::filesystem::path& cache, const std::filesystem::path& private_dir) {
    // One snapshot folder per profile: table files are reused by name and size, and file numbers repeat across databases.
    char sub[24];
    std::snprintf(sub, sizeof sub, "p%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(cache.wstring())));
    auto snap = snapshot_cache(cache, private_dir / sub);
    if (!snap) return snap.error();
    // Snapshots of other profiles, or from before per-profile folders, are private chat data no longer used.
    std::error_code ec;
    std::vector<std::filesystem::path> stale;
    for (std::filesystem::directory_iterator it(private_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() != std::filesystem::path(sub)) stale.push_back(it->path());
    for (const auto& p : stale) std::filesystem::remove_all(p, ec);
    auto db = LevelDbReader::open(*snap);
    if (!db) return db.error();
    return extract(IdbCatalog::build(*db));
}

StoredTeamsMessage row_message(Statement& q) {
    StoredTeamsMessage m;
    m.conversation_id = q.col_text(0);
    m.id = q.col_text(1);
    m.parent_id = q.col_text(2);
    m.sender_mri = q.col_text(3);
    m.sender_name = q.col_text(4);
    m.sender_email = q.col_text(5);
    m.at_ms = q.col_int(6);
    m.edited_ms = q.col_int(7);
    m.text = q.col_text(8);
    m.system = q.col_int(9) != 0;
    m.mine = q.col_int(10) != 0;
    m.mentions_me = q.col_int(11) != 0;
    m.version = q.col_text(12);
    return m;
}

constexpr char kMessageColumns[] =
    "conversation_id,id,parent_id,sender_mri,sender_name,sender_email,at_ms,edited_ms,text,system,mine,mentions_me,version";

std::string fts_query(const std::string& text) {
    // Each word as a quoted prefix term: user text never becomes FTS syntax.
    std::string out;
    for (auto w : split(text, ' ')) {
        w = trim(w);
        std::erase(w, '"');
        if (w.empty()) continue;
        out += (out.empty() ? "" : " ") + std::string("\"") + w + "\"*";
    }
    return out;
}

json schema(json props, std::vector<std::string> required = {}) {
    json s = {{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}

std::string hhmm(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", local.tm_hour, local.tm_min);
    return buf;
}

}  // namespace

std::string conversation_label(const StoredConversation& c) {
    if (!c.topic.empty()) return c.topic;
    if (c.type == "chat") return "Chat";
    if (c.type == "meeting") return "Meeting chat";
    if (c.type == "channel") return "Channel";
    return "Group chat";
}

TeamsService::TeamsService(TeamsEnv env) : env_(std::move(env)) {
    if (!env_.read) {
        const auto dir = env_.private_dir;
        env_.read = [dir](const std::filesystem::path& cache) { return default_read(cache, dir); };
    }
}

TeamsService::~TeamsService() { stop(); }

void TeamsService::start() {
    if (env_.background && !thread_.joinable()) thread_ = std::thread([this] { loop(); });
}

void TeamsService::stop() {
    stop_ = true;
    {
        const std::lock_guard lock(wake_mx_);
        wake_now_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TeamsService::request_sync() {
    {
        const std::lock_guard lock(wake_mx_);
        wake_now_ = true;
    }
    wake_.notify_all();
}

void TeamsService::loop() {
    long long next_due = 0;
    while (!stop_) {
        {
            std::unique_lock lock(wake_mx_);
            const long long now = env_.clock->unix_millis();
            if (next_due > now) wake_.wait_for(lock, std::chrono::milliseconds(std::min<long long>(next_due - now, 60'000)), [this] { return wake_now_; });
            if (!wake_now_ && env_.clock->unix_millis() < next_due) continue;
            wake_now_ = false;
        }
        if (stop_) break;
        next_due = env_.clock->unix_millis() + policy_int("teams.sync_interval_minutes", 10, 1) * 60'000;
        if (env_.paused && env_.paused()) {
            next_due = env_.clock->unix_millis() + 60'000;
            continue;
        }
        if (!enabled()) continue;
        (void)sync_now();
    }
}

// ---- settings ----

std::string TeamsService::state(const std::string& key, const std::string& fallback) {
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare("SELECT value FROM teams_state WHERE key=?1");
    q.bind(1, key);
    return q.step() ? q.col_text(0) : fallback;
}

Status TeamsService::put_state(const std::string& key, const std::string& value) {
    const std::lock_guard lock(env_.db->mutex());
    return env_.db->prepare("INSERT OR REPLACE INTO teams_state(key, value) VALUES(?1, ?2)").bind(1, key).bind(2, value).run();
}

long long TeamsService::policy_int(const std::string& key, long long fallback, long long min) {
    const auto v = env_.policy ? env_.policy(key) : std::nullopt;
    if (!v || !v->is_number_integer()) return fallback;
    return std::max(min, v->get<long long>());
}

bool TeamsService::available() {
    const auto v = env_.policy ? env_.policy("teams.mode") : std::nullopt;
    return !(v && v->is_string() && v->get<std::string>() == "off");
}

bool TeamsService::enabled() { return available() && state("enabled") == "1"; }

std::vector<std::string> TeamsService::excluded_conversations() {
    std::vector<std::string> out;
    if (auto j = parse_json(state("excluded", "[]")); j && j->is_array())
        for (const auto& x : *j)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

std::vector<std::string> TeamsService::excluded_types() {
    std::vector<std::string> out;
    if (const auto v = env_.policy ? env_.policy("teams.excluded_types") : std::nullopt; v && v->is_array())
        for (const auto& x : *v)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

std::filesystem::path TeamsService::chosen_profile() {
    const auto profiles = find_teams_profiles(env_.local_app_data);
    const std::string chosen = state("profile");
    for (const auto& p : profiles)
        if (!chosen.empty() && p.u8string() == std::u8string(chosen.begin(), chosen.end())) return p;
    return profiles.empty() ? std::filesystem::path{} : profiles.front();
}

json TeamsService::status() {
    json j;
    j["available"] = available();
    j["enabled"] = enabled();
    json profiles = json::array();
    for (const auto& p : find_teams_profiles(env_.local_app_data)) {
        const auto s = p.u8string();
        profiles.push_back(std::string(s.begin(), s.end()));
    }
    j["profiles"] = profiles;
    {
        const auto s = chosen_profile().u8string();
        j["profile"] = std::string(s.begin(), s.end());
    }
    {
        const std::lock_guard lock(env_.db->mutex());
        auto c = env_.db->prepare("SELECT COUNT(*) FROM teams_conversations");
        j["conversations"] = c.step() ? c.col_int(0) : 0;
        auto m = env_.db->prepare("SELECT COUNT(*) FROM teams_messages");
        j["messages"] = m.step() ? m.col_int(0) : 0;
    }
    const std::string last = state("last_read");
    j["last_read"] = last.empty() ? "" : iso(std::stoll(last));
    j["excluded"] = excluded_conversations();
    j["policy_excluded_types"] = excluded_types();
    const std::lock_guard lock(mx_);
    j["error"] = last_error_;
    j["counts"] = last_counts_;
    return j;
}

Status TeamsService::configure(const json& params) {
    // Turning Teams on, or reading another profile, starts with a backfill again: no events for what is already there.
    if (params.contains("profile") && params["profile"].is_string()) {
        const std::string profile = params["profile"].get<std::string>();
        if (profile != state("profile")) {
            if (Status s = put_state("profile", profile); !s) return s;
            if (Status s = put_state("baseline", ""); !s) return s;
        }
    }
    if (params.contains("enabled") && params["enabled"].is_boolean()) {
        const bool on = params["enabled"].get<bool>();
        if (on && !available()) return make_error("unavailable", "The organisation has turned off Teams chats (policy.teams.mode).");
        const bool was_on = state("enabled") == "1";
        if (on && !was_on)
            if (Status s = put_state("baseline", ""); !s) return s;
        if (Status s = put_state("enabled", on ? "1" : "0"); !s) return s;
        if (on) request_sync();
    }
    if (env_.changed) env_.changed();
    return {};
}

Status TeamsService::exclude(const std::string& conversation_id, bool excluded) {
    std::vector<TeamsEvent> gone;
    {
        // One transaction under the database lock: a sync reads the exclusions inside its own write transaction, and no
        // tool sees the conversation excluded but still stored.
        const std::lock_guard lock(env_.db->mutex());
        Status s = env_.db->transaction([&]() -> Status {
            auto list = excluded_conversations();
            std::erase(list, conversation_id);
            if (excluded) list.push_back(conversation_id);
            if (Status r = put_state("excluded", json(list).dump()); !r) return r;
            // An un-excluded conversation comes back without events for what it already holds.
            auto quiet = parse_json(state("quiet", "[]")).value_or(json::array());
            if (!quiet.is_array()) quiet = json::array();
            if (!excluded) quiet.push_back(conversation_id);
            if (Status r = put_state("quiet", quiet.dump()); !r) return r;
            if (!excluded) return {};
            auto q = env_.db->prepare("SELECT id, at_ms FROM teams_messages WHERE conversation_id=?1");
            q.bind(1, conversation_id);
            while (q.step()) gone.push_back({"teams.deleted", conversation_id, q.col_text(0), false, false, q.col_int(1)});
            if (Status r = env_.db->prepare("DELETE FROM teams_messages WHERE conversation_id=?1").bind(1, conversation_id).run(); !r) return r;
            if (Status r = env_.db->prepare("DELETE FROM teams_search WHERE conversation_id=?1").bind(1, conversation_id).run(); !r) return r;
            return env_.db->prepare("DELETE FROM teams_conversations WHERE id=?1").bind(1, conversation_id).run();
        });
        if (!s) return s;
    }
    if (!excluded) request_sync();
    publish(gone);
    if (env_.changed) env_.changed();
    return {};
}

Status TeamsService::forget() {
    // After any running read: it would otherwise write the copy and the snapshot files back.
    const std::lock_guard cycle(sync_mx_);
    std::vector<TeamsEvent> gone;
    {
        const std::lock_guard lock(env_.db->mutex());
        auto q = env_.db->prepare("SELECT conversation_id, id, at_ms FROM teams_messages");
        while (q.step()) gone.push_back({"teams.deleted", q.col_text(0), q.col_text(1), false, false, q.col_int(2)});
        Status s = env_.db->transaction([&]() -> Status {
            for (const char* sql : {"DELETE FROM teams_messages", "DELETE FROM teams_search", "DELETE FROM teams_conversations", "DELETE FROM teams_state"})
                if (Status r = env_.db->exec(sql); !r) return r;
            return {};
        });
        if (!s) return s;
    }
    std::error_code ec;
    std::filesystem::remove_all(env_.private_dir, ec);
    publish(gone);
    if (env_.changed) env_.changed();
    return {};
}

void TeamsService::on_event(std::function<void(const TeamsEvent&)> fn) {
    const std::lock_guard lock(mx_);
    listeners_.push_back(std::move(fn));
}

void TeamsService::publish(const std::vector<TeamsEvent>& events) {
    std::vector<std::function<void(const TeamsEvent&)>> ls;
    {
        const std::lock_guard lock(mx_);
        ls = listeners_;
    }
    for (const auto& e : events)
        for (const auto& fn : ls) {
            try {
                fn(e);
            } catch (const std::exception& ex) {
                log::warn(kLog, std::string("teams listener: ") + ex.what());
            }
        }
}

// ---- sync (Teams Sync, Teams Event Feed) ----

// Conversation types the organisation excludes leave the copy, also when the policy changed after they were stored.
Status TeamsService::purge_excluded_types() {
    const auto types = excluded_types();
    if (types.empty()) return {};
    std::vector<TeamsEvent> gone;
    {
        const std::lock_guard lock(env_.db->mutex());
        Status s = env_.db->transaction([&]() -> Status {
            for (const auto& type : types) {
                auto q = env_.db->prepare(
                    "SELECT m.conversation_id, m.id, m.at_ms FROM teams_messages m JOIN teams_conversations c ON c.id=m.conversation_id WHERE c.type=?1");
                q.bind(1, type);
                while (q.step()) gone.push_back({"teams.deleted", q.col_text(0), q.col_text(1), false, false, q.col_int(2)});
                const char* purge[] = {
                    "DELETE FROM teams_search WHERE conversation_id IN (SELECT id FROM teams_conversations WHERE type=?1)",
                    "DELETE FROM teams_messages WHERE conversation_id IN (SELECT id FROM teams_conversations WHERE type=?1)",
                    "DELETE FROM teams_conversations WHERE type=?1"};
                for (const char* sql : purge)
                    if (Status r = env_.db->prepare(sql).bind(1, type).run(); !r) return r;
            }
            return {};
        });
        if (!s) return s;
    }
    publish(gone);
    return {};
}

Result<TeamsReport> TeamsService::sync_now() {
    if (!enabled()) return make_error("unavailable", "Teams chats are not turned on");
    const std::lock_guard cycle(sync_mx_);
    // Before the read, so a policy change takes effect even when the cache cannot be read.
    if (Status s = purge_excluded_types(); !s) return s.error();
    const auto profile = chosen_profile();
    if (profile.empty() && !env_.local_app_data.empty()) {
        const std::lock_guard lock(mx_);
        last_error_ = "No new Teams data was found on this computer.";
        return make_error("not_found", last_error_);
    }
    // The decoders read untrusted files: anything they throw (out of memory included) fails this read, not the client.
    Result<Extraction> ex = make_error("io", "The Teams data could not be read.");
    try {
        ex = env_.read(profile);
    } catch (const std::exception& e) {
        ex = make_error("io", std::string("The Teams data could not be read: ") + e.what());
    }
    if (!ex) {
        const std::lock_guard lock(mx_);
        last_error_ = ex.error().message;
        log::warn(kLog, "read: " + ex.error().code + ": " + ex.error().message);
        return ex.error();
    }
    {
        const std::lock_guard lock(mx_);
        last_counts_ = {{"skipped", ex->skipped}, {"blobs", ex->blobs}, {"decode_errors", ex->decode_errors}};
        last_error_.clear();
    }
    if (ex->conversations.empty() && ex->messages.empty() && (ex->skipped > 0 || ex->decode_errors > 0 || ex->blobs > 0)) {
        // Unrecognised cache: the local copy stays as it is.
        const std::lock_guard lock(mx_);
        last_error_ = "The Teams data on this computer was not recognised; the local copy was not changed.";
        return make_error("unrecognised", last_error_);
    }

    const long long now = env_.clock->unix_millis();
    const long long cutoff = now - policy_int("teams.sync_days", 30, 1) * kDayMs;
    const auto types = excluded_types();
    // Read inside the write transaction below (under the database lock), so a concurrent exclude, Turn off or forget wins.
    std::vector<std::string> excluded, quiet;
    bool backfill = false;

    // Who the user is: the database owner's MRI, or the profile whose address is the mail account.
    std::string me = to_lower(ex->user_mri);
    const std::string account = env_.account ? to_lower(env_.account()) : std::string{};
    std::map<std::string, TeamsProfile> people;
    for (const auto& p : ex->profiles) {
        people[to_lower(p.mri)] = p;
        if (me.empty() && !account.empty() && to_lower(p.email) == account) me = to_lower(p.mri);
    }
    std::map<std::string, TeamsConversation> convs;
    for (const auto& c : ex->conversations) convs[c.id] = c;
    // A conversation whose record was not read gets its type from its id, as it is stored.
    const auto type_of = [&](const std::string& conv) {
        const auto it = convs.find(conv);
        return it != convs.end() ? it->second.type : conversation_type(json::object(), conv);
    };
    const auto skip = [&](const std::string& conv) {
        if (std::find(excluded.begin(), excluded.end(), conv) != excluded.end()) return true;
        const std::string type = type_of(conv);
        return std::find(types.begin(), types.end(), type) != types.end();
    };
    const auto listed = [](const std::string& json_list) {
        std::vector<std::string> out;
        if (auto j = parse_json(json_list); j && j->is_array())
            for (const auto& x : *j)
                if (x.is_string()) out.push_back(x.get<std::string>());
        return out;
    };

    TeamsReport r;
    std::vector<TeamsEvent> events;
    const std::lock_guard lock(env_.db->mutex());
    if (!enabled()) return make_error("unavailable", "Teams chats are not turned on");
    // Another profile chosen while this one was read: its first read is the backfill, not this one.
    if (chosen_profile() != profile) return make_error("unavailable", "The Teams profile changed during the read.");
    excluded = excluded_conversations();
    quiet = listed(state("quiet", "[]"));
    backfill = state("baseline").empty();
    Status written = env_.db->transaction([&]() -> Status {
        std::set<std::string> touched;
        for (const auto& m : ex->messages) {
            if (m.id.empty() || m.conversation_id.empty() || skip(m.conversation_id)) continue;
            if (m.deleted) {
                auto had = env_.db->prepare("SELECT 1 FROM teams_messages WHERE conversation_id=?1 AND id=?2");
                had.bind(1, m.conversation_id).bind(2, m.id);
                if (!had.step()) continue;  // never stored, or already removed
                auto d = env_.db->prepare("DELETE FROM teams_messages WHERE conversation_id=?1 AND id=?2");
                d.bind(1, m.conversation_id).bind(2, m.id);
                if (Status s = d.run(); !s) return s;
                (void)env_.db->prepare("DELETE FROM teams_search WHERE conversation_id=?1 AND message_id=?2").bind(1, m.conversation_id).bind(2, m.id).run();
                events.push_back({"teams.deleted", m.conversation_id, m.id, false, false, m.at_ms});
                ++r.removed;
                continue;
            }
            if (m.at_ms < cutoff) continue;
            std::string old_version;
            {
                auto q = env_.db->prepare("SELECT version FROM teams_messages WHERE conversation_id=?1 AND id=?2");
                q.bind(1, m.conversation_id).bind(2, m.id);
                if (q.step()) old_version = q.col_text(0);
                else old_version = "\x01none";
            }
            if (old_version == m.version) continue;
            const bool fresh = old_version == "\x01none";
            const bool mine = !me.empty() && to_lower(m.sender_mri) == me;
            bool mentions_me = false;
            for (const auto& mr : m.mention_mris)
                if (!me.empty() && to_lower(mr) == me) mentions_me = true;
            const bool one_to_one = type_of(m.conversation_id) == "chat";
            const auto person = people.find(to_lower(m.sender_mri));
            std::string name = m.sender_name.empty() && person != people.end() ? person->second.name : m.sender_name;
            const std::string email = person != people.end() ? person->second.email : std::string{};
            std::string text = m.text.empty() ? html_to_text(m.html) : m.text;
            if (text.size() > kMaxText) text = utf8_truncate(text, kMaxText);
            auto u = env_.db->prepare(std::string("INSERT OR REPLACE INTO teams_messages(") + kMessageColumns +
                                      ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)");
            u.bind(1, m.conversation_id).bind(2, m.id).bind(3, m.parent_id).bind(4, m.sender_mri).bind(5, name).bind(6, email);
            u.bind(7, m.at_ms).bind(8, m.edited_ms).bind(9, m.system ? std::string{} : text).bind(10, m.system ? 1 : 0);
            u.bind(11, mine ? 1 : 0).bind(12, mentions_me ? 1 : 0).bind(13, m.version);
            if (Status s = u.run(); !s) return s;
            (void)env_.db->prepare("DELETE FROM teams_search WHERE conversation_id=?1 AND message_id=?2").bind(1, m.conversation_id).bind(2, m.id).run();
            if (!m.system) {
                auto f = env_.db->prepare("INSERT INTO teams_search(text, sender, conversation_id, message_id) VALUES(?1,?2,?3,?4)");
                if (Status s = f.bind(1, text).bind(2, name).bind(3, m.conversation_id).bind(4, m.id).run(); !s) return s;
            }
            touched.insert(m.conversation_id);
            ++r.messages;
            if (backfill || m.system) continue;
            if (std::find(quiet.begin(), quiet.end(), m.conversation_id) != quiet.end()) continue;
            events.push_back({fresh ? "teams.message" : "teams.updated", m.conversation_id, m.id, one_to_one || mentions_me, mine, m.at_ms});
        }
        for (const auto& [id, c] : convs) {
            if (skip(id)) continue;
            auto u = env_.db->prepare("INSERT OR REPLACE INTO teams_conversations(id, type, topic, last_activity) VALUES(?1,?2,?3,?4)");
            if (Status s = u.bind(1, id).bind(2, c.type).bind(3, c.topic).bind(4, c.last_activity_ms).run(); !s) return s;
            ++r.conversations;
        }
        // Conversations only seen through their messages still get a row.
        for (const auto& id : touched) {
            if (convs.count(id)) continue;
            auto u = env_.db->prepare("INSERT OR IGNORE INTO teams_conversations(id, type) VALUES(?1, ?2)");
            if (Status s = u.bind(1, id).bind(2, conversation_type(json::object(), id)).run(); !s) return s;
        }
        // The sync window: older messages leave the copy, without events.
        if (Status s = env_.db->prepare("DELETE FROM teams_search WHERE message_id IN (SELECT id FROM teams_messages WHERE at_ms < ?1)").bind(1, cutoff).run(); !s) return s;
        if (Status s = env_.db->prepare("DELETE FROM teams_messages WHERE at_ms < ?1").bind(1, cutoff).run(); !s) return s;
        if (Status s = env_.db->prepare("INSERT OR REPLACE INTO teams_state(key, value) VALUES('last_read', ?1)").bind(1, std::to_string(now)).run(); !s) return s;
        // The baseline is set by the first read that found messages: an empty or not yet filled cache is no baseline.
        if (backfill && !ex->messages.empty())
            if (Status s = env_.db->prepare("INSERT OR REPLACE INTO teams_state(key, value) VALUES('baseline', ?1)").bind(1, std::to_string(now)).run(); !s)
                return s;
        if (Status s = env_.db->exec("DELETE FROM teams_state WHERE key='quiet'"); !s) return s;
        if (!me.empty()) {
            if (Status s = env_.db->prepare("INSERT OR REPLACE INTO teams_state(key, value) VALUES('me', ?1)").bind(1, me).run(); !s) return s;
            // The user may have become known only now (the mail account connected later): stored messages follow.
            if (Status s = env_.db->prepare("UPDATE teams_messages SET mine = (lower(sender_mri) = ?1) WHERE mine <> (lower(sender_mri) = ?1)").bind(1, me).run(); !s)
                return s;
        }
        return {};
    });
    if (!written) return written.error();
    r.published = static_cast<int>(events.size());
    publish(events);
    if (env_.changed) env_.changed();
    return r;
}

// ---- queries ----

std::optional<StoredConversation> TeamsService::conversation(const std::string& id) {
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare("SELECT id, type, topic, last_activity FROM teams_conversations WHERE id=?1");
    q.bind(1, id);
    if (!q.step()) return std::nullopt;
    return StoredConversation{q.col_text(0), q.col_text(1), q.col_text(2), q.col_int(3)};
}

std::string TeamsService::display_name(const StoredConversation& c) {
    if (!c.topic.empty()) return c.topic;
    // A chat without a topic is named after the other people in it.
    const std::string me = state("me");
    std::set<std::string> names;
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare("SELECT DISTINCT sender_name FROM teams_messages WHERE conversation_id=?1 AND mine=0 AND sender_name<>'' LIMIT 4");
    q.bind(1, c.id);
    while (q.step()) names.insert(q.col_text(0));
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out.empty() ? conversation_label(c) : out;
}

std::optional<StoredTeamsMessage> TeamsService::message(const std::string& conversation_id, const std::string& id) {
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare(std::string("SELECT ") + kMessageColumns + " FROM teams_messages WHERE conversation_id=?1 AND id=?2");
    q.bind(1, conversation_id).bind(2, id);
    if (!q.step()) return std::nullopt;
    return row_message(q);
}

std::vector<StoredTeamsMessage> TeamsService::messages(const std::string& conversation_id, long long from, long long to, size_t limit) {
    std::vector<StoredTeamsMessage> out;
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare(std::string("SELECT ") + kMessageColumns +
                              " FROM teams_messages WHERE conversation_id=?1 AND at_ms>=?2 AND at_ms<?3 AND system=0 ORDER BY at_ms DESC LIMIT ?4");
    q.bind(1, conversation_id).bind(2, from).bind(3, to).bind(4, static_cast<long long>(limit));
    while (q.step()) out.push_back(row_message(q));
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<StoredTeamsMessage> TeamsService::messages_before(const std::string& conversation_id, long long at_ms, const std::string& id,
                                                              long long from, size_t limit) {
    std::vector<StoredTeamsMessage> out;
    if (limit == 0) return out;
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare(std::string("SELECT ") + kMessageColumns +
                              " FROM teams_messages WHERE conversation_id=?1 AND (at_ms<?2 OR (at_ms=?2 AND id<?3)) AND at_ms>=?4 AND system=0 "
                              "ORDER BY at_ms DESC, id DESC LIMIT ?5");
    q.bind(1, conversation_id).bind(2, at_ms).bind(3, id).bind(4, from).bind(5, static_cast<long long>(limit));
    while (q.step()) out.push_back(row_message(q));
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<StoredTeamsMessage> TeamsService::messages_after(const std::string& conversation_id, long long at_ms, const std::string& id,
                                                             long long to, size_t limit) {
    std::vector<StoredTeamsMessage> out;
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare(std::string("SELECT ") + kMessageColumns +
                              " FROM teams_messages WHERE conversation_id=?1 AND (at_ms>?2 OR (at_ms=?2 AND id>?3)) AND at_ms<?4 AND system=0 "
                              "ORDER BY at_ms ASC, id ASC LIMIT ?5");
    q.bind(1, conversation_id).bind(2, at_ms).bind(3, id).bind(4, to).bind(5, static_cast<long long>(limit));
    while (q.step()) out.push_back(row_message(q));
    return out;
}

std::vector<StoredTeamsMessage> TeamsService::search(const std::string& text, const std::string& conversation_id, const std::string& sender,
                                                     long long since, long long until, size_t limit) {
    std::vector<StoredTeamsMessage> out;
    const std::lock_guard lock(env_.db->mutex());
    std::string cols;
    for (auto c : split(kMessageColumns, ',')) cols += (cols.empty() ? "m." : ",m.") + std::string(trim(c));
    const std::string fq = fts_query(text);
    // With words, the full-text index drives the query (one MATCH), not one MATCH per stored message.
    std::string sql = "SELECT " + cols +
                      (fq.empty() ? std::string(" FROM teams_messages m WHERE m.system=0")
                                  : std::string(" FROM teams_search s JOIN teams_messages m ON m.conversation_id=s.conversation_id AND "
                                                "m.id=s.message_id WHERE teams_search MATCH ?1 AND m.system=0"));
    if (!conversation_id.empty()) sql += " AND m.conversation_id=?2";
    if (!sender.empty()) sql += " AND (lower(m.sender_name) LIKE ?3 OR lower(m.sender_email) LIKE ?3)";
    if (since) sql += " AND m.at_ms>=?4";
    if (until) sql += " AND m.at_ms<?5";
    sql += " ORDER BY m.at_ms DESC LIMIT ?6";
    auto q = env_.db->prepare(sql);
    if (!fq.empty()) q.bind(1, fq);
    if (!conversation_id.empty()) q.bind(2, conversation_id);
    if (!sender.empty()) q.bind(3, "%" + to_lower(sender) + "%");
    if (since) q.bind(4, since);
    if (until) q.bind(5, until);
    q.bind(6, static_cast<long long>(limit));
    while (q.step()) out.push_back(row_message(q));
    return out;
}

std::vector<StoredTeamsMessage> TeamsService::relevant_since(long long since, long long active_since, size_t limit) {
    std::vector<StoredTeamsMessage> out;
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare(
        std::string("SELECT ") + kMessageColumns +
        " FROM teams_messages m WHERE system=0 AND mine=0 AND at_ms>=?1 AND (mentions_me=1 OR "
        "conversation_id IN (SELECT id FROM teams_conversations WHERE type='chat') OR "
        "conversation_id IN (SELECT conversation_id FROM teams_messages WHERE mine=1 AND at_ms>=?2)) ORDER BY at_ms DESC LIMIT ?3");
    q.bind(1, since).bind(2, active_since).bind(3, static_cast<long long>(limit));
    while (q.step()) out.push_back(row_message(q));
    return out;
}

std::string TeamsService::day_text(const std::string& conversation_id, long long day_start, long long day_end) {
    std::string out;
    for (const auto& m : messages(conversation_id, day_start, day_end, 2000)) {
        out += "[" + hhmm(m.at_ms) + "] " + (m.sender_name.empty() ? std::string("Someone") : m.sender_name) + ": " + m.text + "\n";
        if (out.size() > 200'000) break;
    }
    return out;
}

std::vector<std::string> TeamsService::conversation_ids() {
    std::vector<std::string> out;
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare("SELECT id FROM teams_conversations ORDER BY last_activity DESC");
    while (q.step()) out.push_back(q.col_text(0));
    return out;
}

// ---- knowledge (Teams Chat Source) ----

std::string TeamsService::day_id(const std::string& conversation_id, long long at_ms) {
    const std::time_t t = static_cast<std::time_t>(at_ms / 1000);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return conversation_id + "|" + buf;
}

std::optional<json> TeamsService::knowledge_item(const std::string& id) {
    const size_t bar = id.rfind('|');
    if (bar == std::string::npos || id.size() - bar - 1 != 10) return std::nullopt;
    const std::string conv = id.substr(0, bar), date = id.substr(bar + 1);
    std::tm tm{};
    if (std::sscanf(date.c_str(), "%4d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return std::nullopt;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const long long start = static_cast<long long>(std::mktime(&tm)) * 1000;
    tm.tm_mday += 1;
    tm.tm_isdst = -1;
    const long long end = static_cast<long long>(std::mktime(&tm)) * 1000;
    const auto c = conversation(conv);
    if (!c || !enabled()) return std::nullopt;
    const auto list = messages(conv, start, end, 2000);
    if (list.empty()) return std::nullopt;
    json people = json::array();
    std::set<std::string> seen;
    for (const auto& m : list) {
        const std::string key = m.sender_email.empty() ? m.sender_name : to_lower(m.sender_email);
        if (key.empty() || !seen.insert(key).second) continue;
        people.push_back({{"name", m.sender_name}, {"address", m.sender_email}});
    }
    const std::string name = display_name(*c);
    return json{{"title", name + " – " + date},
                {"body", "Teams " + (c->type == "chat" ? std::string("chat") : c->type) + ": " + name + "\nDate: " + date + "\n\n" +
                             day_text(conv, start, end)},
                {"at", list.front().at_ms},
                {"ref", "teams:" + conv},
                {"withheld", false},
                {"people", people}};
}

std::vector<std::string> TeamsService::knowledge_ids() {
    std::set<std::string> out;
    if (!enabled()) return {};
    const std::lock_guard lock(env_.db->mutex());
    auto q = env_.db->prepare("SELECT conversation_id, at_ms FROM teams_messages WHERE system=0");
    while (q.step()) out.insert(day_id(q.col_text(0), q.col_int(1)));
    return {out.begin(), out.end()};
}

// ---- tools (Teams Tools) ----

std::vector<ToolDef> TeamsService::tools() {
    std::vector<ToolDef> out;
    const auto message_json = [this](const StoredTeamsMessage& m) {
        json j = {{"conversation_id", m.conversation_id}, {"id", m.id},     {"sender", m.sender_name},
                  {"sent", iso(m.at_ms)},                 {"text", m.text}, {"from_user", m.mine}};
        if (!m.sender_email.empty()) j["sender_email"] = m.sender_email;
        if (!m.parent_id.empty() && m.parent_id != m.id) j["reply_to"] = m.parent_id;
        if (m.mentions_me) j["mentions_user"] = true;
        if (auto c = conversation(m.conversation_id)) j["conversation"] = display_name(*c);
        return j;
    };
    const auto provenance = [](const std::string& id) { return runtime::Provenance{"teams", id, iso8601_utc_now(), false}; };
    const auto unavailable = [this]() -> std::optional<ToolOutput> {
        if (!available()) return ToolOutput::failure("Teams chats are turned off by the organisation.");
        if (!enabled()) return ToolOutput::failure("Teams chats are not turned on. The user can turn them on in Settings > Teams chats.");
        return std::nullopt;
    };

    ToolDef search;
    search.id = "teams.search";
    search.description =
        "Search the user's Microsoft Teams chats (read from the local Teams app, last 30 days by default) by words, conversation, "
        "sender or date range. Returns messages with conversation, sender, time and text, newest first. Skowork cannot post to Teams.";
    search.risk = Risk::read;
    search.parameters = schema({{"query", {{"type", "string"}}},
                                {"conversation_id", {{"type", "string"}}},
                                {"from", {{"type", "string"}, {"description", "Part of the sender's name or address"}}},
                                {"since", {{"type", "string"}, {"description", "ISO 8601 date-time"}}},
                                {"until", {{"type", "string"}, {"description", "ISO 8601 date-time"}}},
                                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}}});
    search.offered = [this] { return enabled(); };
    search.run = [this, message_json, provenance, unavailable](ToolContext&, const json& a) {
        if (auto u = unavailable()) return *u;
        const auto since = parse_iso8601_ms(jstr(a, "since")), until = parse_iso8601_ms(jstr(a, "until"));
        const size_t limit = static_cast<size_t>(std::clamp<long long>(jint(a, "limit", 20), 1, 50));
        json results = json::array();
        for (const auto& m : this->search(jstr(a, "query"), jstr(a, "conversation_id"), jstr(a, "from"), since.value_or(0), until.value_or(0), limit))
            results.push_back(message_json(m));
        const std::string last = state("last_read");
        ToolOutput o = ToolOutput::success(json{{"results", results}, {"last_read", last.empty() ? "never" : iso(std::stoll(last))}}.dump());
        o.provenance = provenance("search");
        return o;
    };
    search.provenance = {"teams", "", "", false};
    out.push_back(std::move(search));

    ToolDef read;
    read.id = "teams.read";
    read.description =
        "Read Teams messages: with message_id, that message and the messages around it in its conversation; without it, the "
        "conversation's messages in a date range (default: the last 7 days).";
    read.risk = Risk::read;
    read.parameters = schema({{"conversation_id", {{"type", "string"}}},
                              {"message_id", {{"type", "string"}}},
                              {"since", {{"type", "string"}}},
                              {"until", {{"type", "string"}}},
                              {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}},
                             {"conversation_id"});
    read.offered = [this] { return enabled(); };
    read.run = [this, message_json, provenance, unavailable](ToolContext&, const json& a) {
        if (auto u = unavailable()) return *u;
        const std::string conv = jstr(a, "conversation_id"), id = jstr(a, "message_id");
        const auto c = conversation(conv);
        if (!c) return ToolOutput::failure("Not found: no such Teams conversation in the local copy.");
        const size_t limit = static_cast<size_t>(std::clamp<long long>(jint(a, "limit", 50), 1, 200));
        std::vector<StoredTeamsMessage> found;
        if (!id.empty()) {
            const auto m = message(conv, id);
            if (!m) return ToolOutput::failure("Not found: no such Teams message in the local copy.");
            // The message itself always: up to half the limit before it (with it) and the rest after it, within two hours.
            const size_t before_n = (limit - 1) / 2;
            found = messages_before(conv, m->at_ms, m->id, m->at_ms - 2 * 3'600'000, before_n);
            found.push_back(*m);
            if (limit > found.size()) {
                auto after = messages_after(conv, m->at_ms, m->id, m->at_ms + 2 * 3'600'000, limit - found.size());
                found.insert(found.end(), std::make_move_iterator(after.begin()), std::make_move_iterator(after.end()));
            }
        } else {
            const long long to = parse_iso8601_ms(jstr(a, "until")).value_or(env_.clock->unix_millis() + 1);
            const long long from = parse_iso8601_ms(jstr(a, "since")).value_or(to - 7 * kDayMs);
            found = messages(conv, from, to, limit);
        }
        json list = json::array();
        for (const auto& m : found) list.push_back(message_json(m));
        ToolOutput o = ToolOutput::success(json{{"conversation", display_name(*c)}, {"type", c->type}, {"messages", list}}.dump());
        o.provenance = provenance(conv);
        return o;
    };
    read.provenance = {"teams", "", "", false};
    out.push_back(std::move(read));
    return out;
}

}  // namespace wb::teams
