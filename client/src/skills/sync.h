// Skill installation and lifecycle (client/skills Skill Sync, Skill Installation And Pinning, Skill Updates And
// Rollback, Skill Revocation; add-skills design D5, D8): the registry list is applied to the org, group and shared
// tiers on disk under policy.skills.install_from and policy.skills.auto_update, content is fetched by digest and
// checked, org Skills are checked against their registry signature when installed and again when loaded, the
// previous version is kept for rollback, and revocations remove Skills and stop the subagents running them.
//
// On disk, per registry Skill (<data dir>/<tier>/<name>/):
//   <version>/            the files, package.zip (the bytes the digest and signature cover) and install.json
//   pinned                the version in use (skills::Catalog reads it)
//   state.json            {previous, held, offer}: the version to roll back to, whether the user pinned it, a
//                         pending update when updates are not automatic
// and <data dir>/keys.json caches the registry's signing keys, <data dir>/usage.json the counts not yet reported.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "skills/catalog.h"
#include "skills/package.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wb::skills {

inline constexpr const char* kSkillSignatureTyp = "workbench-skill+jwt";

struct SyncEnv {
    std::filesystem::path data_dir;  // <data root>/skills
    // GET /api/v1/skills/sync: {skills: [{name, version, scope, digest, signature?, groups?, author, ...}], revocations}.
    std::function<Result<json>()> fetch_list;
    // GET /api/v1/skills/content/{digest}: the package bytes.
    std::function<Result<std::string>(const std::string& digest)> fetch_content;
    // GET /.well-known/workbench-skill-keys.json.
    std::function<Result<json>()> fetch_keys;
    // POST /api/v1/skills/usage with {skills: [{name, installs, runs, failures}]}.
    std::function<Status(const json& body)> post_usage;
    // Lower-case hex SHA-256.
    std::function<std::string(std::string_view bytes)> sha256_hex;
    // The payload of a compact JWS when its ES256 signature verifies with one of `keys` and its typ is
    // workbench-skill+jwt; an error otherwise.
    std::function<Result<json>(const std::string& jws, const json& keys)> verify_jws;
    // policy.skills.install_from (default [org, group, shared]) and policy.skills.auto_update (default true).
    std::function<std::vector<std::string>()> install_from;
    std::function<bool()> auto_update;
    std::function<void(const std::string& kind, const std::string& message)> security_event;
    // A Skill in use was removed (revoked): stop its subagents with this reason.
    std::function<void(const std::string& name, const std::string& reason)> stop_skill;
    // The tiers on disk changed: reload the catalogue.
    std::function<void()> changed;
    PackageLimits limits;
};

class SkillSync {
public:
    explicit SkillSync(SyncEnv env);

    // One full sync: reports the counts collected so far, applies the revocations, then installs, updates or offers
    // what the list holds. Returns {installed, updated, offered, removed, refused: [{name, version, reason}]}.
    Result<json> sync();
    // A skill-revoked push: {name, versions: [{version, digest}], reason}.
    void revoke(const std::string& name, const json& versions, const std::string& reason);
    // Rollback: the previous version is used again and pinned until update() (Skill Updates And Rollback).
    Status rollback(Tier tier, const std::string& name);
    // Ends a pin: installs the pending update, or returns to the highest installed version.
    Status update(Tier tier, const std::string& name);
    // Usage Reporting: a Skill subagent ended. Counted for registry Skills only.
    void record_run(const std::string& name, bool ok);
    // Load-time check of an installed copy (design D5): its package bytes match the recorded digest, every file on
    // disk is the package's, and for org Skills the registry signature covers the digest, name, version and scope.
    std::optional<ParseError> verify_installed(Tier tier, const std::filesystem::path& folder);

    // The pending updates and pins, for the Skills section: [{tier, name, current, previous, held, offer}].
    [[nodiscard]] json state();

private:
    struct Listed {
        std::string name, version, scope, digest, signature, author;
        std::vector<std::string> groups;
    };
    [[nodiscard]] bool allowed(const Listed& l) const;
    [[nodiscard]] bool tier_allowed(Tier tier, const std::vector<std::string>& groups) const;
    Status install(const Listed& l);
    Result<json> verify_signature(const std::string& jws, bool allow_fetch);
    void remove_version(Tier tier, const std::string& name, const std::string& version, const std::string& reason, json& removed);
    void count(const std::string& name, const char* field, int n);
    void flush_usage();

    SyncEnv env_;
    std::recursive_mutex mx_;
};

// Reads and writes a small JSON file; missing or unreadable: null.
json read_json_file(const std::filesystem::path& file);
bool write_file_atomic(const std::filesystem::path& file, std::string_view data);

}  // namespace wb::skills
