#include "knowledge/kg_store.h"

#include "base/strings.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <queue>
#include <set>

namespace wb::knowledge {

const std::vector<Migration>& kg_migrations() {
    static const std::vector<Migration> m = {
        {1, R"sql(
CREATE TABLE sources(
  kind TEXT PRIMARY KEY,
  user_enabled INTEGER NOT NULL DEFAULT 1,
  paused INTEGER NOT NULL DEFAULT 0,
  last_update INTEGER NOT NULL DEFAULT 0,
  last_error TEXT NOT NULL DEFAULT '');
CREATE TABLE folders(path TEXT PRIMARY KEY);
CREATE TABLE user_exclusions(kind TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY(kind, value));
CREATE TABLE skipped(source TEXT NOT NULL, ext_id TEXT NOT NULL, reason TEXT NOT NULL, PRIMARY KEY(source, ext_id));
CREATE TABLE items(
  id INTEGER PRIMARY KEY,
  source TEXT NOT NULL,
  ext_id TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  ref TEXT NOT NULL DEFAULT '',
  hash TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL DEFAULT 0,
  indexed_at INTEGER NOT NULL DEFAULT 0,
  UNIQUE(source, ext_id));
CREATE TABLE chunks(
  id INTEGER PRIMARY KEY,
  item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
  ord INTEGER NOT NULL,
  heading TEXT NOT NULL DEFAULT '',
  text TEXT NOT NULL,
  at INTEGER NOT NULL DEFAULT 0,
  tokens INTEGER NOT NULL DEFAULT 0);
CREATE INDEX chunks_item ON chunks(item_id);
CREATE VIRTUAL TABLE chunks_fts USING fts5(heading, text, content='chunks', content_rowid='id',
  tokenize='unicode61 remove_diacritics 2');
CREATE TRIGGER chunks_ai AFTER INSERT ON chunks BEGIN
  INSERT INTO chunks_fts(rowid, heading, text) VALUES (new.id, new.heading, new.text);
END;
CREATE TRIGGER chunks_ad AFTER DELETE ON chunks BEGIN
  INSERT INTO chunks_fts(chunks_fts, rowid, heading, text) VALUES ('delete', old.id, old.heading, old.text);
END;
CREATE TABLE chunks_vec(
  chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
  model TEXT NOT NULL,
  dim INTEGER NOT NULL,
  bin BLOB NOT NULL,
  i8 BLOB NOT NULL,
  PRIMARY KEY(chunk_id, model, dim)) WITHOUT ROWID;
CREATE INDEX chunks_vec_model ON chunks_vec(model, dim);
CREATE TABLE embed_queue(
  chunk_id INTEGER PRIMARY KEY REFERENCES chunks(id) ON DELETE CASCADE,
  priority INTEGER NOT NULL);
CREATE INDEX embed_queue_order ON embed_queue(priority DESC, chunk_id DESC);
CREATE TABLE extract_queue(
  chunk_id INTEGER PRIMARY KEY REFERENCES chunks(id) ON DELETE CASCADE,
  priority INTEGER NOT NULL);
CREATE INDEX extract_queue_order ON extract_queue(priority DESC, chunk_id DESC);
CREATE TABLE entities(
  id INTEGER PRIMARY KEY,
  type TEXT NOT NULL,
  name TEXT NOT NULL,
  key TEXT NOT NULL UNIQUE,
  email TEXT NOT NULL DEFAULT '',
  department TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT '',
  manager TEXT NOT NULL DEFAULT '',
  enriched_at INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT 0);
CREATE TABLE entity_aliases(
  entity_id INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  alias TEXT NOT NULL,
  norm TEXT NOT NULL,
  PRIMARY KEY(entity_id, norm));
CREATE INDEX entity_aliases_norm ON entity_aliases(norm);
CREATE TABLE mentions(
  chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
  entity_id INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  alias TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(chunk_id, entity_id));
CREATE INDEX mentions_entity ON mentions(entity_id);
CREATE TABLE relations(
  id INTEGER PRIMARY KEY,
  subject INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  predicate TEXT NOT NULL,
  object INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  confidence REAL NOT NULL,
  valid_from TEXT NOT NULL DEFAULT '',
  valid_to TEXT NOT NULL DEFAULT '',
  chunk_id INTEGER REFERENCES chunks(id) ON DELETE CASCADE);
CREATE INDEX relations_subject ON relations(subject);
CREATE INDEX relations_object ON relations(object);
CREATE INDEX relations_chunk ON relations(chunk_id);
CREATE TABLE facts_user(
  id INTEGER PRIMARY KEY,
  entity_id INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  text TEXT NOT NULL,
  thread_id TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL);
CREATE INDEX facts_user_entity ON facts_user(entity_id);
CREATE TABLE budget(day TEXT NOT NULL, kind TEXT NOT NULL, used INTEGER NOT NULL, PRIMARY KEY(day, kind));
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql"},
        // Vectors split by use (design D1, measured by spike 1.1): the coarse scan reads only the small binary rows, in
        // order of (model, chunk), and the int8 vectors (about 1 KB at 1024 dimensions) live in a rowid table where they
        // fit their page instead of spilling to a mostly empty overflow page each.
        {2, R"sql(
CREATE TABLE vec_models(id INTEGER PRIMARY KEY, model TEXT NOT NULL, dim INTEGER NOT NULL, UNIQUE(model, dim));
CREATE TABLE vec_bin(
  model_id INTEGER NOT NULL REFERENCES vec_models(id) ON DELETE CASCADE,
  chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
  bin BLOB NOT NULL,
  PRIMARY KEY(model_id, chunk_id)) WITHOUT ROWID;
CREATE INDEX vec_bin_chunk ON vec_bin(chunk_id);
CREATE TABLE vec_i8(
  id INTEGER PRIMARY KEY,
  model_id INTEGER NOT NULL REFERENCES vec_models(id) ON DELETE CASCADE,
  chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
  i8 BLOB NOT NULL,
  UNIQUE(model_id, chunk_id));
CREATE INDEX vec_i8_chunk ON vec_i8(chunk_id);
INSERT INTO vec_models(model, dim) SELECT DISTINCT model, dim FROM chunks_vec;
INSERT INTO vec_bin(model_id, chunk_id, bin)
  SELECT m.id, v.chunk_id, v.bin FROM chunks_vec v JOIN vec_models m ON m.model=v.model AND m.dim=v.dim;
INSERT INTO vec_i8(model_id, chunk_id, i8)
  SELECT m.id, v.chunk_id, v.i8 FROM chunks_vec v JOIN vec_models m ON m.model=v.model AND m.dim=v.dim;
DROP TABLE chunks_vec;
)sql"},
        // add-knowledge-sharing: single documents, notes, fact tombstones (design D5), user-stated facts with an item as
        // provenance, and shared items with their share rules (design D3).
        {3, R"sql(
CREATE TABLE documents(path TEXT PRIMARY KEY);
CREATE TABLE notes(
  id INTEGER PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  text TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE TABLE fact_tombstones(
  source TEXT NOT NULL,
  ext_id TEXT NOT NULL,
  item_hash TEXT NOT NULL,
  fact_key TEXT NOT NULL,
  PRIMARY KEY(source, ext_id, item_hash, fact_key)) WITHOUT ROWID;
ALTER TABLE facts_user ADD COLUMN item_source TEXT NOT NULL DEFAULT '';
ALTER TABLE facts_user ADD COLUMN item_ext_id TEXT NOT NULL DEFAULT '';
CREATE TABLE shares(
  source TEXT NOT NULL,
  ext_id TEXT NOT NULL,
  remote_id TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  groups TEXT NOT NULL DEFAULT '[]',
  item_hash TEXT NOT NULL DEFAULT '',
  content_hash TEXT NOT NULL DEFAULT '',
  size INTEGER NOT NULL DEFAULT 0,
  state TEXT NOT NULL DEFAULT 'shared',
  error TEXT NOT NULL DEFAULT '',
  rule TEXT NOT NULL DEFAULT '',
  updated_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(source, ext_id));
CREATE TABLE share_rules(folder TEXT PRIMARY KEY, groups TEXT NOT NULL);
)sql"},
        // add-save-as-note: where a note saved from mail or a Chat came from (design D1).
        {4, R"sql(
ALTER TABLE notes ADD COLUMN origin_kind TEXT NOT NULL DEFAULT '';
ALTER TABLE notes ADD COLUMN origin_ref TEXT NOT NULL DEFAULT '';
ALTER TABLE notes ADD COLUMN origin_title TEXT NOT NULL DEFAULT '';
)sql"},
    };
    return m;
}

const std::vector<std::string>& source_kinds() {
    static const std::vector<std::string> k = {"threads", "files", "mail", "calendar", "meetings", "notes"};
    return k;
}

std::string fts_query(const std::string& text) {
    std::string out, term;
    int n = 0;
    const auto flush = [&] {
        if (!term.empty() && n < 32) {
            if (!out.empty()) out += " OR ";
            out += '"' + term + '"';
            ++n;
        }
        term.clear();
    };
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u)) term += c;
        else flush();
    }
    flush();
    return out;
}

namespace {

constexpr const char* kEntityColumns = "id, type, name, email, department, title, manager, enriched_at";

EntityRow read_entity(const Statement& s) {
    EntityRow e;
    e.id = s.col_int(0);
    e.type = s.col_text(1);
    e.name = s.col_text(2);
    e.email = s.col_text(3);
    e.department = s.col_text(4);
    e.title = s.col_text(5);
    e.manager = s.col_text(6);
    e.enriched_at = s.col_int(7);
    return e;
}

std::string id_list(const std::vector<long long>& ids) {
    std::string out;
    for (long long id : ids) out += (out.empty() ? "" : ",") + std::to_string(id);
    return out;
}

void delete_orphan_entities(Database& db) {
    (void)db.exec(
        "DELETE FROM entities WHERE id NOT IN (SELECT entity_id FROM mentions) AND id NOT IN (SELECT entity_id FROM facts_user) "
        "AND id NOT IN (SELECT subject FROM relations) AND id NOT IN (SELECT object FROM relations)");
}

// The key of an embedding model and dimension in vec_models; 0 when unknown and create is false.
long long model_id(Database& db, const std::string& model, int dim, bool create) {
    if (create) {
        auto ins = db.prepare("INSERT OR IGNORE INTO vec_models(model, dim) VALUES(?1, ?2)");
        ins.bind(1, model).bind(2, dim);
        (void)ins.run();
    }
    auto s = db.prepare("SELECT id FROM vec_models WHERE model=?1 AND dim=?2");
    s.bind(1, model).bind(2, dim);
    return s.step() ? s.col_int(0) : 0;
}

}  // namespace

// ---- Sources, folders, exclusions ---------------------------------------------------------------------------------------

SourceState KgStore::source(const std::string& kind) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT user_enabled, paused, last_update, last_error FROM sources WHERE kind=?1");
    s.bind(1, kind);
    SourceState out;
    out.kind = kind;
    if (s.step()) {
        out.user_enabled = s.col_int(0) != 0;
        out.paused = s.col_int(1) != 0;
        out.last_update = s.col_int(2);
        out.last_error = s.col_text(3);
    }
    return out;
}

void KgStore::set_source(const SourceState& st) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR REPLACE INTO sources(kind, user_enabled, paused, last_update, last_error) VALUES(?1,?2,?3,?4,?5)");
    s.bind(1, st.kind).bind(2, st.user_enabled ? 1 : 0).bind(3, st.paused ? 1 : 0).bind(4, st.last_update).bind(5, st.last_error);
    (void)s.run();
}

void KgStore::touch_source(const std::string& kind, long long at, const std::string& error) {
    const std::lock_guard lock(db_->mutex());
    auto st = source(kind);
    if (at) st.last_update = at;
    st.last_error = error;
    set_source(st);
}

std::vector<std::string> KgStore::folders() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT path FROM folders ORDER BY path");
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

Status KgStore::add_folder(const std::string& path) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR IGNORE INTO folders(path) VALUES(?1)");
    s.bind(1, path);
    return s.run();
}

Status KgStore::remove_folder(const std::string& path) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM folders WHERE path=?1");
    s.bind(1, path);
    return s.run();
}

std::vector<std::pair<std::string, std::string>> KgStore::user_exclusions() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT kind, value FROM user_exclusions ORDER BY kind, value");
    std::vector<std::pair<std::string, std::string>> out;
    while (s.step()) out.emplace_back(s.col_text(0), s.col_text(1));
    return out;
}

Status KgStore::add_user_exclusion(const std::string& kind, const std::string& value) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR IGNORE INTO user_exclusions(kind, value) VALUES(?1,?2)");
    s.bind(1, kind).bind(2, value);
    return s.run();
}

Status KgStore::remove_user_exclusion(const std::string& kind, const std::string& value) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM user_exclusions WHERE kind=?1 AND value=?2");
    s.bind(1, kind).bind(2, value);
    return s.run();
}

void KgStore::set_skipped(const std::string& source, const std::string& ext_id, const std::string& reason) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR REPLACE INTO skipped(source, ext_id, reason) VALUES(?1,?2,?3)");
    s.bind(1, source).bind(2, ext_id).bind(3, reason);
    (void)s.run();
}

void KgStore::clear_skipped(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM skipped WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    (void)s.run();
}

std::vector<std::pair<std::string, std::string>> KgStore::skipped(const std::string& source, int limit) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT ext_id, reason FROM skipped WHERE source=?1 ORDER BY ext_id LIMIT ?2");
    s.bind(1, source).bind(2, limit);
    std::vector<std::pair<std::string, std::string>> out;
    while (s.step()) out.emplace_back(s.col_text(0), s.col_text(1));
    return out;
}

std::vector<std::string> KgStore::documents() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT path FROM documents ORDER BY path");
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

Status KgStore::add_document(const std::string& path) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR IGNORE INTO documents(path) VALUES(?1)");
    s.bind(1, path);
    return s.run();
}

Status KgStore::remove_document(const std::string& path) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM documents WHERE path=?1");
    s.bind(1, path);
    return s.run();
}

// ---- Notes --------------------------------------------------------------------------------------------------------------

long long KgStore::save_note(long long id, const std::string& title, const std::string& text, long long now, const NoteRow& origin) {
    const std::lock_guard lock(db_->mutex());
    if (id) {
        auto s = db_->prepare("UPDATE notes SET title=?2, text=?3, updated_at=?4 WHERE id=?1");
        s.bind(1, id).bind(2, title).bind(3, text).bind(4, now);
        if (!s.run() || db_->changes() == 0) return 0;
        return id;
    }
    auto s = db_->prepare(
        "INSERT INTO notes(title, text, created_at, updated_at, origin_kind, origin_ref, origin_title) VALUES(?1,?2,?3,?3,?4,?5,?6)");
    s.bind(1, title).bind(2, text).bind(3, now).bind(4, origin.origin_kind).bind(5, origin.origin_ref).bind(6, origin.origin_title);
    if (!s.run()) return 0;
    return db_->last_insert_id();
}

bool KgStore::delete_note(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM notes WHERE id=?1");
    s.bind(1, id);
    (void)s.run();
    return db_->changes() > 0;
}

namespace {

constexpr const char* kNoteColumns = "id, title, text, created_at, updated_at, origin_kind, origin_ref, origin_title";

NoteRow read_note(const Statement& s) {
    return {s.col_int(0), s.col_text(1), s.col_text(2), s.col_int(3), s.col_int(4), s.col_text(5), s.col_text(6), s.col_text(7)};
}

}  // namespace

std::optional<NoteRow> KgStore::note(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kNoteColumns + " FROM notes WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_note(s);
}

std::vector<NoteRow> KgStore::notes() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kNoteColumns + " FROM notes ORDER BY updated_at DESC, id DESC");
    std::vector<NoteRow> out;
    while (s.step()) out.push_back(read_note(s));
    return out;
}

// ---- Items and chunks ---------------------------------------------------------------------------------------------------

std::optional<std::string> KgStore::item_hash(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT hash FROM items WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

Result<long long> KgStore::put_item(const ItemInput& in, long long now) {
    const std::lock_guard lock(db_->mutex());
    long long id = 0;
    Status st = db_->transaction([&]() -> Status {
        {
            auto d = db_->prepare("DELETE FROM items WHERE source=?1 AND ext_id=?2");
            d.bind(1, in.source).bind(2, in.ext_id);
            if (Status s = d.run(); !s) return s;
        }
        auto i = db_->prepare(
            "INSERT INTO items(source, ext_id, title, ref, hash, created_at, updated_at, indexed_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
        i.bind(1, in.source).bind(2, in.ext_id).bind(3, in.title).bind(4, in.ref).bind(5, in.hash).bind(6, in.created_at);
        i.bind(7, in.updated_at).bind(8, now);
        if (Status s = i.run(); !s) return s;
        id = db_->last_insert_id();
        int ord = 0;
        for (const auto& c : in.chunks) {
            auto ci = db_->prepare("INSERT INTO chunks(item_id, ord, heading, text, at, tokens) VALUES(?1,?2,?3,?4,?5,?6)");
            ci.bind(1, id).bind(2, ord++).bind(3, c.heading).bind(4, c.text).bind(5, c.at).bind(6, estimate_tokens(c.text));
            if (Status s = ci.run(); !s) return s;
            const long long cid = db_->last_insert_id();
            auto q = db_->prepare("INSERT INTO embed_queue(chunk_id, priority) VALUES(?1,?2)");
            q.bind(1, cid).bind(2, in.priority);
            if (Status s = q.run(); !s) return s;
            if (in.extract) {
                auto x = db_->prepare("INSERT INTO extract_queue(chunk_id, priority) VALUES(?1,?2)");
                x.bind(1, cid).bind(2, in.priority);
                if (Status s = x.run(); !s) return s;
            }
        }
        auto sk = db_->prepare("DELETE FROM skipped WHERE source=?1 AND ext_id=?2");
        sk.bind(1, in.source).bind(2, in.ext_id);
        return sk.run();
    });
    if (!st) return st.error();
    delete_orphan_entities(*db_);
    return id;
}

bool KgStore::remove_item(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM items WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    (void)s.run();
    const bool removed = db_->changes() > 0;
    auto k = db_->prepare("DELETE FROM skipped WHERE source=?1 AND ext_id=?2");
    k.bind(1, source).bind(2, ext_id);
    (void)k.run();
    if (removed) delete_orphan_entities(*db_);
    return removed;
}

bool KgStore::remove_item(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM items WHERE id=?1");
    s.bind(1, id);
    (void)s.run();
    const bool removed = db_->changes() > 0;
    if (removed) delete_orphan_entities(*db_);
    return removed;
}

std::vector<std::string> KgStore::item_ids(const std::string& source) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT ext_id FROM items WHERE source=?1");
    s.bind(1, source);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

int KgStore::remove_source(const std::string& kind) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM items WHERE source=?1");
    s.bind(1, kind);
    (void)s.run();
    const int n = db_->changes();
    auto k = db_->prepare("DELETE FROM skipped WHERE source=?1");
    k.bind(1, kind);
    (void)k.run();
    delete_orphan_entities(*db_);
    return n;
}

namespace {
constexpr const char* kItemColumns = "id, source, ext_id, title, ref, created_at, updated_at, indexed_at, hash";
ItemRow read_item(const Statement& s) {
    return {s.col_int(0), s.col_text(1), s.col_text(2), s.col_text(3), s.col_text(4), s.col_int(5), s.col_int(6), s.col_int(7), s.col_text(8)};
}
}  // namespace

std::optional<ItemRow> KgStore::item(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns + " FROM items WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_item(s);
}

std::optional<ItemRow> KgStore::item(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns + " FROM items WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    if (!s.step()) return std::nullopt;
    return read_item(s);
}

std::vector<ItemRow> KgStore::find_items(const std::string& query, int limit) {
    std::vector<ItemRow> out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns +
                          " FROM items WHERE instr(lower(title), lower(?1)) > 0 OR instr(lower(ref), lower(?1)) > 0"
                          " ORDER BY updated_at DESC, id DESC LIMIT ?2");
    s.bind(1, query).bind(2, static_cast<long long>(limit));
    while (s.step()) out.push_back(read_item(s));
    return out;
}

std::vector<ChunkRow> KgStore::chunks(const std::vector<long long>& ids) {
    std::vector<ChunkRow> out;
    if (ids.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT id, item_id, heading, text, at FROM chunks WHERE id IN (" + id_list(ids) + ")");
    std::map<long long, ChunkRow> by_id;
    while (s.step()) by_id[s.col_int(0)] = {s.col_int(0), s.col_int(1), s.col_text(2), s.col_text(3), s.col_int(4)};
    for (long long id : ids)
        if (const auto it = by_id.find(id); it != by_id.end()) out.push_back(it->second);
    return out;
}

std::vector<ChunkRow> KgStore::chunks_of_item(long long item_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT id, item_id, heading, text, at FROM chunks WHERE item_id=?1 ORDER BY ord");
    s.bind(1, item_id);
    std::vector<ChunkRow> out;
    while (s.step()) out.push_back({s.col_int(0), s.col_int(1), s.col_text(2), s.col_text(3), s.col_int(4)});
    return out;
}

SourceCounts KgStore::counts(const std::string& source) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(
        "SELECT (SELECT count(*) FROM items WHERE source=?1),"
        " (SELECT count(*) FROM chunks c JOIN items i ON i.id=c.item_id WHERE i.source=?1),"
        " (SELECT count(*) FROM embed_queue q JOIN chunks c ON c.id=q.chunk_id JOIN items i ON i.id=c.item_id WHERE i.source=?1),"
        " (SELECT count(*) FROM extract_queue q JOIN chunks c ON c.id=q.chunk_id JOIN items i ON i.id=c.item_id WHERE i.source=?1),"
        " (SELECT count(*) FROM skipped WHERE source=?1)");
    s.bind(1, source);
    SourceCounts c;
    if (s.step()) c = {s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4)};
    return c;
}

std::vector<BrowseRow> KgStore::browse(const std::string& source, const std::string& query, int limit, bool extraction) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns +
                          ", (SELECT ifnull(sum(length(c.text)), 0) FROM chunks c WHERE c.item_id=items.id)"
                          ", (SELECT count(*) FROM chunks c WHERE c.item_id=items.id AND (c.id IN (SELECT chunk_id FROM embed_queue)"
                          " OR (?4 AND c.id IN (SELECT chunk_id FROM extract_queue))))"
                          " FROM items WHERE source=?1 AND (?2='' OR instr(lower(title), lower(?2)) > 0 OR instr(lower(ref), lower(?2)) > 0)"
                          " ORDER BY updated_at DESC, id DESC LIMIT ?3");
    s.bind(1, source).bind(2, query).bind(3, static_cast<long long>(limit)).bind(4, extraction ? 1 : 0);
    std::vector<BrowseRow> out;
    while (s.step()) out.push_back({read_item(s), s.col_int(9), s.col_int(10)});
    return out;
}

long long KgStore::item_pending(long long item_id, bool extraction) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT count(*) FROM chunks c WHERE c.item_id=?1 AND (c.id IN (SELECT chunk_id FROM embed_queue)"
                          " OR (?2 AND c.id IN (SELECT chunk_id FROM extract_queue)))");
    s.bind(1, item_id).bind(2, extraction ? 1 : 0);
    return s.step() ? s.col_int(0) : 0;
}

// ---- Full text ----------------------------------------------------------------------------------------------------------

std::vector<long long> KgStore::fts(const std::string& query, int limit) {
    std::vector<long long> out;
    const std::string q = fts_query(query);
    if (q.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT rowid FROM chunks_fts WHERE chunks_fts MATCH ?1 ORDER BY rank LIMIT ?2");
    s.bind(1, q).bind(2, limit);
    while (s.step()) out.push_back(s.col_int(0));
    return out;
}

// ---- Embeddings ---------------------------------------------------------------------------------------------------------

std::vector<ChunkRow> KgStore::embed_batch(int n) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(
        "SELECT c.id, c.item_id, c.heading, c.text, c.at FROM embed_queue q JOIN chunks c ON c.id=q.chunk_id "
        "ORDER BY q.priority DESC, q.chunk_id DESC LIMIT ?1");
    s.bind(1, n);
    std::vector<ChunkRow> out;
    while (s.step()) out.push_back({s.col_int(0), s.col_int(1), s.col_text(2), s.col_text(3), s.col_int(4)});
    return out;
}

long long KgStore::embed_pending() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT count(*) FROM embed_queue");
    return s.step() ? s.col_int(0) : 0;
}

void KgStore::store_vectors(const std::vector<std::pair<long long, std::vector<float>>>& vectors, const std::string& model, int dim) {
    const std::lock_guard lock(db_->mutex());
    (void)db_->transaction([&]() -> Status {
        const long long mid = model_id(*db_, model, dim, true);
        for (const auto& [id, v] : vectors) {
            auto b = db_->prepare("INSERT OR REPLACE INTO vec_bin(model_id, chunk_id, bin) "
                                  "SELECT ?1,?2,?3 WHERE EXISTS (SELECT 1 FROM chunks WHERE id=?2)");
            b.bind(1, mid).bind(2, id).bind_blob(3, quantize_binary(v));
            if (Status st = b.run(); !st) return st;
            auto s = db_->prepare("INSERT OR REPLACE INTO vec_i8(model_id, chunk_id, i8) "
                                  "SELECT ?1,?2,?3 WHERE EXISTS (SELECT 1 FROM chunks WHERE id=?2)");
            s.bind(1, mid).bind(2, id).bind_blob(3, quantize_int8(v));
            if (Status st = s.run(); !st) return st;
            auto d = db_->prepare("DELETE FROM embed_queue WHERE chunk_id=?1");
            d.bind(1, id);
            if (Status st = d.run(); !st) return st;
        }
        return {};
    });
}

void KgStore::drop_from_embed_queue(const std::vector<long long>& ids) {
    if (ids.empty()) return;
    const std::lock_guard lock(db_->mutex());
    (void)db_->exec("DELETE FROM embed_queue WHERE chunk_id IN (" + id_list(ids) + ")");
}

std::pair<std::string, int> KgStore::active_embedding() {
    const std::string m = meta("embed.model"), d = meta("embed.dim");
    return {m, d.empty() ? 0 : std::atoi(d.c_str())};
}

void KgStore::set_active_embedding(const std::string& model, int dim) {
    set_meta("embed.model", model);
    set_meta("embed.dim", std::to_string(dim));
}

long long KgStore::queue_reembed(const std::string& model, int dim) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(
        "INSERT OR IGNORE INTO embed_queue(chunk_id, priority) SELECT c.id, 0 FROM chunks c WHERE NOT EXISTS "
        "(SELECT 1 FROM vec_bin v JOIN vec_models m ON m.id=v.model_id WHERE v.chunk_id=c.id AND m.model=?1 AND m.dim=?2)");
    s.bind(1, model).bind(2, dim);
    (void)s.run();
    return db_->changes();
}

void KgStore::drop_other_vectors(const std::string& model, int dim) {
    const std::lock_guard lock(db_->mutex());
    // The cascade removes their binary and int8 vectors.
    auto s = db_->prepare("DELETE FROM vec_models WHERE NOT (model=?1 AND dim=?2)");
    s.bind(1, model).bind(2, dim);
    (void)s.run();
}

long long KgStore::vectors_of(const std::string& model, int dim) {
    const std::lock_guard lock(db_->mutex());
    const long long mid = model_id(*db_, model, dim, false);
    if (!mid) return 0;
    auto s = db_->prepare("SELECT count(*) FROM vec_bin WHERE model_id=?1");
    s.bind(1, mid);
    return s.step() ? s.col_int(0) : 0;
}

std::vector<std::pair<long long, double>> KgStore::vector_search(const std::vector<float>& query, const std::string& model, int dim,
                                                                 int limit, int coarse) {
    std::vector<std::pair<long long, double>> out;
    if (query.empty() || static_cast<int>(query.size()) != dim) return out;
    const auto qb = quantize_binary(query), qi = quantize_int8(query);
    // Coarse: the `coarse` smallest Hamming distances (a max-heap of the best so far).
    std::priority_queue<std::pair<int, long long>> best;
    long long mid = 0;
    {
        const std::lock_guard lock(db_->mutex());
        mid = model_id(*db_, model, dim, false);
        if (!mid) return out;
        // A range of the (model_id, chunk_id) key: the binary rows in order, nothing else read.
        auto s = db_->prepare("SELECT chunk_id, bin FROM vec_bin WHERE model_id=?1");
        s.bind(1, mid);
        while (s.step()) {
            const auto b = s.col_blob(1);
            if (b.size() != qb.size()) continue;
            const int d = hamming(b.data(), qb.data(), b.size());
            if (static_cast<int>(best.size()) < coarse) best.emplace(d, s.col_int(0));
            else if (d < best.top().first) {
                best.pop();
                best.emplace(d, s.col_int(0));
            }
        }
    }
    std::vector<long long> ids;
    while (!best.empty()) {
        ids.push_back(best.top().second);
        best.pop();
    }
    if (ids.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT chunk_id, i8 FROM vec_i8 WHERE model_id=?1 AND chunk_id IN (" + id_list(ids) + ")");
    s.bind(1, mid);
    while (s.step()) {
        const auto v = s.col_blob(1);
        if (v.size() != qi.size()) continue;
        out.emplace_back(s.col_int(0), int8_cosine(v.data(), qi.data(), v.size()));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (static_cast<int>(out.size()) > limit) out.resize(static_cast<size_t>(limit));
    return out;
}

// ---- Extraction ---------------------------------------------------------------------------------------------------------

std::vector<ChunkRow> KgStore::extract_batch(int n) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(
        "SELECT c.id, c.item_id, c.heading, c.text, c.at FROM extract_queue q JOIN chunks c ON c.id=q.chunk_id "
        "ORDER BY q.priority DESC, q.chunk_id DESC LIMIT ?1");
    s.bind(1, n);
    std::vector<ChunkRow> out;
    while (s.step()) out.push_back({s.col_int(0), s.col_int(1), s.col_text(2), s.col_text(3), s.col_int(4)});
    return out;
}

long long KgStore::extract_pending() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT count(*) FROM extract_queue");
    return s.step() ? s.col_int(0) : 0;
}

void KgStore::mark_extracted(const std::vector<long long>& chunk_ids) {
    if (chunk_ids.empty()) return;
    const std::lock_guard lock(db_->mutex());
    (void)db_->exec("DELETE FROM extract_queue WHERE chunk_id IN (" + id_list(chunk_ids) + ")");
}

void KgStore::clear_extract_queue() {
    const std::lock_guard lock(db_->mutex());
    (void)db_->exec("DELETE FROM extract_queue");
}

// ---- Entities -----------------------------------------------------------------------------------------------------------

namespace {

std::vector<std::string> words(const std::string& norm) { return split(norm, ' '); }

}  // namespace

long long KgStore::resolve_entity(const std::string& type, const std::string& name_in, const std::string& email_in, long long now) {
    const std::lock_guard lock(db_->mutex());
    const std::string email = to_lower(trim(email_in));
    std::string name = trim(name_in);
    if (name.empty()) name = email;
    const std::string norm = normalize_name(name);
    if (norm.empty()) return 0;
    const auto add_alias = [&](long long id) {
        auto a = db_->prepare("INSERT OR IGNORE INTO entity_aliases(entity_id, alias, norm) VALUES(?1,?2,?3)");
        a.bind(1, id).bind(2, name).bind(3, norm);
        (void)a.run();
    };
    const auto by_key = [&](const std::string& key) -> long long {
        auto s = db_->prepare("SELECT id FROM entities WHERE key=?1");
        s.bind(1, key);
        return s.step() ? s.col_int(0) : 0;
    };
    const auto create = [&](const std::string& key) -> long long {
        auto s = db_->prepare("INSERT INTO entities(type, name, key, email, created_at) VALUES(?1,?2,?3,?4,?5)");
        s.bind(1, type).bind(2, name).bind(3, key).bind(4, email).bind(5, now);
        if (!s.run()) return 0;
        const long long id = db_->last_insert_id();
        add_alias(id);
        return id;
    };
    if (type == "Person") {
        if (!email.empty()) {
            if (long long id = by_key("person:" + email)) {
                add_alias(id);
                return id;
            }
            // A Person first seen by name only takes the address when the name matches one person unambiguously.
            auto s = db_->prepare(
                "SELECT DISTINCT e.id FROM entities e JOIN entity_aliases a ON a.entity_id=e.id WHERE e.type='Person' AND e.email='' AND a.norm=?1");
            s.bind(1, norm);
            std::vector<long long> hits;
            while (s.step()) hits.push_back(s.col_int(0));
            if (hits.size() == 1) {
                auto u = db_->prepare("UPDATE entities SET email=?1, key=?2 WHERE id=?3");
                u.bind(1, email).bind(2, "person:" + email).bind(3, hits[0]);
                (void)u.run();
                add_alias(hits[0]);
                return hits[0];
            }
            return create("person:" + email);
        }
        // By name: an exact alias of exactly one Person, else an abbreviation ("Jana N.") of exactly one Person.
        std::set<long long> hits;
        {
            auto s = db_->prepare("SELECT DISTINCT e.id FROM entities e JOIN entity_aliases a ON a.entity_id=e.id WHERE e.type='Person' AND a.norm=?1");
            s.bind(1, norm);
            while (s.step()) hits.insert(s.col_int(0));
        }
        if (hits.empty()) {
            const auto w = words(norm);
            if (w.size() >= 2 && w.back().size() == 1) {
                auto s = db_->prepare(
                    "SELECT DISTINCT e.id, a.norm FROM entities e JOIN entity_aliases a ON a.entity_id=e.id WHERE e.type='Person' AND a.norm LIKE ?1");
                s.bind(1, w.front() + " %");
                while (s.step()) {
                    const auto cand = words(s.col_text(1));
                    if (cand.size() >= 2 && cand.back().size() > 1 && cand.back()[0] == w.back()[0]) hits.insert(s.col_int(0));
                }
            }
        }
        if (hits.size() == 1) {
            add_alias(*hits.begin());
            return *hits.begin();
        }
        if (long long id = by_key("person-name:" + norm)) return id;
        return create("person-name:" + norm);
    }
    const std::string key = to_lower(type) + ":" + norm;
    if (long long id = by_key(key)) return id;
    // Another entity of the type may know this name as an alias (merged earlier by the user).
    auto s = db_->prepare("SELECT DISTINCT e.id FROM entities e JOIN entity_aliases a ON a.entity_id=e.id WHERE e.type=?1 AND a.norm=?2");
    s.bind(1, type).bind(2, norm);
    std::vector<long long> hits;
    while (s.step()) hits.push_back(s.col_int(0));
    if (hits.size() == 1) return hits[0];
    return create(key);
}

std::optional<EntityRow> KgStore::entity(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kEntityColumns + " FROM entities WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    EntityRow e = read_entity(s);
    auto a = db_->prepare("SELECT alias FROM entity_aliases WHERE entity_id=?1 ORDER BY alias");
    a.bind(1, id);
    while (a.step()) e.aliases.push_back(a.col_text(0));
    return e;
}

std::vector<EntityRow> KgStore::find_entities(const std::string& query, int limit) {
    const std::lock_guard lock(db_->mutex());
    const std::string norm = normalize_name(query);
    std::vector<EntityRow> out;
    if (norm.empty()) {
        auto s = db_->prepare(std::string("SELECT ") + kEntityColumns + " FROM entities ORDER BY name LIMIT ?1");
        s.bind(1, limit);
        while (s.step()) out.push_back(read_entity(s));
        return out;
    }
    // Exact alias first, then aliases containing the words, then email.
    auto s = db_->prepare(std::string("SELECT ") + kEntityColumns +
                          ", (SELECT max(CASE WHEN a.norm=?1 THEN 2 WHEN a.norm LIKE ?2 THEN 1 ELSE 0 END) FROM entity_aliases a WHERE a.entity_id=entities.id) AS score"
                          " FROM entities WHERE score>0 OR email=?3 ORDER BY score DESC, name LIMIT ?4");
    s.bind(1, norm).bind(2, "%" + norm + "%").bind(3, to_lower(trim(query))).bind(4, limit);
    while (s.step()) out.push_back(read_entity(s));
    return out;
}

std::vector<EntityRow> KgStore::entities_in_chunks(const std::vector<long long>& chunk_ids) {
    std::vector<EntityRow> out;
    if (chunk_ids.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT DISTINCT ") + "e.id, e.type, e.name, e.email, e.department, e.title, e.manager, e.enriched_at" +
                          " FROM entities e JOIN mentions m ON m.entity_id=e.id WHERE m.chunk_id IN (" + id_list(chunk_ids) + ")");
    while (s.step()) out.push_back(read_entity(s));
    return out;
}

std::vector<long long> KgStore::entities_named_in(const std::string& text, int limit) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT entity_id, max(length(norm)) AS l FROM entity_aliases WHERE length(norm)>=3 AND instr(?1, ' ' || norm || ' ')>0 "
                          "GROUP BY entity_id ORDER BY l DESC LIMIT ?2");
    s.bind(1, " " + normalize_name(text) + " ").bind(2, limit);
    std::vector<long long> out;
    while (s.step()) out.push_back(s.col_int(0));
    return out;
}

void KgStore::add_mention(long long chunk_id, long long entity_id, const std::string& alias) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR IGNORE INTO mentions(chunk_id, entity_id, alias) VALUES(?1,?2,?3)");
    s.bind(1, chunk_id).bind(2, entity_id).bind(3, normalize_name(alias));
    (void)s.run();
}

long long KgStore::add_relation(long long subject, const std::string& predicate, long long object, double confidence,
                                const std::string& valid_from, const std::string& valid_to, long long chunk_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(
        "INSERT INTO relations(subject, predicate, object, confidence, valid_from, valid_to, chunk_id) VALUES(?1,?2,?3,?4,?5,?6,?7)");
    s.bind(1, subject).bind(2, predicate).bind(3, object).bind(4, confidence).bind(5, valid_from).bind(6, valid_to);
    if (chunk_id) s.bind(7, chunk_id);
    else s.bind_null(7);
    if (!s.run()) return 0;
    return db_->last_insert_id();
}

namespace {
constexpr const char* kRelationSelect =
    "SELECT r.id, r.subject, r.object, ifnull(r.chunk_id, 0), r.predicate, r.valid_from, r.valid_to, r.confidence, s.name, o.name "
    "FROM relations r JOIN entities s ON s.id=r.subject JOIN entities o ON o.id=r.object ";
RelationRow read_relation(const Statement& s) {
    RelationRow r;
    r.id = s.col_int(0);
    r.subject = s.col_int(1);
    r.object = s.col_int(2);
    r.chunk_id = s.col_int(3);
    r.predicate = s.col_text(4);
    r.valid_from = s.col_text(5);
    r.valid_to = s.col_text(6);
    r.confidence = s.col_double(7);
    r.subject_name = s.col_text(8);
    r.object_name = s.col_text(9);
    return r;
}
}  // namespace

std::vector<RelationRow> KgStore::relations_of(long long entity_id, int limit) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string(kRelationSelect) + "WHERE r.subject=?1 OR r.object=?1 ORDER BY r.valid_from DESC, r.id DESC LIMIT ?2");
    s.bind(1, entity_id).bind(2, limit);
    std::vector<RelationRow> out;
    while (s.step()) out.push_back(read_relation(s));
    return out;
}

std::optional<RelationRow> KgStore::relation(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string(kRelationSelect) + "WHERE r.id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_relation(s);
}

bool KgStore::delete_relation(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM relations WHERE id=?1");
    s.bind(1, id);
    (void)s.run();
    const bool removed = db_->changes() > 0;
    if (removed) delete_orphan_entities(*db_);
    return removed;
}

std::vector<RelationRow> KgStore::relations_of_item(long long item_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string(kRelationSelect) + "WHERE r.chunk_id IN (SELECT id FROM chunks WHERE item_id=?1) ORDER BY r.id");
    s.bind(1, item_id);
    std::vector<RelationRow> out;
    while (s.step()) out.push_back(read_relation(s));
    return out;
}

std::vector<EntityRow> KgStore::entities_of_item(long long item_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT DISTINCT e.id, e.type, e.name, e.email, e.department, e.title, e.manager, e.enriched_at FROM entities e "
                          "JOIN mentions m ON m.entity_id=e.id JOIN chunks c ON c.id=m.chunk_id WHERE c.item_id=?1 ORDER BY e.name");
    s.bind(1, item_id);
    std::vector<EntityRow> out;
    while (s.step()) out.push_back(read_entity(s));
    return out;
}

Status KgStore::rename_entity(long long id, const std::string& name_in) {
    const std::string name = trim(name_in);
    const std::string norm = normalize_name(name);
    if (norm.empty()) return make_error("invalid", "The name is empty.");
    const std::lock_guard lock(db_->mutex());
    const auto e = entity(id);
    if (!e) return make_error("not_found", "no such entity");
    // A Person keyed by address keeps its key; any other entity is keyed by its name.
    std::string key;
    if (e->type == "Person") key = e->email.empty() ? "person-name:" + norm : "person:" + e->email;
    else key = to_lower(e->type) + ":" + norm;
    {
        auto c = db_->prepare("SELECT id, name FROM entities WHERE key=?1 AND id<>?2");
        c.bind(1, key).bind(2, id);
        if (c.step()) return make_error("conflict", "Another entity is already called " + c.col_text(1) + ". Merge them instead.");
    }
    return db_->transaction([&]() -> Status {
        auto u = db_->prepare("UPDATE entities SET name=?1, key=?2 WHERE id=?3");
        u.bind(1, name).bind(2, key).bind(3, id);
        if (Status s = u.run(); !s) return s;
        auto a = db_->prepare("INSERT OR IGNORE INTO entity_aliases(entity_id, alias, norm) VALUES(?1,?2,?3)");
        a.bind(1, id).bind(2, name).bind(3, norm);
        return a.run();
    });
}

void KgStore::add_tombstone(const std::string& source, const std::string& ext_id, const std::string& item_hash, const std::string& fact_key) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR IGNORE INTO fact_tombstones(source, ext_id, item_hash, fact_key) VALUES(?1,?2,?3,?4)");
    s.bind(1, source).bind(2, ext_id).bind(3, item_hash).bind(4, fact_key);
    (void)s.run();
}

bool KgStore::tombstoned(const std::string& source, const std::string& ext_id, const std::string& item_hash, const std::string& fact_key) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT 1 FROM fact_tombstones WHERE source=?1 AND ext_id=?2 AND item_hash=?3 AND fact_key=?4");
    s.bind(1, source).bind(2, ext_id).bind(3, item_hash).bind(4, fact_key);
    return s.step();
}

std::vector<long long> KgStore::chunks_mentioning(const std::vector<long long>& entity_ids, int limit) {
    std::vector<long long> out;
    if (entity_ids.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT DISTINCT m.chunk_id FROM mentions m JOIN chunks c ON c.id=m.chunk_id JOIN items i ON i.id=c.item_id "
                          "WHERE m.entity_id IN (" + id_list(entity_ids) + ") ORDER BY i.updated_at DESC, c.id DESC LIMIT ?1");
    s.bind(1, limit);
    while (s.step()) out.push_back(s.col_int(0));
    return out;
}

std::vector<long long> KgStore::neighbours(const std::vector<long long>& entity_ids, int limit) {
    std::vector<long long> out;
    if (entity_ids.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    const std::string ids = id_list(entity_ids);
    auto s = db_->prepare("SELECT n, count(*) AS k FROM (SELECT object AS n FROM relations WHERE subject IN (" + ids +
                          ") UNION ALL SELECT subject AS n FROM relations WHERE object IN (" + ids + ")) WHERE n NOT IN (" + ids +
                          ") GROUP BY n ORDER BY k DESC LIMIT ?1");
    s.bind(1, limit);
    while (s.step()) out.push_back(s.col_int(0));
    return out;
}

void KgStore::set_enrichment(long long id, const std::string& name, const std::string& department, const std::string& title,
                             const std::string& manager, long long at) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("UPDATE entities SET name=CASE WHEN ?2='' THEN name ELSE ?2 END, department=?3, title=?4, manager=?5, enriched_at=?6 WHERE id=?1");
    s.bind(1, id).bind(2, name).bind(3, department).bind(4, title).bind(5, manager).bind(6, at);
    (void)s.run();
    if (!name.empty()) {
        auto a = db_->prepare("INSERT OR IGNORE INTO entity_aliases(entity_id, alias, norm) VALUES(?1,?2,?3)");
        a.bind(1, id).bind(2, name).bind(3, normalize_name(name));
        (void)a.run();
    }
}

std::vector<EntityRow> KgStore::people_to_enrich(long long older_than, int limit) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kEntityColumns +
                          " FROM entities WHERE type='Person' AND email<>'' AND enriched_at<?1 ORDER BY enriched_at, id LIMIT ?2");
    s.bind(1, older_than).bind(2, limit);
    std::vector<EntityRow> out;
    while (s.step()) out.push_back(read_entity(s));
    return out;
}

Status KgStore::merge_entities(long long from, long long into) {
    if (from == into) return make_error("invalid", "an entity cannot be merged into itself");
    const std::lock_guard lock(db_->mutex());
    const auto a = entity(from), b = entity(into);
    if (!a || !b) return make_error("not_found", "no such entity");
    return db_->transaction([&]() -> Status {
        const std::string f = std::to_string(from), t = std::to_string(into);
        const std::vector<std::string> sqls = {"INSERT OR IGNORE INTO entity_aliases(entity_id, alias, norm) SELECT " + t + ", alias, norm FROM entity_aliases WHERE entity_id=" + f,
              "INSERT OR IGNORE INTO mentions(chunk_id, entity_id, alias) SELECT chunk_id, " + t + ", alias FROM mentions WHERE entity_id=" + f,
              "UPDATE relations SET subject=" + t + " WHERE subject=" + f, "UPDATE relations SET object=" + t + " WHERE object=" + f,
              "DELETE FROM relations WHERE subject=object", "UPDATE facts_user SET entity_id=" + t + " WHERE entity_id=" + f,
              "DELETE FROM entities WHERE id=" + f};
        for (const auto& sql : sqls)
            if (Status s = db_->exec(sql); !s) return s;
        if (b->email.empty() && !a->email.empty()) {
            auto u = db_->prepare("UPDATE entities SET email=?1, key=?2 WHERE id=?3");
            u.bind(1, a->email).bind(2, "person:" + a->email).bind(3, into);
            return u.run();
        }
        return {};
    });
}

Result<long long> KgStore::split_entity(long long id, const std::string& alias, long long now) {
    const std::lock_guard lock(db_->mutex());
    const auto e = entity(id);
    if (!e) return make_error("not_found", "no such entity");
    const std::string norm = normalize_name(alias);
    if (e->aliases.size() < 2) return make_error("invalid", "the entity has only one name");
    long long created = 0;
    Status st = db_->transaction([&]() -> Status {
        auto c = db_->prepare("INSERT INTO entities(type, name, key, created_at) VALUES(?1,?2,?3,?4)");
        c.bind(1, e->type).bind(2, alias).bind(3, to_lower(e->type) + "-split:" + norm + ":" + std::to_string(now) + ":" + std::to_string(id)).bind(4, now);
        if (Status s = c.run(); !s) return s;
        created = db_->last_insert_id();
        const std::string n = std::to_string(created), o = std::to_string(id);
        auto a = db_->prepare("UPDATE entity_aliases SET entity_id=?1 WHERE entity_id=?2 AND norm=?3");
        a.bind(1, created).bind(2, id).bind(3, norm);
        if (Status s = a.run(); !s) return s;
        if (db_->changes() == 0) return make_error("not_found", "the entity has no such name");
        // Relations taken from the chunks that mention this name move with it.
        auto r = db_->prepare("UPDATE relations SET subject=?1 WHERE subject=?2 AND chunk_id IN (SELECT chunk_id FROM mentions WHERE entity_id=?2 AND alias=?3)");
        r.bind(1, created).bind(2, id).bind(3, norm);
        if (Status s = r.run(); !s) return s;
        auto r2 = db_->prepare("UPDATE relations SET object=?1 WHERE object=?2 AND chunk_id IN (SELECT chunk_id FROM mentions WHERE entity_id=?2 AND alias=?3)");
        r2.bind(1, created).bind(2, id).bind(3, norm);
        if (Status s = r2.run(); !s) return s;
        auto m = db_->prepare("UPDATE mentions SET entity_id=?1 WHERE entity_id=?2 AND alias=?3");
        m.bind(1, created).bind(2, id).bind(3, norm);
        return m.run();
    });
    if (!st) return st.error();
    return created;
}

Status KgStore::forget_entity(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM entities WHERE id=?1");
    s.bind(1, id);
    if (Status st = s.run(); !st) return st;
    if (db_->changes() == 0) return make_error("not_found", "no such entity");
    return {};
}

// ---- User-stated facts --------------------------------------------------------------------------------------------------

namespace {
constexpr const char* kFactColumns = "id, entity_id, text, thread_id, created_at, item_source, item_ext_id";
FactRow read_fact(const Statement& s) {
    return {s.col_int(0), s.col_int(1), s.col_text(2), s.col_text(3), s.col_int(4), s.col_text(5), s.col_text(6)};
}
}  // namespace

long long KgStore::add_fact(long long entity_id, const std::string& text, const std::string& thread_id, long long now,
                            const std::string& item_source, const std::string& item_ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT INTO facts_user(entity_id, text, thread_id, created_at, item_source, item_ext_id) VALUES(?1,?2,?3,?4,?5,?6)");
    s.bind(1, entity_id).bind(2, text).bind(3, thread_id).bind(4, now).bind(5, item_source).bind(6, item_ext_id);
    if (!s.run()) return 0;
    return db_->last_insert_id();
}

std::vector<FactRow> KgStore::facts_of(long long entity_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kFactColumns + " FROM facts_user WHERE entity_id=?1 ORDER BY created_at DESC, id DESC");
    s.bind(1, entity_id);
    std::vector<FactRow> out;
    while (s.step()) out.push_back(read_fact(s));
    return out;
}

std::optional<FactRow> KgStore::fact(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kFactColumns + " FROM facts_user WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_fact(s);
}

bool KgStore::update_fact(long long id, const std::string& text) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("UPDATE facts_user SET text=?2 WHERE id=?1");
    s.bind(1, id).bind(2, text);
    (void)s.run();
    return db_->changes() > 0;
}

bool KgStore::delete_fact(long long id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM facts_user WHERE id=?1");
    s.bind(1, id);
    (void)s.run();
    const bool removed = db_->changes() > 0;
    if (removed) delete_orphan_entities(*db_);
    return removed;
}

std::vector<FactRow> KgStore::facts_of_item(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kFactColumns + " FROM facts_user WHERE item_source=?1 AND item_ext_id=?2 ORDER BY id");
    s.bind(1, source).bind(2, ext_id);
    std::vector<FactRow> out;
    while (s.step()) out.push_back(read_fact(s));
    return out;
}

std::vector<FactRow> KgStore::find_facts(const std::string& query, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<FactRow> out;
    auto s = db_->prepare(std::string("SELECT ") + kFactColumns + " FROM facts_user ORDER BY created_at DESC LIMIT 2000");
    const auto qw = words(normalize_name(query));
    std::vector<std::pair<int, FactRow>> scored;
    while (s.step()) {
        FactRow f = read_fact(s);
        const std::string n = " " + normalize_name(f.text) + " ";
        int hits = 0;
        for (const auto& w : qw)
            if (w.size() >= 3 && n.find(w) != std::string::npos) ++hits;
        if (hits > 0) scored.emplace_back(hits, std::move(f));
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [_, f] : scored) {
        if (static_cast<int>(out.size()) >= limit) break;
        out.push_back(std::move(f));
    }
    return out;
}

// ---- Budgets and meta ---------------------------------------------------------------------------------------------------

long long KgStore::used(const std::string& day, const std::string& kind) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT used FROM budget WHERE day=?1 AND kind=?2");
    s.bind(1, day).bind(2, kind);
    return s.step() ? s.col_int(0) : 0;
}

void KgStore::add_used(const std::string& day, const std::string& kind, long long n) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT INTO budget(day, kind, used) VALUES(?1,?2,?3) ON CONFLICT(day, kind) DO UPDATE SET used=used+?3");
    s.bind(1, day).bind(2, kind).bind(3, n);
    (void)s.run();
    auto d = db_->prepare("DELETE FROM budget WHERE day<>?1");
    d.bind(1, day);
    (void)d.run();
}

std::string KgStore::meta(const std::string& key) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT value FROM meta WHERE key=?1");
    s.bind(1, key);
    return s.step() ? s.col_text(0) : std::string{};
}

void KgStore::set_meta(const std::string& key, const std::string& value) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?1,?2)");
    s.bind(1, key).bind(2, value);
    (void)s.run();
}

// ---- Sharing ------------------------------------------------------------------------------------------------------------

namespace {
std::vector<std::string> groups_from(const std::string& text) {
    std::vector<std::string> out;
    const json j = json::parse(text, nullptr, false);
    if (j.is_array())
        for (const auto& g : j)
            if (g.is_string()) out.push_back(g.get<std::string>());
    return out;
}
constexpr const char* kShareColumns = "source, ext_id, remote_id, title, groups, item_hash, content_hash, size, state, error, rule, updated_at";
ShareRow read_share(const Statement& s) {
    return {s.col_text(0), s.col_text(1), s.col_text(2), s.col_text(3), groups_from(s.col_text(4)), s.col_text(5),
            s.col_text(6), s.col_int(7), s.col_text(8), s.col_text(9), s.col_text(10), s.col_int(11)};
}
}  // namespace

std::vector<ShareRow> KgStore::shares() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kShareColumns + " FROM shares ORDER BY title");
    std::vector<ShareRow> out;
    while (s.step()) out.push_back(read_share(s));
    return out;
}

std::optional<ShareRow> KgStore::share(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kShareColumns + " FROM shares WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    if (!s.step()) return std::nullopt;
    return read_share(s);
}

void KgStore::put_share(const ShareRow& r) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("INSERT OR REPLACE INTO shares(") + kShareColumns + ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
    s.bind(1, r.source).bind(2, r.ext_id).bind(3, r.remote_id).bind(4, r.title).bind(5, json(r.groups).dump()).bind(6, r.item_hash);
    s.bind(7, r.content_hash).bind(8, r.size).bind(9, r.state).bind(10, r.error).bind(11, r.rule).bind(12, r.updated_at);
    (void)s.run();
}

void KgStore::remove_share(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM shares WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    (void)s.run();
}

std::vector<std::pair<std::string, std::vector<std::string>>> KgStore::share_rules() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT folder, groups FROM share_rules ORDER BY folder");
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    while (s.step()) out.emplace_back(s.col_text(0), groups_from(s.col_text(1)));
    return out;
}

Status KgStore::put_share_rule(const std::string& folder, const std::vector<std::string>& groups) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("INSERT OR REPLACE INTO share_rules(folder, groups) VALUES(?1,?2)");
    s.bind(1, folder).bind(2, json(groups).dump());
    return s.run();
}

void KgStore::remove_share_rule(const std::string& folder) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM share_rules WHERE folder=?1");
    s.bind(1, folder);
    (void)s.run();
}

void KgStore::clear(bool all) {
    const std::lock_guard lock(db_->mutex());
    // The day's budgets stay: forgetting must not let backfill spend the budget a second time.
    (void)db_->exec("DELETE FROM items; DELETE FROM entities; DELETE FROM skipped; DELETE FROM meta; DELETE FROM fact_tombstones;");
    if (all)
        (void)db_->exec("DELETE FROM sources; DELETE FROM folders; DELETE FROM user_exclusions; DELETE FROM documents; DELETE FROM notes; "
                        "DELETE FROM shares; DELETE FROM share_rules;");
}

}  // namespace wb::knowledge
