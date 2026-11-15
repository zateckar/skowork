#include "storage/generated_surface_submission.h"

#include "base/time.h"
#include "platform/crypto.h"
#include "storage/generated_surface_records.h"
#include "ui/generated_values.h"

namespace wb {
namespace {

const json* button_with_id(const json& components, const std::string& id) {
    for (const auto& component : components) {
        const auto type = jstr(component, "type");
        if (type == "button" && jstr(component, "id") == id) return &component;
        if (type == "section" || type == "card") {
            if (const auto* found = button_with_id(component["components"], id)) return found;
        }
    }
    return nullptr;
}

std::string message_for(const json& button, const std::vector<ui::GeneratedFieldValue>& fields) {
    std::string text = jstr(button, "label");
    for (const auto& field : fields) text += "\n" + field.label + ": " + field.value.dump();
    return text;
}
}  // namespace

Result<bool> GeneratedSurfaceSubmissionStore::accepted(const GeneratedSurfaceBinding& binding) {
    const std::lock_guard lock(db_.mutex());
    const auto surface = states_.assistant_surface(binding);
    if (!surface) return surface.error();
    auto receipt = read_generated_surface_record(db_, generated_surface_record_id("acceptance", binding, "surface"));
    if (!receipt) return receipt.error();
    if (!receipt->has_value()) return false;
    if (jstr(**receipt, "button_id").empty() || jint(**receipt, "queued_id") <= 0)
        return make_error("invalid_surface_action", "invalid surface acceptance record");
    return true;
}

Result<std::string> GeneratedSurfaceSubmissionStore::prepare(const GeneratedSurfaceBinding& binding, const std::string& button_id) {
    std::string result;
    const auto id = generated_surface_record_id("action", binding, button_id);
    const auto token = random_urlsafe(24);
    if (token.empty()) return make_error("crypto", "cannot generate surface action token");
    auto status = db_.transaction([&]() -> Status {
        const auto surface = states_.assistant_surface(binding);
        if (!surface) return surface.error();
        if (!button_with_id((*surface)["components"], button_id)) return make_error("invalid_surface_action", "surface button not found");
        auto existing = read_generated_surface_record(db_, id);
        if (!existing) return existing.error();
        if (existing->has_value()) {
            const auto saved_token = jstr(**existing, "token");
            if (saved_token.empty()) return make_error("invalid_surface_action", "invalid surface ticket record");
            result = id + "." + saved_token;
            return {};
        }
        const json record = {{"version", 1}, {"thread_id", binding.thread_id}, {"run_id", binding.run_id},
                             {"event_seq", binding.event_seq}, {"block_index", binding.block_index},
                             {"revision", binding.revision}, {"button_id", button_id}, {"token", token}, {"queued_id", nullptr}};
        if (auto s = write_generated_surface_record(db_, id, binding, record); !s) return s;
        result = id + "." + token;
        return {};
    });
    if (!status) return status.error();
    return result;
}

Result<long long> GeneratedSurfaceSubmissionStore::submit(const std::string& ticket, const json& draft, const std::string& mode, bool online,
                                                         const json& sections) {
    const auto dot = ticket.find('.');
    if (ticket.size() > 160 || dot == std::string::npos || dot + 1 == ticket.size())
        return make_error("invalid_surface_action", "invalid surface ticket");
    const auto id = ticket.substr(0, dot), token = ticket.substr(dot + 1);
    if (!id.starts_with("gs_action_")) return make_error("invalid_surface_action", "invalid surface ticket");
    long long queued_id = 0;
    auto status = db_.transaction([&]() -> Status {
        auto saved = read_generated_surface_record(db_, id);
        if (!saved) return saved.error();
        if (!saved->has_value()) return make_error("not_found", "surface ticket not found");
        auto& record = **saved;
        const auto index = jint(record, "block_index", -1);
        if (index < 0 || index >= 16 || token != jstr(record, "token"))
            return make_error("invalid_surface_action", "invalid surface ticket");
        GeneratedSurfaceBinding binding{jstr(record, "thread_id"), jstr(record, "run_id"), jstr(record, "revision"),
                                        jint(record, "event_seq"), static_cast<int>(index)};
        const auto button_id = jstr(record, "button_id");
        if (generated_surface_record_id("action", binding, button_id) != id)
            return make_error("invalid_surface_action", "surface ticket identity mismatch");
        const auto surface = states_.assistant_surface(binding);
        if (!surface) return surface.error();
        const json* button = button_with_id((*surface)["components"], button_id);
        if (!button) return make_error("invalid_surface_action", "surface button not found");
        const auto receipt_id = generated_surface_record_id("acceptance", binding, "surface");
        auto receipt = read_generated_surface_record(db_, receipt_id);
        if (!receipt) return receipt.error();
        if (receipt->has_value()) {
            if (jstr(**receipt, "button_id") != button_id)
                return make_error("already_submitted", "this surface has already been submitted");
            queued_id = jint(**receipt, "queued_id");
            if (queued_id <= 0) return make_error("invalid_surface_action", "invalid surface acceptance record");
            return {};
        }
        if (record.contains("queued_id") && record["queued_id"].is_number_integer()) {
            queued_id = jint(record, "queued_id");
            if (queued_id <= 0) return make_error("invalid_surface_action", "invalid accepted surface ticket");
            return {};
        }
        if (!online) return make_error("offline", "surface submission is unavailable while offline");
        if (mode != "queue" && mode != "steer_now") return make_error("invalid_surface_action", "invalid delivery mode");
        if (!ui::generated_surface_sections_allow_action(*surface, sections, button_id))
            return make_error("invalid_surface_action", "the surface controls must be visible");
        const auto fields = ui::generated_surface_values(*surface, draft);
        if (!fields) return make_error("invalid_surface_values", "surface values are invalid");
        const auto text = message_for(*button, *fields);
        auto queue = db_.prepare("INSERT INTO queued_messages(thread_id,text,mode,created_at) VALUES(?1,?2,?3,?4)");
        queue.bind(1, binding.thread_id).bind(2, text).bind(3, mode).bind(4, iso8601_utc_now());
        if (auto s = queue.run(); !s) return s;
        queued_id = db_.last_insert_id();
        const json receipt_value = {{"version", 1}, {"button_id", button_id}, {"queued_id", queued_id}};
        if (auto s = write_generated_surface_record(db_, receipt_id, binding, receipt_value); !s) return s;
        record["queued_id"] = queued_id;
        return write_generated_surface_record(db_, id, binding, record);
    });
    if (!status) return status.error();
    return queued_id;
}

}  // namespace wb
