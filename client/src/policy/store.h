// The local approval log (client/policy-enforcement Decision Audit) and the user's folder grants (User Folder
// Grants), both in app.db.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "policy/core.h"
#include "storage/database.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wb::policy {

struct DecisionRecord {
    long long id = 0;
    std::string at;
    std::string kind;  // decision | resolution
    std::string run_id, call_id, tool, risk;
    std::string outcome;  // allow | approve | deny, or the resolution (approve, approve_edited, reject, respond)
    std::string rule, source;
    long long policy_version = 0;
    std::string reason;
};

class DecisionLog {
public:
    explicit DecisionLog(Database& db) : db_(db) {}
    Status record(const DecisionRecord& r);
    // Called after each recorded decision or resolution (client/telemetry policy decision records). Set once at start.
    void set_observer(std::function<void(const DecisionRecord&)> f) { observer_ = std::move(f); }
    [[nodiscard]] std::vector<DecisionRecord> recent(int limit = 100);
    // The decision record of a tool call ("why was this denied").
    [[nodiscard]] std::optional<DecisionRecord> for_call(const std::string& call_id);

private:
    Database& db_;
    std::function<void(const DecisionRecord&)> observer_;
};

class FolderGrants {
public:
    explicit FolderGrants(Database& db) : db_(db) {}
    // Grants a folder (a final path) if it lies within one of `grantable` (globs, environment variables expanded).
    // Refused with error "policy_denied" naming policy.filesystem.grantable otherwise.
    Status grant(const std::string& final_folder, bool write, const std::vector<std::string>& grantable);
    Status revoke(const std::string& final_folder);
    [[nodiscard]] std::vector<FolderGrant> list();

private:
    Database& db_;
};

json to_json(const DecisionRecord& r);

}  // namespace wb::policy
