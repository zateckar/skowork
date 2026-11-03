#include "storage/app_store.h"

#include "base/time.h"

namespace wb {

std::optional<std::string> AppStore::setting(const std::string& key) {
    auto st = db_->prepare("SELECT value FROM settings WHERE key=?1");
    st.bind(1, key);
    if (st.step()) return st.col_text(0);
    return std::nullopt;
}

Status AppStore::set_setting(const std::string& key, const std::string& value) {
    auto st = db_->prepare(
        "INSERT INTO settings(key,value,updated_at) VALUES(?1,?2,?3) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at");
    st.bind(1, key).bind(2, value).bind(3, iso8601_utc_now());
    return st.run();
}

Status AppStore::delete_setting(const std::string& key) {
    auto st = db_->prepare("DELETE FROM settings WHERE key=?1");
    st.bind(1, key);
    return st.run();
}

std::optional<std::string> AppStore::secret(const std::string& name) {
    auto st = db_->prepare("SELECT value FROM secrets WHERE name=?1");
    st.bind(1, name);
    if (st.step()) return st.col_text(0);
    return std::nullopt;
}

Status AppStore::set_secret(const std::string& name, const std::string& value) {
    auto st = db_->prepare(
        "INSERT INTO secrets(name,value,updated_at) VALUES(?1,?2,?3) "
        "ON CONFLICT(name) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at");
    st.bind(1, name).bind(2, value).bind(3, iso8601_utc_now());
    return st.run();
}

Status AppStore::delete_secret(const std::string& name) {
    auto st = db_->prepare("DELETE FROM secrets WHERE name=?1");
    st.bind(1, name);
    return st.run();
}

Status AppStore::security_event(const std::string& kind, const std::string& detail) {
    auto st = db_->prepare("INSERT INTO security_events(at,kind,detail) VALUES(?1,?2,?3)");
    st.bind(1, iso8601_utc_now()).bind(2, kind).bind(3, detail);
    return st.run();
}

std::vector<AppStore::SecurityEvent> AppStore::security_events(int limit) {
    std::vector<SecurityEvent> out;
    auto st = db_->prepare("SELECT id,at,kind,detail FROM security_events ORDER BY id DESC LIMIT ?1");
    st.bind(1, limit);
    while (st.step()) out.push_back({st.col_int(0), st.col_text(1), st.col_text(2), st.col_text(3)});
    return out;
}

Status AppStore::outbox_push(const std::string& type, long long ref, const std::string& payload_json) {
    auto st = db_->prepare("INSERT INTO channel_outbox(type,ref,payload,created_at) VALUES(?1,?2,?3,?4)");
    st.bind(1, type).bind(2, ref).bind(3, payload_json).bind(4, iso8601_utc_now());
    return st.run();
}

std::vector<AppStore::OutboxItem> AppStore::outbox_pending(int limit) {
    std::vector<OutboxItem> out;
    auto st = db_->prepare("SELECT id,type,ref,payload FROM channel_outbox ORDER BY id LIMIT ?1");
    st.bind(1, limit);
    while (st.step()) out.push_back({st.col_int(0), st.col_text(1), st.col_int(2), st.col_text(3)});
    return out;
}

Status AppStore::outbox_remove(long long id) {
    auto st = db_->prepare("DELETE FROM channel_outbox WHERE id=?1");
    st.bind(1, id);
    return st.run();
}

}  // namespace wb
