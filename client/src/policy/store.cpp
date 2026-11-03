#include "policy/store.h"

#include "base/time.h"

namespace wb::policy {

namespace {
DecisionRecord row(Statement& st) {
    DecisionRecord r;
    r.id = st.col_int(0);
    r.at = st.col_text(1);
    r.kind = st.col_text(2);
    r.run_id = st.col_text(3);
    r.call_id = st.col_text(4);
    r.tool = st.col_text(5);
    r.risk = st.col_text(6);
    r.outcome = st.col_text(7);
    r.rule = st.col_text(8);
    r.source = st.col_text(9);
    r.policy_version = st.col_int(10);
    r.reason = st.col_text(11);
    return r;
}
constexpr const char* kCols = "id,at,kind,run_id,call_id,tool,risk,outcome,rule,source,policy_version,reason";
}  // namespace

Status DecisionLog::record(const DecisionRecord& r) {
    auto st = db_.prepare(
        "INSERT INTO policy_decisions(at,kind,run_id,call_id,tool,risk,outcome,rule,source,policy_version,reason) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
    st.bind(1, r.at.empty() ? iso8601_utc_now() : r.at).bind(2, r.kind).bind(3, r.run_id).bind(4, r.call_id).bind(5, r.tool)
        .bind(6, r.risk).bind(7, r.outcome).bind(8, r.rule).bind(9, r.source).bind(10, r.policy_version).bind(11, r.reason);
    Status s = st.run();
    if (s && observer_) observer_(r);
    return s;
}

std::vector<DecisionRecord> DecisionLog::recent(int limit) {
    std::vector<DecisionRecord> out;
    auto st = db_.prepare(std::string("SELECT ") + kCols + " FROM policy_decisions ORDER BY id DESC LIMIT ?1");
    st.bind(1, limit);
    while (st.step()) out.push_back(row(st));
    return out;
}

std::optional<DecisionRecord> DecisionLog::for_call(const std::string& call_id) {
    auto st = db_.prepare(std::string("SELECT ") + kCols + " FROM policy_decisions WHERE call_id=?1 AND kind='decision' ORDER BY id DESC LIMIT 1");
    st.bind(1, call_id);
    if (st.step()) return row(st);
    return std::nullopt;
}

Status FolderGrants::grant(const std::string& folder, bool write, const std::vector<std::string>& grantable) {
    const std::string f = normalize_final_path(folder);
    bool within = false;
    for (const auto& g : grantable)
        if (glob_match(g, f) || glob_match(g, f + "\\")) within = true;
    if (!within)
        return make_error("policy_denied", "policy.filesystem.grantable: " + f + " is outside the folders your policy lets you grant");
    auto st = db_.prepare(
        "INSERT INTO folder_grants(folder,write,created_at) VALUES(?1,?2,?3) "
        "ON CONFLICT(folder) DO UPDATE SET write=excluded.write");
    st.bind(1, f).bind(2, write ? 1 : 0).bind(3, iso8601_utc_now());
    return st.run();
}

Status FolderGrants::revoke(const std::string& folder) {
    auto st = db_.prepare("DELETE FROM folder_grants WHERE folder=?1");
    st.bind(1, normalize_final_path(folder));
    return st.run();
}

std::vector<FolderGrant> FolderGrants::list() {
    std::vector<FolderGrant> out;
    auto st = db_.prepare("SELECT folder, write FROM folder_grants ORDER BY folder");
    while (st.step()) out.push_back({st.col_text(0), st.col_int(1) != 0});
    return out;
}

json to_json(const DecisionRecord& r) {
    return {{"id", r.id},     {"at", r.at},       {"kind", r.kind},     {"run_id", r.run_id}, {"call_id", r.call_id},
            {"tool", r.tool}, {"risk", r.risk},   {"outcome", r.outcome}, {"rule", r.rule},   {"source", r.source},
            {"policy_version", r.policy_version}, {"reason", r.reason}};
}

}  // namespace wb::policy
