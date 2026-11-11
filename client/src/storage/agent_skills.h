// The agent-created Skill record (client/skills Agent-Created Skill Record; add-agent-created-skills design D6): which
// private Skills the agent created, when, and in which Thread. It holds no Skill text; the Skill itself is on disk.
#pragma once

#include "base/result.h"
#include "storage/database.h"

#include <optional>
#include <string>

namespace wb {

struct AgentSkillRecord {
    std::string name;
    std::string thread_id;  // the Thread of the Run that created it
    long long created_at = 0;  // unix milliseconds
    long long updated_at = 0;  // unix milliseconds of the last agent update
};

class AgentSkillRecords {
public:
    explicit AgentSkillRecords(Database& db) : db_(db) {}

    [[nodiscard]] std::optional<AgentSkillRecord> find(const std::string& name);
    // Inserts the record of a Skill the agent created (replaces a stale record of the same name).
    Status create(const std::string& name, const std::string& thread_id, long long now_ms);
    // Sets the last update time of a Skill the agent updated.
    Status touch(const std::string& name, long long now_ms);
    // Removes the record, when the user deletes the Skill in the editor.
    Status forget(const std::string& name);

private:
    Database& db_;
};

}  // namespace wb
