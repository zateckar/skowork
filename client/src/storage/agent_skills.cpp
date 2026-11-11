#include "storage/agent_skills.h"

#include <mutex>

namespace wb {

std::optional<AgentSkillRecord> AgentSkillRecords::find(const std::string& name) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT name, thread_id, created_at, updated_at FROM agent_skills WHERE name=?1");
    s.bind(1, name);
    if (!s.step()) return std::nullopt;
    return AgentSkillRecord{s.col_text(0), s.col_text(1), s.col_int(2), s.col_int(3)};
}

Status AgentSkillRecords::create(const std::string& name, const std::string& thread_id, long long now_ms) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("INSERT OR REPLACE INTO agent_skills(name, thread_id, created_at, updated_at) VALUES(?1,?2,?3,?3)");
    s.bind(1, name).bind(2, thread_id).bind(3, now_ms);
    return s.run();
}

Status AgentSkillRecords::touch(const std::string& name, long long now_ms) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("UPDATE agent_skills SET updated_at=?2 WHERE name=?1");
    s.bind(1, name).bind(2, now_ms);
    return s.run();
}

Status AgentSkillRecords::forget(const std::string& name) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM agent_skills WHERE name=?1");
    s.bind(1, name);
    return s.run();
}

}  // namespace wb
