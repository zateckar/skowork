#include "storage/generated_surface_records.h"

#include "base/time.h"
#include "platform/crypto.h"
#include "platform/dpapi.h"

namespace wb {
namespace {
constexpr size_t kMaximumRecord = 1024 * 1024;

std::wstring purpose(const std::string& id) {
    return L"generated-surface-v1/" + std::wstring(id.begin(), id.end());
}
}  // namespace

std::string generated_surface_record_id(const char* kind, const GeneratedSurfaceBinding& binding, const std::string& action) {
    const json identity = json::array({kind, binding.thread_id, binding.run_id, binding.event_seq, binding.block_index,
                                       action.empty() ? std::string{} : binding.revision, action});
    return std::string("gs_") + kind + "_" + hex_encode(sha256(identity.dump()));
}

Result<std::optional<json>> read_generated_surface_record(Database& db, const std::string& id) {
    if (!id.starts_with("gs_") || id.size() > 100) return make_error("invalid_surface", "invalid internal record");
    auto row = db.prepare("SELECT content FROM thread_attachments WHERE id=?1");
    row.bind(1, id);
    if (!row.step()) {
        if (!row.status()) return row.status().error();
        return std::optional<json>{};
    }
    const auto sealed = row.col_blob(0);
    if (sealed.size() > kMaximumRecord + 4096) return make_error("invalid_surface_state", "surface record exceeds its limit");
    auto plain = dpapi_unprotect(sealed, purpose(id));
    if (!plain) return plain.error();
    if (plain->size() > kMaximumRecord) return make_error("invalid_surface_state", "surface record exceeds its limit");
    auto payload = json::parse(plain->begin(), plain->end(), nullptr, false);
    if (payload.is_discarded() || !payload.is_object() || jint(payload, "version") != 1)
        return make_error("invalid_surface_state", "unsupported surface record");
    return std::optional<json>(std::move(payload));
}

Status write_generated_surface_record(Database& db, const std::string& id, const GeneratedSurfaceBinding& binding,
                                      const json& payload) {
    if (!id.starts_with("gs_") || id.size() > 100 || !payload.is_object() || jint(payload, "version") != 1)
        return make_error("invalid_surface_state", "invalid surface record");
    const auto text = payload.dump();
    if (text.size() > kMaximumRecord) return make_error("invalid_surface_state", "surface record exceeds its limit");
    const bytes plain(text.begin(), text.end());
    auto sealed = dpapi_protect(plain, purpose(id));
    if (!sealed) return sealed.error();
    auto row = db.prepare("INSERT INTO thread_attachments(id,thread_id,run_id,call_id,name,size,provenance,content,created_at) "
                          "VALUES(?1,?2,?3,'generated_surface','internal',?4,'{}',?5,?6) "
                          "ON CONFLICT(id) DO UPDATE SET size=excluded.size,content=excluded.content "
                          "WHERE thread_attachments.thread_id=excluded.thread_id AND thread_attachments.run_id=excluded.run_id "
                          "AND thread_attachments.call_id='generated_surface'");
    row.bind(1, id).bind(2, binding.thread_id).bind(3, binding.run_id).bind(4, static_cast<long long>(sealed->size()))
        .bind_blob(5, *sealed).bind(6, iso8601_utc_now());
    if (auto result = row.run(); !result) return result;
    if (db.changes() != 1) return make_error("invalid_surface_state", "surface record identity mismatch");
    return {};
}

}  // namespace wb
