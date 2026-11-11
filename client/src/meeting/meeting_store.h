// Meetings in app.db (add-meeting-capture design D5, migration 1200): one row per detected or manually captured meeting
// with its state, calendar match, audio directory, transcript segments and speaker names, and the series the user
// opted in for automatic capture.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "storage/database.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wb::meeting {

// offered -> notice -> capturing [-> interrupted -> capturing] -> pending -> transcribing -> summarizing -> done; declined, discarded (policy revoked
// during the capture), failed, unsupported (language) and expired (pending audio deleted by retention) end it.
struct MeetingRecord {
    std::string id, thread_id, state, source, app, title, event_id, series_id, event_link;
    std::vector<std::string> attendees;
    long long detected_at = 0, started_at = 0, ended_at = 0;
    unsigned pid = 0;
    std::string audio_dir;
    bool audio_deleted = false, user_started = false;
    std::string language, error, action_id, run_id;
    json segments = json::array();
    std::map<std::string, std::string> speakers;
    // The meeting vocabulary from the matched calendar event (improve-meeting-capture-quality D2, migration 1700).
    std::vector<std::string> vocabulary;
};

class MeetingStore {
public:
    explicit MeetingStore(Database& db) : db_(db) {}
    Status put(const MeetingRecord& m);
    std::optional<MeetingRecord> get(const std::string& id);
    std::optional<MeetingRecord> by_action(const std::string& action_id);
    std::vector<MeetingRecord> list(int limit = 100);
    std::vector<MeetingRecord> in_states(const std::vector<std::string>& states);
    Status remove(const std::string& id);

    Status add_series(const std::string& series_id, const std::string& title, long long now);
    Status remove_series(const std::string& series_id);
    bool has_series(const std::string& series_id);
    std::vector<std::pair<std::string, std::string>> series();  // (id, title)

    // The user's vocabulary terms (User Vocabulary Terms), oldest first.
    std::vector<std::string> terms();
    Status add_term(const std::string& term, long long now);
    Status remove_term(const std::string& term);

private:
    Database& db_;
};

}  // namespace wb::meeting
