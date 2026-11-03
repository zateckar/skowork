#include "mail/mail_store.h"

#include <cctype>
#include <mutex>

namespace wb::mail {
namespace {

json people_json(const std::vector<Person>& p) {
    json a = json::array();
    for (const auto& x : p) a.push_back({{"name", x.name}, {"address", x.address}});
    return a;
}

std::vector<Person> people_of(const std::string& text) {
    std::vector<Person> out;
    const auto j = parse_json(text);
    if (j && j->is_array())
        for (const auto& x : *j) out.push_back({jstr(x, "name"), jstr(x, "address")});
    return out;
}

json strings_json(const std::vector<std::string>& v) {
    json a = json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

std::vector<std::string> strings_of(const std::string& text) {
    std::vector<std::string> out;
    const auto j = parse_json(text);
    if (j && j->is_array())
        for (const auto& x : *j)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

// Search terms as FTS5 prefix queries: letters and digits only, each term quoted.
std::string fts_query(const std::string& q) {
    std::string out, term;
    const auto flush = [&] {
        if (term.empty()) return;
        if (!out.empty()) out += ' ';
        out += '"' + term + "\"*";
        term.clear();
    };
    for (const char c : q) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u)) term += c;
        else flush();
    }
    flush();
    return out;
}

constexpr const char* kMessageColumns =
    "id,folder_id,conversation_id,change_key,received_at,subject,from_name,from_address,to_json,cc_json,body,preview,"
    "has_attachments,is_read,is_draft,categories,label,withheld,web_link";

StoredMessage read_message(const Statement& s) {
    StoredMessage m;
    m.id = s.col_text(0);
    m.folder_id = s.col_text(1);
    m.conversation_id = s.col_text(2);
    m.change_key = s.col_text(3);
    m.received_at = s.col_int(4);
    m.subject = s.col_text(5);
    m.from = {s.col_text(6), s.col_text(7)};
    m.to = people_of(s.col_text(8));
    m.cc = people_of(s.col_text(9));
    m.body = s.col_text(10);
    m.preview = s.col_text(11);
    m.has_attachments = s.col_int(12) != 0;
    m.is_read = s.col_int(13) != 0;
    m.is_draft = s.col_int(14) != 0;
    m.categories = strings_of(s.col_text(15));
    m.label = s.col_text(16);
    m.withheld = s.col_int(17) != 0;
    m.web_link = s.col_text(18);
    return m;
}

constexpr const char* kEventColumns =
    "id,ical_uid,series_master_id,change_key,start_at,end_at,all_day,subject,body,location,organizer_json,attendees_json,"
    "is_organizer,cancelled,response,show_as,sensitivity,masked,categories,label,withheld,web_link,online_url";

StoredEvent read_event(const Statement& s) {
    StoredEvent e;
    e.id = s.col_text(0);
    e.ical_uid = s.col_text(1);
    e.series_master_id = s.col_text(2);
    e.change_key = s.col_text(3);
    e.start_at = s.col_int(4);
    e.end_at = s.col_int(5);
    e.all_day = s.col_int(6) != 0;
    e.subject = s.col_text(7);
    e.body = s.col_text(8);
    e.location = s.col_text(9);
    if (const auto o = parse_json(s.col_text(10)); o && o->is_object()) e.organizer = {jstr(*o, "name"), jstr(*o, "address")};
    if (auto a = parse_json(s.col_text(11)); a && a->is_array()) e.attendees = std::move(*a);
    e.is_organizer = s.col_int(12) != 0;
    e.cancelled = s.col_int(13) != 0;
    e.response = s.col_text(14);
    e.show_as = s.col_text(15);
    e.sensitivity = s.col_text(16);
    e.masked = s.col_int(17) != 0;
    e.categories = strings_of(s.col_text(18));
    e.label = s.col_text(19);
    e.withheld = s.col_int(20) != 0;
    e.web_link = s.col_text(21);
    e.online_url = s.col_text(22);
    return e;
}

}  // namespace

MailStore::Change MailStore::upsert_message(const StoredMessage& m) {
    const std::lock_guard lock(db_.mutex());
    Change change = Change::created;
    {
        auto q = db_.prepare("SELECT change_key, folder_id FROM mail_messages WHERE id=?1");
        q.bind(1, m.id);
        if (q.step()) {
            if (q.col_text(0) == m.change_key && q.col_text(1) == m.folder_id && !m.change_key.empty()) return Change::none;
            change = Change::updated;
        }
    }
    (void)db_.transaction([&]() -> Status {
        auto s = db_.prepare(std::string("INSERT OR REPLACE INTO mail_messages(") + kMessageColumns +
                             ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)");
        s.bind(1, m.id).bind(2, m.folder_id).bind(3, m.conversation_id).bind(4, m.change_key).bind(5, m.received_at);
        s.bind(6, m.subject).bind(7, m.from.name).bind(8, m.from.address).bind(9, people_json(m.to).dump()).bind(10, people_json(m.cc).dump());
        s.bind(11, m.body).bind(12, m.preview).bind(13, m.has_attachments ? 1 : 0).bind(14, m.is_read ? 1 : 0).bind(15, m.is_draft ? 1 : 0);
        s.bind(16, strings_json(m.categories).dump()).bind(17, m.label).bind(18, m.withheld ? 1 : 0).bind(19, m.web_link);
        if (Status st = s.run(); !st) return st;
        auto d = db_.prepare("DELETE FROM mail_search WHERE message_id=?1");
        d.bind(1, m.id);
        if (Status st = d.run(); !st) return st;
        if (m.withheld) return {};  // never indexed
        std::string people = m.from.name + " " + m.from.address;
        for (const auto& p : m.to) people += " " + p.name + " " + p.address;
        for (const auto& p : m.cc) people += " " + p.name + " " + p.address;
        auto i = db_.prepare("INSERT INTO mail_search(subject, body, people, message_id) VALUES(?1,?2,?3,?4)");
        i.bind(1, m.subject).bind(2, m.body).bind(3, people).bind(4, m.id);
        return i.run();
    });
    return change;
}

std::optional<StoredMessage> MailStore::remove_message(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto m = message(id);
    if (!m) return std::nullopt;
    (void)db_.transaction([&]() -> Status {
        auto s = db_.prepare("DELETE FROM mail_messages WHERE id=?1");
        s.bind(1, id);
        if (Status st = s.run(); !st) return st;
        auto d = db_.prepare("DELETE FROM mail_search WHERE message_id=?1");
        d.bind(1, id);
        return d.run();
    });
    return m;
}

std::optional<StoredMessage> MailStore::message(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kMessageColumns + " FROM mail_messages WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_message(s);
}

std::vector<StoredMessage> MailStore::search(const SearchQuery& q) {
    const std::lock_guard lock(db_.mutex());
    std::string sql = std::string("SELECT ") + kMessageColumns + " FROM mail_messages m WHERE 1=1";
    const std::string fts = fts_query(q.text);
    if (!fts.empty()) sql += " AND (m.withheld=0 AND m.id IN (SELECT message_id FROM mail_search WHERE mail_search MATCH ?1))";
    if (!q.folder_id.empty()) sql += " AND m.folder_id=?2";
    if (!q.from.empty()) sql += " AND (lower(m.from_address) LIKE ?3 OR lower(m.from_name) LIKE ?3)";
    if (q.since > 0) sql += " AND m.received_at>=?4";
    if (q.until > 0) sql += " AND m.received_at<?5";
    if (q.has_attachments) sql += " AND m.has_attachments=?6";
    sql += " ORDER BY m.received_at DESC LIMIT ?7";
    auto s = db_.prepare(sql);
    if (!fts.empty()) s.bind(1, fts);
    if (!q.folder_id.empty()) s.bind(2, q.folder_id);
    if (!q.from.empty()) {
        std::string f;
        for (const char c : q.from) f += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        s.bind(3, "%" + f + "%");
    }
    if (q.since > 0) s.bind(4, q.since);
    if (q.until > 0) s.bind(5, q.until);
    if (q.has_attachments) s.bind(6, *q.has_attachments ? 1 : 0);
    s.bind(7, q.limit);
    std::vector<StoredMessage> out;
    while (s.step()) out.push_back(read_message(s));
    return out;
}

std::vector<std::string> MailStore::message_ids(const std::string& folder_id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT id FROM mail_messages WHERE folder_id=?1");
    s.bind(1, folder_id);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

MailStore::Change MailStore::upsert_event(const StoredEvent& e) {
    const std::lock_guard lock(db_.mutex());
    Change change = Change::created;
    {
        auto q = db_.prepare("SELECT change_key, start_at, end_at, cancelled FROM cal_events WHERE id=?1");
        q.bind(1, e.id);
        if (q.step()) {
            if (q.col_text(0) == e.change_key && !e.change_key.empty() && q.col_int(1) == e.start_at && q.col_int(2) == e.end_at &&
                (q.col_int(3) != 0) == e.cancelled)
                return Change::none;
            change = Change::updated;
        }
    }
    auto s = db_.prepare(std::string("INSERT OR REPLACE INTO cal_events(") + kEventColumns +
                         ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23)");
    s.bind(1, e.id).bind(2, e.ical_uid).bind(3, e.series_master_id).bind(4, e.change_key).bind(5, e.start_at).bind(6, e.end_at);
    s.bind(7, e.all_day ? 1 : 0).bind(8, e.subject).bind(9, e.body).bind(10, e.location);
    s.bind(11, json{{"name", e.organizer.name}, {"address", e.organizer.address}}.dump()).bind(12, e.attendees.dump());
    s.bind(13, e.is_organizer ? 1 : 0).bind(14, e.cancelled ? 1 : 0).bind(15, e.response).bind(16, e.show_as).bind(17, e.sensitivity);
    s.bind(18, e.masked ? 1 : 0).bind(19, strings_json(e.categories).dump()).bind(20, e.label).bind(21, e.withheld ? 1 : 0).bind(22, e.web_link).bind(23, e.online_url);
    (void)s.run();
    return change;
}

std::optional<StoredEvent> MailStore::remove_event(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto e = event(id);
    if (!e) return std::nullopt;
    auto s = db_.prepare("DELETE FROM cal_events WHERE id=?1");
    s.bind(1, id);
    (void)s.run();
    return e;
}

std::optional<StoredEvent> MailStore::event(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kEventColumns + " FROM cal_events WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_event(s);
}

std::vector<StoredEvent> MailStore::events_between(long long from, long long to) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kEventColumns + " FROM cal_events WHERE end_at>?1 AND start_at<?2 ORDER BY start_at");
    s.bind(1, from).bind(2, to);
    std::vector<StoredEvent> out;
    while (s.step()) out.push_back(read_event(s));
    return out;
}

std::vector<std::string> MailStore::all_message_ids() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT id FROM mail_messages");
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

std::vector<std::string> MailStore::event_ids() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT id FROM cal_events");
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

void MailStore::set_folders(const std::vector<StoredFolder>& folders) {
    const std::lock_guard lock(db_.mutex());
    (void)db_.transaction([&]() -> Status {
        if (Status st = db_.exec("DELETE FROM mail_folders"); !st) return st;
        for (const auto& f : folders) {
            auto s = db_.prepare("INSERT OR REPLACE INTO mail_folders(id, name, well_known) VALUES(?1,?2,?3)");
            s.bind(1, f.id).bind(2, f.name).bind(3, f.well_known);
            if (Status st = s.run(); !st) return st;
        }
        return {};
    });
}

std::vector<StoredFolder> MailStore::folders() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT id, name, well_known FROM mail_folders ORDER BY name");
    std::vector<StoredFolder> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_text(2)});
    return out;
}

std::optional<StoredFolder> MailStore::folder(const std::string& id_or_name) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT id, name, well_known FROM mail_folders WHERE id=?1 OR lower(name)=lower(?1) OR lower(well_known)=lower(?1)");
    s.bind(1, id_or_name);
    if (!s.step()) return std::nullopt;
    return StoredFolder{s.col_text(0), s.col_text(1), s.col_text(2)};
}

std::string MailStore::delta_link(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT delta_link FROM mail_sync_state WHERE key=?1");
    s.bind(1, key);
    return s.step() ? s.col_text(0) : std::string{};
}

std::string MailStore::sync_extra(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT extra FROM mail_sync_state WHERE key=?1");
    s.bind(1, key);
    return s.step() ? s.col_text(0) : std::string{};
}

void MailStore::set_delta_link(const std::string& key, const std::string& link, long long synced_at, const std::string& extra) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("INSERT OR REPLACE INTO mail_sync_state(key, delta_link, extra, synced_at) VALUES(?1,?2,?3,?4)");
    s.bind(1, key).bind(2, link).bind(3, extra).bind(4, synced_at);
    (void)s.run();
}

void MailStore::drop_sync_state(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM mail_sync_state WHERE key=?1");
    s.bind(1, key);
    (void)s.run();
}

void MailStore::restart_sync() {
    const std::lock_guard lock(db_.mutex());
    (void)db_.exec("UPDATE mail_sync_state SET delta_link='', extra=CASE key WHEN 'calendar' THEN 'restart' ELSE extra END");
}

long long MailStore::last_sync() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT max(synced_at) FROM mail_sync_state");
    return s.step() && !s.col_null(0) ? s.col_int(0) : 0;
}

int MailStore::prune(long long mail_before, long long events_before, long long events_after) {
    const std::lock_guard lock(db_.mutex());
    int n = 0;
    (void)db_.transaction([&]() -> Status {
        auto a = db_.prepare("DELETE FROM mail_search WHERE message_id IN (SELECT id FROM mail_messages WHERE received_at<?1)");
        a.bind(1, mail_before);
        if (Status st = a.run(); !st) return st;
        auto b = db_.prepare("DELETE FROM mail_messages WHERE received_at<?1");
        b.bind(1, mail_before);
        if (Status st = b.run(); !st) return st;
        n += db_.changes();
        auto c = db_.prepare("DELETE FROM cal_events WHERE end_at<?1 OR start_at>?2");
        c.bind(1, events_before).bind(2, events_after);
        if (Status st = c.run(); !st) return st;
        n += db_.changes();
        return {};
    });
    return n;
}

std::vector<std::string> MailStore::user_exclusions(const std::string& kind) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT value FROM mail_user_exclusions WHERE kind=?1 ORDER BY value");
    s.bind(1, kind);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

Status MailStore::add_user_exclusion(const std::string& kind, const std::string& value) {
    if (kind != "folder" && kind != "category") return make_error("invalid_argument", "kind must be folder or category");
    if (value.empty()) return make_error("invalid_argument", "the value is empty");
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("INSERT OR IGNORE INTO mail_user_exclusions(kind, value) VALUES(?1,?2)");
    s.bind(1, kind).bind(2, value);
    return s.run();
}

Status MailStore::remove_user_exclusion(const std::string& kind, const std::string& value) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM mail_user_exclusions WHERE kind=?1 AND value=?2");
    s.bind(1, kind).bind(2, value);
    return s.run();
}

void MailStore::clear(bool all) {
    const std::lock_guard lock(db_.mutex());
    (void)db_.exec("DELETE FROM mail_messages; DELETE FROM mail_search; DELETE FROM cal_events; DELETE FROM mail_sync_state; DELETE FROM mail_folders;");
    if (all) (void)db_.exec("DELETE FROM mail_user_exclusions;");
}

MailStore::Counts MailStore::counts() {
    const std::lock_guard lock(db_.mutex());
    Counts c;
    auto s = db_.prepare("SELECT (SELECT count(*) FROM mail_messages), (SELECT count(*) FROM cal_events), "
                         "(SELECT count(*) FROM mail_messages WHERE withheld=1) + (SELECT count(*) FROM cal_events WHERE withheld=1)");
    if (s.step()) {
        c.messages = s.col_int(0);
        c.events = s.col_int(1);
        c.withheld = s.col_int(2);
    }
    return c;
}

}  // namespace wb::mail
