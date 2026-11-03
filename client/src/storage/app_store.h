// app.db access shared by the foundation modules: settings, secrets (encrypted at rest by the database key),
// security events and the control-channel outbox.
#pragma once

#include "base/result.h"
#include "storage/database.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wb {

// All app.db migrations, in version order. Each roadmap change appends its own (see schema.cpp).
const std::vector<Migration>& app_migrations();

class AppStore {
public:
    explicit AppStore(std::unique_ptr<Database> db) : db_(std::move(db)) {}

    [[nodiscard]] Database& db() { return *db_; }
    [[nodiscard]] bool read_only() const { return db_->read_only(); }

    std::optional<std::string> setting(const std::string& key);
    Status set_setting(const std::string& key, const std::string& value);
    Status delete_setting(const std::string& key);

    std::optional<std::string> secret(const std::string& name);
    Status set_secret(const std::string& name, const std::string& value);
    Status delete_secret(const std::string& name);

    // Security events (state mismatch, subject mismatch, forged messages...). Detail must not contain secrets.
    Status security_event(const std::string& kind, const std::string& detail);
    struct SecurityEvent {
        long long id;
        std::string at, kind, detail;
    };
    std::vector<SecurityEvent> security_events(int limit = 100);

    // Control-channel outbox (client/control-channel Client Messages Over The Channel).
    struct OutboxItem {
        long long id;
        std::string type;
        long long ref;
        std::string payload;  // JSON
    };
    Status outbox_push(const std::string& type, long long ref, const std::string& payload_json);
    std::vector<OutboxItem> outbox_pending(int limit = 100);
    Status outbox_remove(long long id);

private:
    std::unique_ptr<Database> db_;
};

}  // namespace wb
