// The user decision log (client/evaluation User Decision Log; add-evaluation-export design D8): every decision the user
// makes about a result Skowork produced, kept locally for evaluation and later per-user learning. It never leaves the
// device except through an evaluation export.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <string>
#include <vector>

namespace wb {
class Database;
}

namespace wb::eval {

inline constexpr long long kDecisionKeepMs = 365LL * 86'400'000;
inline constexpr size_t kDecisionValueLimit = 2048;  // serialized bytes of before or after

struct Decision {
    long long at_ms = 0;
    std::string step, kind, item_ref, result_ref, instruction_id;
    json before = json::object(), after = json::object();
};

class DecisionLog {
public:
    explicit DecisionLog(Database& app_db) : db_(app_db) {}
    // One insert, no transaction of its own: inside a caller's transaction it commits or rolls back with the decision.
    Status record(const Decision& d);
    [[nodiscard]] std::vector<Decision> for_item(const std::string& item_ref) const;
    [[nodiscard]] std::vector<Decision> between(long long from_ms, long long to_ms) const;  // inclusive, oldest first
    // Removes the entries of an item; for "meeting:<id>" also those whose result is one of its speakers.
    Status forget_item(const std::string& item_ref);
    int prune(long long now_ms);  // removes entries older than kDecisionKeepMs, returns how many
    Status forget_source(const std::string& kind);  // every entry whose item is of this kind ("mail", "teams", ...)
    Status forget_step(const std::string& step);    // every entry of a step ("knowledge" when all knowledge is forgotten)
    // Every knowledge entry whose fact names this entity as subject or object (a forgotten entity).
    Status forget_name(const std::string& name);

private:
    Database& db_;
};

std::string item_ref_mail(const std::string& id);
std::string item_ref_teams(const std::string& conversation, const std::string& message);
std::string item_ref_calendar(const std::string& id);
std::string item_ref_meeting(const std::string& id);

}  // namespace wb::eval
