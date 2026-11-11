#include "meeting/meeting_store.h"

#include <algorithm>
#include <mutex>

namespace wb::meeting {
namespace {

constexpr const char* kColumns =
    "id,thread_id,state,source,app,title,event_id,series_id,event_link,attendees,detected_at,started_at,ended_at,pid,"
    "audio_dir,audio_deleted,user_started,language,error,action_id,run_id,segments,speakers,vocabulary";

MeetingRecord read(const Statement& s) {
    MeetingRecord m;
    m.id = s.col_text(0);
    m.thread_id = s.col_text(1);
    m.state = s.col_text(2);
    m.source = s.col_text(3);
    m.app = s.col_text(4);
    m.title = s.col_text(5);
    m.event_id = s.col_text(6);
    m.series_id = s.col_text(7);
    m.event_link = s.col_text(8);
    if (const auto a = parse_json(s.col_text(9)); a && a->is_array())
        for (const auto& x : *a)
            if (x.is_string()) m.attendees.push_back(x.get<std::string>());
    m.detected_at = s.col_int(10);
    m.started_at = s.col_int(11);
    m.ended_at = s.col_int(12);
    m.pid = static_cast<unsigned>(s.col_int(13));
    m.audio_dir = s.col_text(14);
    m.audio_deleted = s.col_int(15) != 0;
    m.user_started = s.col_int(16) != 0;
    m.language = s.col_text(17);
    m.error = s.col_text(18);
    m.action_id = s.col_text(19);
    m.run_id = s.col_text(20);
    if (auto seg = parse_json(s.col_text(21)); seg && seg->is_array()) m.segments = std::move(*seg);
    if (const auto sp = parse_json(s.col_text(22)); sp && sp->is_object())
        for (const auto& [k, v] : sp->items())
            if (v.is_string()) m.speakers[k] = v.get<std::string>();
    if (const auto v = parse_json(s.col_text(23)); v && v->is_array())
        for (const auto& x : *v)
            if (x.is_string()) m.vocabulary.push_back(x.get<std::string>());
    return m;
}

}  // namespace

Status MeetingStore::put(const MeetingRecord& m) {
    const std::lock_guard lock(db_.mutex());
    json attendees = json::array();
    for (const auto& a : m.attendees) attendees.push_back(a);
    json speakers = json::object();
    for (const auto& [k, v] : m.speakers) speakers[k] = v;
    json vocabulary = json::array();
    for (const auto& t : m.vocabulary) vocabulary.push_back(t);
    auto s = db_.prepare(std::string("INSERT OR REPLACE INTO meetings(") + kColumns +
                         ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24)");
    s.bind(1, m.id).bind(2, m.thread_id).bind(3, m.state).bind(4, m.source).bind(5, m.app).bind(6, m.title);
    s.bind(7, m.event_id).bind(8, m.series_id).bind(9, m.event_link).bind(10, attendees.dump()).bind(11, m.detected_at);
    s.bind(12, m.started_at).bind(13, m.ended_at).bind(14, static_cast<long long>(m.pid)).bind(15, m.audio_dir);
    s.bind(16, m.audio_deleted ? 1 : 0).bind(17, m.user_started ? 1 : 0).bind(18, m.language).bind(19, m.error);
    s.bind(20, m.action_id).bind(21, m.run_id).bind(22, m.segments.dump()).bind(23, speakers.dump());
    s.bind(24, vocabulary.dump());
    return s.run();
}

std::optional<MeetingRecord> MeetingStore::get(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kColumns + " FROM meetings WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read(s);
}

std::optional<MeetingRecord> MeetingStore::by_action(const std::string& action_id) {
    if (action_id.empty()) return std::nullopt;
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kColumns + " FROM meetings WHERE action_id=?1");
    s.bind(1, action_id);
    if (!s.step()) return std::nullopt;
    return read(s);
}

std::vector<MeetingRecord> MeetingStore::list(int limit) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kColumns + " FROM meetings ORDER BY detected_at DESC, id LIMIT ?1");
    s.bind(1, limit);
    std::vector<MeetingRecord> out;
    while (s.step()) out.push_back(read(s));
    return out;
}

std::vector<MeetingRecord> MeetingStore::in_states(const std::vector<std::string>& states) {
    std::vector<MeetingRecord> out;
    for (const auto& m : list(100000))
        for (const auto& st : states)
            if (m.state == st) out.push_back(m);
    std::sort(out.begin(), out.end(), [](const MeetingRecord& a, const MeetingRecord& b) { return a.ended_at < b.ended_at; });
    return out;
}

Status MeetingStore::remove(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM meetings WHERE id=?1");
    s.bind(1, id);
    return s.run();
}

Status MeetingStore::add_series(const std::string& series_id, const std::string& title, long long now) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("INSERT OR REPLACE INTO meeting_series(series_id, title, created_at) VALUES(?1,?2,?3)");
    s.bind(1, series_id).bind(2, title).bind(3, now);
    return s.run();
}

Status MeetingStore::remove_series(const std::string& series_id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM meeting_series WHERE series_id=?1");
    s.bind(1, series_id);
    return s.run();
}

bool MeetingStore::has_series(const std::string& series_id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT 1 FROM meeting_series WHERE series_id=?1");
    s.bind(1, series_id);
    return s.step();
}

std::vector<std::pair<std::string, std::string>> MeetingStore::series() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT series_id, title FROM meeting_series ORDER BY created_at, series_id");
    std::vector<std::pair<std::string, std::string>> out;
    while (s.step()) out.emplace_back(s.col_text(0), s.col_text(1));
    return out;
}

std::vector<std::string> MeetingStore::terms() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT term FROM meeting_terms ORDER BY added_at, term");
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

Status MeetingStore::add_term(const std::string& term, long long now) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("INSERT OR IGNORE INTO meeting_terms(term, added_at) VALUES(?1,?2)");
    s.bind(1, term).bind(2, now);
    return s.run();
}

Status MeetingStore::remove_term(const std::string& term) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM meeting_terms WHERE term=?1");
    s.bind(1, term);
    return s.run();
}

}  // namespace wb::meeting
