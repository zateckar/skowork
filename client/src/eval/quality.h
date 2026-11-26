// Daily quality counters (client/telemetry Quality Counters; add-evaluation-export design D9): counts with no content,
// sent as one telemetry event per counter and day. Counts are kept in app.db until sent, so a restart loses nothing.
#pragma once

#include "base/result.h"
#include "telemetry/record.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {
class Database;
}

namespace wb::eval {

inline constexpr long long kWeekMs = 7LL * 86'400'000;

// The UTC day "YYYY-MM-DD" of a Unix millisecond time.
std::string utc_day(long long ms);

struct QualityRow {
    std::string day, step, counter, category, action, instruction_id;
    long long value = 0;
};

// The telemetry fields of one counter row: only the catalogue attributes of quality.daily, with values from fixed
// enumerations (step, counter, category, action), the instruction id and the count.
std::vector<telemetry::Field> quality_fields(const QualityRow& row);

class QualityCounters {
public:
    explicit QualityCounters(Database& app_db) : db_(app_db) {}

    // Adds to the count of a counter for a day. Unknown steps, counters, categories or actions are ignored.
    void add(const std::string& day, std::string_view step, std::string_view counter, std::string_view instruction_id,
             long long value = 1, std::string_view category = "none", std::string_view action = "none");
    // Derives the day's counts from local state once per finished day: decisions from the decision log, triage
    // suggestions, admitted Agenda items and transcribed meetings; and counts, once each, triage suggestions left
    // open for 7 days and speakers still unnamed 7 days after their meeting.
    void collect(long long now_ms);
    // Emits the rows of days before today and deletes them. With send false (telemetry off) nothing is emitted and
    // every row is dropped, so nothing counted while telemetry is off is sent later. Returns the number emitted.
    int flush(const std::function<void(const std::vector<telemetry::Field>&)>& emit, long long now_ms, bool send);

    [[nodiscard]] std::vector<QualityRow> rows() const;  // for tests and the viewer

private:
    void collect_day(const std::string& day, long long from_ms, long long to_ms);
    void sweep(long long now_ms);
    bool mark(const std::string& kind, const std::string& ref);  // true when it was not marked yet
    Database& db_;
};

}  // namespace wb::eval
