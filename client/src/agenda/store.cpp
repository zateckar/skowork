#include "agenda/store.h"

#include <algorithm>
#include <mutex>

namespace wb::agenda {
namespace {

constexpr const char* kColumns =
    "id,title,action,due_at,has_time,inferred,state,draft_kind,user_edited,prep_thread_id,prep_run_id,prepared_at,"
    "refreshed,reminders,notice,created_at,updated_at,state_at";

Item read(const Statement& s) {
    Item it;
    it.id = s.col_text(0);
    it.title = s.col_text(1);
    it.action = s.col_text(2);
    it.due_at = s.col_int(3);
    it.has_time = s.col_int(4) != 0;
    it.inferred = s.col_int(5) != 0;
    it.state = s.col_text(6);
    it.draft_kind = s.col_text(7);
    it.user_edited = s.col_int(8) != 0;
    it.prep_thread_id = s.col_text(9);
    it.prep_run_id = s.col_text(10);
    it.prepared_at = s.col_int(11);
    it.refreshed = s.col_int(12) != 0;
    if (const auto r = parse_json(s.col_text(13)); r && r->is_array())
        for (const auto& x : *r)
            if (x.is_string()) it.reminders.push_back(x.get<std::string>());
    it.notice = s.col_text(14);
    it.created_at = s.col_int(15);
    it.updated_at = s.col_int(16);
    it.state_at = s.col_int(17);
    return it;
}

}  // namespace

json to_json(const SourceItem& it) {
    return {{"kind", it.kind},     {"id", it.id},           {"conversation", it.conversation},
            {"title", it.title},   {"text", it.text},       {"author", it.author},
            {"author_is_user", it.author_is_user},          {"user_addressed", it.user_addressed},
            {"recipients", it.recipients},                  {"distribution_list", it.distribution_list},
            {"at", it.at_ms},      {"link", it.link}};
}

std::optional<SourceItem> source_item_from(const json& j) {
    if (!j.is_object()) return std::nullopt;
    SourceItem it;
    it.kind = jstr(j, "kind");
    it.id = jstr(j, "id");
    if (it.kind.empty() || it.id.empty()) return std::nullopt;
    it.conversation = jstr(j, "conversation");
    it.title = jstr(j, "title");
    it.text = jstr(j, "text");
    it.author = jstr(j, "author");
    it.author_is_user = jbool(j, "author_is_user");
    it.user_addressed = jbool(j, "user_addressed");
    it.recipients = static_cast<int>(jint(j, "recipients"));
    it.distribution_list = jbool(j, "distribution_list");
    it.at_ms = jint(j, "at");
    it.link = jstr(j, "link");
    return it;
}

json to_json(const Item& it) {
    json sources = json::array();
    for (const auto& s : it.sources)
        sources.push_back({{"kind", s.kind}, {"id", s.source_id}, {"title", s.title}, {"quote", s.quote}, {"link", s.link}, {"at", s.at}});
    return {{"id", it.id},
            {"title", it.title},
            {"action", it.action},
            {"due_at", it.due_at},
            {"has_time", it.has_time},
            {"inferred", it.inferred},
            {"state", it.state},
            {"draft_kind", it.draft_kind},
            {"user_edited", it.user_edited},
            {"prep_thread_id", it.prep_thread_id},
            {"prepared", !it.prep_thread_id.empty()},
            {"notice", it.notice},
            {"created_at", it.created_at},
            {"sources", sources}};
}

std::vector<ItemSource> Store::sources_of(const std::string& id) {
    auto s = db_.prepare("SELECT kind,source_id,conversation,title,quote,link,at FROM agenda_sources WHERE item_id=?1 ORDER BY at, kind, source_id");
    s.bind(1, id);
    std::vector<ItemSource> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_text(1), s.col_text(2), s.col_text(3), s.col_text(4), s.col_text(5), s.col_int(6)});
    return out;
}

Status Store::put(const Item& it) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        json reminders = json::array();
        for (const auto& r : it.reminders) reminders.push_back(r);
        auto s = db_.prepare(std::string("INSERT OR REPLACE INTO agenda_items(") + kColumns +
                             ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18)");
        s.bind(1, it.id).bind(2, it.title).bind(3, it.action).bind(4, it.due_at).bind(5, it.has_time ? 1 : 0);
        s.bind(6, it.inferred ? 1 : 0).bind(7, it.state).bind(8, it.draft_kind).bind(9, it.user_edited ? 1 : 0);
        s.bind(10, it.prep_thread_id).bind(11, it.prep_run_id).bind(12, it.prepared_at).bind(13, it.refreshed ? 1 : 0);
        s.bind(14, reminders.dump()).bind(15, it.notice).bind(16, it.created_at).bind(17, it.updated_at).bind(18, it.state_at);
        if (auto r = s.run(); !r) return r;
        auto del = db_.prepare("DELETE FROM agenda_sources WHERE item_id=?1");
        del.bind(1, it.id);
        if (auto r = del.run(); !r) return r;
        for (const auto& src : it.sources) {
            auto ins = db_.prepare(
                "INSERT OR REPLACE INTO agenda_sources(item_id,kind,source_id,conversation,title,quote,link,at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
            ins.bind(1, it.id).bind(2, src.kind).bind(3, src.source_id).bind(4, src.conversation).bind(5, src.title);
            ins.bind(6, src.quote).bind(7, src.link).bind(8, src.at);
            if (auto r = ins.run(); !r) return r;
        }
        return {};
    });
}

std::optional<Item> Store::get(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kColumns + " FROM agenda_items WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    Item it = read(s);
    it.sources = sources_of(it.id);
    return it;
}

std::vector<Item> Store::list(const std::string& state) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kColumns +
                         " FROM agenda_items WHERE (?1='' OR state=?1) ORDER BY due_at, created_at, id");
    s.bind(1, state);
    std::vector<Item> out;
    while (s.step()) out.push_back(read(s));
    for (auto& it : out) it.sources = sources_of(it.id);
    return out;
}

std::vector<Item> Store::open_between(long long from_ms, long long to_ms) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare(std::string("SELECT ") + kColumns +
                         " FROM agenda_items WHERE state='open' AND due_at>=?1 AND due_at<=?2 ORDER BY due_at, id");
    s.bind(1, from_ms).bind(2, to_ms);
    std::vector<Item> out;
    while (s.step()) out.push_back(read(s));
    for (auto& it : out) it.sources = sources_of(it.id);
    return out;
}

std::vector<std::string> Store::items_with_source(const std::string& kind, const std::string& source_id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT DISTINCT item_id FROM agenda_sources WHERE kind=?1 AND source_id=?2");
    s.bind(1, kind).bind(2, source_id);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

Status Store::remove_source(const std::string& kind, const std::string& source_id) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        auto del = db_.prepare("DELETE FROM agenda_sources WHERE kind=?1 AND source_id=?2");
        del.bind(1, kind).bind(2, source_id);
        if (auto r = del.run(); !r) return r;
        // Withdrawn: an item with no remaining source goes, with its source-derived content (Agenda Item Withdrawal).
        return db_.exec("DELETE FROM agenda_items WHERE NOT EXISTS(SELECT 1 FROM agenda_sources s WHERE s.item_id=agenda_items.id)");
    });
}

Status Store::remove(const std::string& id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM agenda_items WHERE id=?1");
    s.bind(1, id);
    return s.run();
}

int Store::purge_closed(long long before_ms) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM agenda_items WHERE state IN ('done','dismissed') AND state_at<?1");
    s.bind(1, before_ms);
    if (!s.run()) return 0;
    return db_.changes();
}

Status Store::clear() {
    const std::lock_guard lock(db_.mutex());
    return db_.exec("DELETE FROM agenda_sources; DELETE FROM agenda_items; DELETE FROM agenda_pending; DELETE FROM agenda_state;");
}

int Store::enqueue(const SourceItem& item, long long now_ms, size_t cap) {
    const std::lock_guard lock(db_.mutex());
    int dropped = 0;
    (void)db_.transaction([&]() -> Status {
        auto ins = db_.prepare("INSERT OR REPLACE INTO agenda_pending(kind,source_id,item,queued_at,attempts,next_at) VALUES(?1,?2,?3,?4,0,0)");
        ins.bind(1, item.kind).bind(2, item.id).bind(3, to_json(item).dump()).bind(4, now_ms);
        if (auto r = ins.run(); !r) return r;
        auto count = db_.prepare("SELECT COUNT(*) FROM agenda_pending");
        const long long n = count.step() ? count.col_int(0) : 0;
        if (n > static_cast<long long>(cap)) {
            auto del = db_.prepare(
                "DELETE FROM agenda_pending WHERE rowid IN (SELECT rowid FROM agenda_pending ORDER BY queued_at, rowid LIMIT ?1)");
            del.bind(1, n - static_cast<long long>(cap));
            if (auto r = del.run(); !r) return r;
            dropped = db_.changes();
        }
        return {};
    });
    return dropped;
}

std::vector<Pending> Store::due_pending(long long now_ms, size_t limit) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT item,queued_at,attempts,next_at FROM agenda_pending WHERE next_at<=?1 ORDER BY queued_at, rowid LIMIT ?2");
    s.bind(1, now_ms).bind(2, static_cast<long long>(limit));
    std::vector<Pending> out;
    while (s.step()) {
        const auto j = parse_json(s.col_text(0));
        auto item = j ? source_item_from(*j) : std::nullopt;
        if (!item) continue;
        out.push_back({std::move(*item), s.col_int(1), static_cast<int>(s.col_int(2)), s.col_int(3)});
    }
    return out;
}

Status Store::dequeue(const std::vector<SourceItem>& items) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        for (const auto& it : items) {
            auto s = db_.prepare("DELETE FROM agenda_pending WHERE kind=?1 AND source_id=?2");
            s.bind(1, it.kind).bind(2, it.id);
            if (auto r = s.run(); !r) return r;
        }
        return {};
    });
}

Status Store::retry_later(const std::vector<SourceItem>& items, long long now_ms) {
    const std::lock_guard lock(db_.mutex());
    return db_.transaction([&]() -> Status {
        for (const auto& it : items) {
            // Capped exponential backoff: 2, 4, 8 ... minutes, at most one hour.
            auto s = db_.prepare(
                "UPDATE agenda_pending SET attempts=attempts+1, next_at=?3 + MIN(3600000, 120000 * (1 << MIN(attempts, 5))) "
                "WHERE kind=?1 AND source_id=?2");
            s.bind(1, it.kind).bind(2, it.id).bind(3, now_ms);
            if (auto r = s.run(); !r) return r;
        }
        return {};
    });
}

int Store::pending_count() {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT COUNT(*) FROM agenda_pending");
    return s.step() ? static_cast<int>(s.col_int(0)) : 0;
}

Status Store::unqueue_source(const std::string& kind, const std::string& source_id) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("DELETE FROM agenda_pending WHERE kind=?1 AND source_id=?2");
    s.bind(1, kind).bind(2, source_id);
    return s.run();
}

std::optional<std::string> Store::state(const std::string& key) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("SELECT value FROM agenda_state WHERE key=?1");
    s.bind(1, key);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

Status Store::set_state(const std::string& key, const std::string& value) {
    const std::lock_guard lock(db_.mutex());
    auto s = db_.prepare("INSERT OR REPLACE INTO agenda_state(key,value) VALUES(?1,?2)");
    s.bind(1, key).bind(2, value);
    return s.run();
}

long long Store::counter(const std::string& key) {
    const auto v = state(key);
    if (!v) return 0;
    try {
        return std::stoll(*v);
    } catch (...) {
        return 0;
    }
}

Status Store::add_counter(const std::string& key, long long delta) {
    const std::lock_guard lock(db_.mutex());
    return set_state(key, std::to_string(counter(key) + delta));
}

}  // namespace wb::agenda
