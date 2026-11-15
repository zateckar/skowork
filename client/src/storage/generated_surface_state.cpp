#include "storage/generated_surface_state.h"

#include "platform/crypto.h"
#include "storage/generated_surface_records.h"
#include "ui/generated_surface.h"

namespace wb {
namespace {
bool same(const GeneratedSurfaceBinding& a, const GeneratedSurfaceBinding& b) {
    return a.thread_id == b.thread_id && a.run_id == b.run_id && a.event_seq == b.event_seq &&
           a.block_index == b.block_index && a.revision == b.revision;
}
GeneratedSurfaceBinding question_record_binding(const GeneratedQuestionBinding& binding) {
    return {binding.thread_id, binding.run_id, binding.revision, 0, 0};
}
size_t button_count(const json& components) {
    size_t count = 0;
    for (const auto& component : components) {
        const auto type = jstr(component, "type");
        if (type == "button") ++count;
        if (type == "section" || type == "card") count += button_count(component["components"]);
    }
    return count;
}
}  // namespace

Result<GeneratedSurfaceBinding> GeneratedSurfaceStateStore::bind_assistant(const std::string& thread_id, const std::string& run_id,
                                                                          long long event_seq, int block_index) {
    const std::lock_guard lock(db_.mutex());
    if (thread_id.empty() || run_id.empty() || event_seq <= 0 || block_index < 0 || block_index >= 16)
        return make_error("invalid_surface", "invalid surface identity");
    auto st = db_.prepare("SELECT e.payload FROM run_events e JOIN runs r ON r.id=e.run_id "
                          "WHERE r.thread_id=?1 AND r.id=?2 AND COALESCE(r.parent_run_id,'')='' "
                          "AND e.seq=?3 AND e.kind='message'");
    st.bind(1, thread_id).bind(2, run_id).bind(3, event_seq);
    if (!st.step()) return st.status() ? make_error("not_found", "surface event not found") : st.status().error();
    const auto payload = json::parse(st.col_text(0), nullptr, false);
    if (payload.is_discarded() || jstr(payload, "role") != "assistant" || !jbool(payload, "surface_complete"))
        return make_error("invalid_surface", "surface event is not a completed assistant message");
    const auto content = jstr(payload, "content");
    const auto blocks = ui::discover_generated_surfaces(content);
    if (static_cast<size_t>(block_index) >= blocks.size()) return make_error("invalid_surface", "surface block not found");
    const auto& block = blocks[static_cast<size_t>(block_index)];
    return GeneratedSurfaceBinding{thread_id, run_id, hex_encode(sha256(std::string_view(content).substr(block.begin, block.end - block.begin))),
                                   event_seq, block_index};
}

Result<std::optional<json>> GeneratedSurfaceStateStore::load(const GeneratedSurfaceBinding& binding) {
    const std::lock_guard lock(db_.mutex());
    const auto current = bind_assistant(binding.thread_id, binding.run_id, binding.event_seq, binding.block_index);
    if (!current || !same(binding, *current)) return make_error("stale_surface", "surface revision is no longer available");
    auto record = read_generated_surface_record(db_, generated_surface_record_id("state", binding));
    if (!record) return record.error();
    if (!record->has_value() || jstr(**record, "revision") != binding.revision) return std::optional<json>{};
    const auto* state = jobj(**record, "state");
    if (!state || !state->is_object()) return make_error("invalid_surface_state", "surface state is invalid");
    return std::optional<json>(*state);
}

Result<json> GeneratedSurfaceStateStore::assistant_surface(const GeneratedSurfaceBinding& binding) {
    const std::lock_guard lock(db_.mutex());
    const auto current = bind_assistant(binding.thread_id, binding.run_id, binding.event_seq, binding.block_index);
    if (!current || !same(binding, *current)) return make_error("stale_surface", "surface revision is no longer available");
    auto st = db_.prepare("SELECT payload FROM run_events WHERE run_id=?1 AND seq=?2");
    st.bind(1, binding.run_id).bind(2, binding.event_seq);
    if (!st.step()) return st.status() ? make_error("not_found", "surface event not found") : st.status().error();
    const auto payload = json::parse(st.col_text(0), nullptr, false);
    if (payload.is_discarded()) return make_error("invalid_surface", "surface event is invalid");
    const auto blocks = ui::discover_generated_surfaces(jstr(payload, "content"));
    if (static_cast<size_t>(binding.block_index) >= blocks.size()) return make_error("stale_surface", "surface block is unavailable");
    return blocks[static_cast<size_t>(binding.block_index)].surface;
}

Status GeneratedSurfaceStateStore::save(const GeneratedSurfaceBinding& binding, const json& state) {
    if (!state.is_object()) return make_error("invalid_surface_state", "surface state must be an object");
    return db_.transaction([&]() -> Status {
        const auto current = bind_assistant(binding.thread_id, binding.run_id, binding.event_seq, binding.block_index);
        if (!current || !same(binding, *current)) return make_error("stale_surface", "surface revision is no longer available");
        return write_generated_surface_record(db_, generated_surface_record_id("state", binding), binding,
                                              {{"version", 1}, {"revision", binding.revision}, {"state", state}});
    });
}

Result<GeneratedQuestionBinding> GeneratedSurfaceStateStore::bind_question(const std::string& thread_id, const std::string& run_id,
                                                                           const std::string& action_id, const std::string& revision,
                                                                           const std::string& token) {
    const std::lock_guard lock(db_.mutex());
    if (thread_id.empty() || run_id.empty() || action_id.empty() || revision.empty() || token.empty())
        return make_error("invalid_surface", "invalid Question identity");
    auto row = db_.prepare("SELECT a.payload FROM action_items a JOIN runs r ON r.id=a.run_id "
                           "WHERE a.id=?1 AND a.thread_id=?2 AND a.run_id=?3 AND r.thread_id=?2 "
                           "AND a.kind='question' AND a.resolved_at='' AND r.state IN ('queued','running','interrupted')");
    row.bind(1, action_id).bind(2, thread_id).bind(3, run_id);
    if (!row.step()) return row.status() ? make_error("stale_surface", "Question is no longer open") : row.status().error();
    const auto payload = json::parse(row.col_text(0), nullptr, false);
    const auto* surface = jobj(payload, "surface");
    if (!surface || revision != jstr(payload, "surface_revision") || token != jstr(payload, "surface_token"))
        return make_error("stale_surface", "Question identity or revision does not match");
    const auto validated = ui::parse_generated_surface(surface->dump());
    if (!validated || hex_encode(sha256(validated->dump())) != revision || button_count((*validated)["components"]) != 1)
        return make_error("invalid_surface", "Question form is invalid");
    return GeneratedQuestionBinding{thread_id, run_id, action_id, revision, token};
}

Result<std::optional<json>> GeneratedSurfaceStateStore::load_question(const GeneratedQuestionBinding& binding) {
    const std::lock_guard lock(db_.mutex());
    auto current = bind_question(binding.thread_id, binding.run_id, binding.action_id, binding.revision, binding.token);
    if (!current) return current.error();
    const auto record_binding = question_record_binding(binding);
    auto record = read_generated_surface_record(db_, generated_surface_record_id("question_state", record_binding, binding.action_id));
    if (!record) return record.error();
    if (!record->has_value()) return std::optional<json>{};
    const auto* state = jobj(**record, "state");
    if (jstr(**record, "revision") != binding.revision || !state || !state->is_object())
        return make_error("invalid_surface_state", "Question state is invalid");
    return std::optional<json>(*state);
}

Status GeneratedSurfaceStateStore::save_question(const GeneratedQuestionBinding& binding, const json& state) {
    if (!state.is_object()) return make_error("invalid_surface_state", "Question state must be an object");
    return db_.transaction([&]() -> Status {
        auto current = bind_question(binding.thread_id, binding.run_id, binding.action_id, binding.revision, binding.token);
        if (!current) return current.error();
        const auto record_binding = question_record_binding(binding);
        return write_generated_surface_record(db_, generated_surface_record_id("question_state", record_binding, binding.action_id),
                                              record_binding, {{"version", 1}, {"revision", binding.revision}, {"state", state}});
    });
}

}  // namespace wb
