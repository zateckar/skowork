// Evaluation export (client/evaluation Export Of Items, Verdicts And Decisions, Export Of Meetings, Export Manifest;
// add-evaluation-export design D2): items of a time window with what Skowork concluded and what the user decided, as
// JSON lines in a new folder, plus meeting audio as WAV on request. Reads only; it changes nothing it exports.
#pragma once

#include "platform/http.h"  // CancelToken
#include "base/json.h"
#include "base/result.h"
#include "eval/decision_log.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace wb {
class Database;
}
namespace wb::mail {
class MailStore;
}
namespace wb::teams {
class TeamsService;
}
namespace wb::meeting {
class MeetingStore;
}

namespace wb::eval {

struct ExportRequest {
    std::filesystem::path out_dir;
    long long from_ms = 0, to_ms = 0;  // [from, to)
    std::set<std::string> sources;     // mail, teams, calendar, meetings
    bool audio = false;
};

struct ExportCounts {
    long long written = 0, withheld = 0;
};

struct ExportEnv {
    mail::MailStore* mail = nullptr;
    teams::TeamsService* teams = nullptr;
    meeting::MeetingStore* meetings = nullptr;
    Database* app_db = nullptr;  // triage_items, agenda_items and agenda_sources
    DecisionLog* decisions = nullptr;
    // The knowledge facts of a Source item, by Source kind and item id: [{statement, status, relations: [...]}].
    std::function<json(const std::string& source, const std::string& ext_id)> facts_of;
    std::function<long long()> now;
    std::string client_version;
};

// Validates a request: a writable absolute out_dir, a window with from < to, at least one known Source.
Status check_request(const ExportRequest& r);

class Exporter {
public:
    explicit Exporter(ExportEnv env) : env_(std::move(env)) {}

    // Creates <out_dir>/<YYYYMMDD-HHMMSS>/; fails when it exists or cannot be created.
    Result<std::filesystem::path> begin(const ExportRequest& r);
    Result<ExportCounts> export_mail(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel);
    Result<ExportCounts> export_teams(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel);
    Result<ExportCounts> export_calendar(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel);
    Result<ExportCounts> export_meetings(const std::filesystem::path& folder, const ExportRequest& r, const CancelToken& cancel);
    // Bytes the WAV files of the window's meetings would take (0 when audio is not requested).
    [[nodiscard]] long long audio_size(const ExportRequest& r) const;
    Status write_manifest(const std::filesystem::path& folder, const ExportRequest& r,
                          const std::map<std::string, ExportCounts>& counts, bool complete) const;

private:
    json triage_of(const std::string& scope, const std::string& item_id) const;
    json agenda_of(const std::string& kind, const std::string& source_id) const;
    json decisions_of(const std::string& item_ref) const;
    ExportEnv env_;
};

}  // namespace wb::eval
