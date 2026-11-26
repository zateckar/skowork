#include "eval/decision_log.h"

#include "storage/database.h"

#include <mutex>

namespace wb::eval {
namespace {

// A value whose serialized form is over the limit is kept as a shortened string of that serialization.
std::string bounded(const json& v) {
    std::string s = v.dump(-1, ' ', false, json::error_handler_t::replace);
    if (s.size() <= kDecisionValueLimit) return s;
    std::string cut = s.substr(0, kDecisionValueLimit - 16);
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();  // whole characters
    if (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0xC0) cut.pop_back();
    return json(cut + "\xE2\x80\xA6").dump(-1, ' ', false, json::error_handler_t::replace);
}

json parse_or_empty(const std::string& s) {
    auto j = parse_json(s);
    return j ? *j : json::object();
}

std::vector<Decision> read(Statement& st) {
    std::vector<Decision> out;
    while (st.step()) {
        Decision d;
        d.at_ms = st.col_int(0);
        d.step = st.col_text(1);
        d.kind = st.col_text(2);
        d.item_ref = st.col_text(3);
        d.result_ref = st.col_text(4);
        d.before = parse_or_empty(st.col_text(5));
        d.after = parse_or_empty(st.col_text(6));
        d.instruction_id = st.col_text(7);
        out.push_back(std::move(d));
    }
    return out;
}

constexpr const char* kColumns = "SELECT at, step, kind, item_ref, result_ref, before_json, after_json, instruction_id FROM decision_log ";

}  // namespace

Status DecisionLog::record(const Decision& d) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO decision_log(at, step, kind, item_ref, result_ref, before_json, after_json, instruction_id) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    return st.bind(1, d.at_ms).bind(2, d.step).bind(3, d.kind).bind(4, d.item_ref).bind(5, d.result_ref)
        .bind(6, bounded(d.before)).bind(7, bounded(d.after)).bind(8, d.instruction_id.empty() ? "unknown" : d.instruction_id)
        .run();
}

std::vector<Decision> DecisionLog::for_item(const std::string& item_ref) const {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(std::string(kColumns) + "WHERE item_ref=?1 ORDER BY at, id");
    st.bind(1, item_ref);
    return read(st);
}

std::vector<Decision> DecisionLog::between(long long from_ms, long long to_ms) const {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare(std::string(kColumns) + "WHERE at BETWEEN ?1 AND ?2 ORDER BY at, id");
    st.bind(1, from_ms).bind(2, to_ms);
    return read(st);
}

Status DecisionLog::forget_item(const std::string& item_ref) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM decision_log WHERE item_ref=?1 OR substr(result_ref, 1, length(?1) + 1)=?1 || '/' OR "
                          "(step='agenda' AND json_valid(before_json) AND "
                          "EXISTS (SELECT 1 FROM json_each(before_json, '$.sources') WHERE value=?1))");
    return st.bind(1, item_ref).run();
}

Status DecisionLog::forget_source(const std::string& kind) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM decision_log WHERE substr(item_ref, 1, length(?1) + 1)=?1 || ':'");
    return st.bind(1, kind).run();
}

Status DecisionLog::forget_step(const std::string& step) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM decision_log WHERE step=?1");
    return st.bind(1, step).run();
}

Status DecisionLog::forget_name(const std::string& name) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM decision_log WHERE step='knowledge' AND json_valid(before_json) AND "
                          "(json_extract(before_json, '$.subject')=?1 OR json_extract(before_json, '$.object')=?1)");
    return st.bind(1, name).run();
}

int DecisionLog::prune(long long now_ms) {
    const std::lock_guard lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM decision_log WHERE at < ?1");
    if (!st.bind(1, now_ms - kDecisionKeepMs).run()) return 0;
    return db_.changes();
}

std::string item_ref_mail(const std::string& id) { return "mail:" + id; }
std::string item_ref_teams(const std::string& conversation, const std::string& message) {
    return "teams:" + conversation + "/" + message;
}
std::string item_ref_calendar(const std::string& id) { return "calendar:" + id; }
std::string item_ref_meeting(const std::string& id) { return "meeting:" + id; }

}  // namespace wb::eval
