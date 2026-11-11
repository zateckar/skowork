#include "knowledge/kg_store.h"

#include "base/strings.h"
#include "platform/crypto.h"

#include <sqlite3mc_amalgamation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <map>
#include <limits>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>

namespace wb::knowledge {

Status register_kg_writer(Database& db) {
    // Removing a required fact can remove another support set, whose now-unsupported fact can in turn be required
    // by further derivations. Recursive triggers are connection-local and must be enabled on every modern writer.
    if (auto status = db.exec("PRAGMA recursive_triggers=ON"); !status) return status;
    const int rc = sqlite3_create_function_v2(db.raw(), "skowork_writer_v1", 0, SQLITE_UTF8 | SQLITE_INNOCUOUS,
                                              nullptr, [](sqlite3_context* ctx, int, sqlite3_value**) { sqlite3_result_int(ctx, 1); },
                                              nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return make_error("sqlite", db.errmsg());
    return {};
}

std::string kg_writer_barrier_sql() {
    std::string sql;
    for (const char* table : {"sources", "folders", "user_exclusions", "skipped", "items", "chunks", "embed_queue",
                              "extract_queue", "entities", "entity_aliases", "mentions", "relations", "facts_user",
                              "budget", "meta", "vec_models", "vec_bin", "vec_i8", "documents", "notes",
                              "fact_tombstones", "shares", "share_rules"}) {
        for (const char* verb : {"INSERT", "UPDATE", "DELETE"})
            sql += std::string("CREATE TRIGGER skowork_guard_") + table + "_" + verb + " BEFORE " + verb + " ON " + table +
                   " BEGIN SELECT skowork_writer_v1(); END;\n";
    }
    return sql;
}

std::string kg_evidence_migration_sql() {
    // A legacy relation records one passage, not the exact words or every input used by the model. Backfill it as
    // unverified passage-level evidence; do not invent precise spans or claim that the passage is a complete proof.
    return std::string(R"sql(
CREATE TABLE knowledge_revisions(
  id INTEGER PRIMARY KEY,
  item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
  sequence INTEGER NOT NULL,
  source_hash TEXT NOT NULL,
  indexed_at INTEGER NOT NULL,
  current INTEGER NOT NULL CHECK(current IN (0,1)),
  UNIQUE(item_id,sequence));
CREATE UNIQUE INDEX knowledge_revision_current ON knowledge_revisions(item_id) WHERE current=1;
CREATE TABLE knowledge_evidence(
  id INTEGER PRIMARY KEY,
  revision_id INTEGER NOT NULL REFERENCES knowledge_revisions(id) ON DELETE CASCADE,
  chunk_id INTEGER REFERENCES chunks(id) ON DELETE SET NULL,
  excerpt TEXT NOT NULL,
  precision TEXT NOT NULL CHECK(precision IN ('passage','exact')),
  byte_start INTEGER,
  byte_end INTEGER,
  CHECK((precision='passage' AND byte_start IS NULL AND byte_end IS NULL) OR
        (precision='exact' AND byte_start IS NOT NULL AND byte_end IS NOT NULL AND byte_start>=0 AND byte_end>byte_start)));
CREATE INDEX knowledge_evidence_revision ON knowledge_evidence(revision_id,chunk_id);
-- Deleting a passage clears its evidence references; without this every deletion scans all evidence.
CREATE INDEX knowledge_evidence_chunk ON knowledge_evidence(chunk_id);
CREATE UNIQUE INDEX knowledge_passage_identity ON knowledge_evidence(revision_id,chunk_id) WHERE precision='passage';
CREATE UNIQUE INDEX knowledge_exact_identity ON knowledge_evidence(revision_id,chunk_id,byte_start,byte_end) WHERE precision='exact';
CREATE TRIGGER knowledge_evidence_validate_insert BEFORE INSERT ON knowledge_evidence
WHEN new.chunk_id IS NULL OR NOT EXISTS (
  SELECT 1 FROM chunks c JOIN knowledge_revisions v ON v.item_id=c.item_id
  WHERE c.id=new.chunk_id AND v.id=new.revision_id AND
    ((new.precision='passage' AND new.excerpt=c.text) OR
     (new.precision='exact' AND substr(CAST(c.text AS BLOB),new.byte_start+1,new.byte_end-new.byte_start)=CAST(new.excerpt AS BLOB)))
)
BEGIN SELECT RAISE(ABORT,'evidence does not match its revision and passage'); END;
CREATE TRIGGER knowledge_evidence_immutable BEFORE UPDATE OF revision_id,excerpt,precision,byte_start,byte_end ON knowledge_evidence
BEGIN SELECT RAISE(ABORT,'replace evidence instead of changing it'); END;
CREATE TRIGGER knowledge_evidence_chunk_immutable BEFORE UPDATE OF chunk_id ON knowledge_evidence
WHEN new.chunk_id IS NOT NULL AND new.chunk_id IS NOT old.chunk_id
BEGIN SELECT RAISE(ABORT,'replace evidence instead of changing its passage'); END;
CREATE TABLE knowledge_facts(
  id INTEGER PRIMARY KEY,
  subject INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  predicate TEXT NOT NULL,
  object INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  unit TEXT NOT NULL DEFAULT '',
  scope TEXT NOT NULL DEFAULT '',
  valid_from TEXT NOT NULL DEFAULT '',
  valid_to TEXT NOT NULL DEFAULT '',
  UNIQUE(subject,predicate,object,unit,scope,valid_from,valid_to));
CREATE TABLE knowledge_support_sets(
  id INTEGER PRIMARY KEY,
  fact_id INTEGER NOT NULL REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  legacy_relation_id INTEGER UNIQUE REFERENCES relations(id) ON DELETE SET NULL,
  state TEXT NOT NULL CHECK(state IN ('unverified','current','superseded','unsupported')));
CREATE INDEX knowledge_support_fact ON knowledge_support_sets(fact_id,state);
CREATE TABLE knowledge_support_evidence(
  support_id INTEGER NOT NULL REFERENCES knowledge_support_sets(id) ON DELETE CASCADE,
  evidence_id INTEGER NOT NULL REFERENCES knowledge_evidence(id) ON DELETE CASCADE,
  PRIMARY KEY(support_id,evidence_id));
CREATE INDEX knowledge_evidence_reverse ON knowledge_support_evidence(evidence_id,support_id);
CREATE TABLE knowledge_support_facts(
  support_id INTEGER NOT NULL REFERENCES knowledge_support_sets(id) ON DELETE CASCADE,
  required_fact_id INTEGER NOT NULL REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  PRIMARY KEY(support_id,required_fact_id));
CREATE INDEX knowledge_fact_reverse ON knowledge_support_facts(required_fact_id,support_id);
CREATE TRIGGER knowledge_no_dependency_cycle BEFORE INSERT ON knowledge_support_facts
WHEN EXISTS(
  WITH RECURSIVE required(id) AS (
    SELECT new.required_fact_id
    UNION
    SELECT d.required_fact_id FROM required r
      JOIN knowledge_support_sets s ON s.fact_id=r.id
      JOIN knowledge_support_facts d ON d.support_id=s.id
  )
  SELECT 1 FROM required WHERE id=(SELECT fact_id FROM knowledge_support_sets WHERE id=new.support_id)
)
BEGIN SELECT RAISE(ABORT,'cyclic knowledge dependency'); END;
-- Invalidation and forgetting follow required facts through nested triggers. Bounding the longest chain to 32 facts
-- (kMaxDerivationDepth) keeps every cascade far below SQLite's trigger depth, so a forget cannot fail on a deep graph.
CREATE TRIGGER knowledge_dependency_depth BEFORE INSERT ON knowledge_support_facts
WHEN (WITH RECURSIVE below(id,d) AS (
        SELECT new.required_fact_id,1
        UNION
        SELECT x.required_fact_id,b.d+1 FROM below b
          JOIN knowledge_support_sets s ON s.fact_id=b.id
          JOIN knowledge_support_facts x ON x.support_id=s.id WHERE b.d<=32)
      SELECT max(d) FROM below)
   + (WITH RECURSIVE above(id,d) AS (
        SELECT fact_id,1 FROM knowledge_support_sets WHERE id=new.support_id
        UNION
        SELECT s.fact_id,a.d+1 FROM above a
          JOIN knowledge_support_facts x ON x.required_fact_id=a.id
          JOIN knowledge_support_sets s ON s.id=x.support_id WHERE a.d<=32)
      SELECT max(d) FROM above) > 32
BEGIN SELECT RAISE(ABORT,'knowledge dependency chain is too deep'); END;
CREATE TRIGGER knowledge_immutable_dependency BEFORE UPDATE ON knowledge_support_facts
BEGIN SELECT RAISE(ABORT,'replace knowledge dependencies instead of updating them'); END;
CREATE TABLE knowledge_annotations(
  user_fact_id INTEGER PRIMARY KEY REFERENCES facts_user(id) ON DELETE CASCADE,
  revision_id INTEGER REFERENCES knowledge_revisions(id) ON DELETE CASCADE);
CREATE TABLE knowledge_categories(
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  normalized_name TEXT NOT NULL UNIQUE);
CREATE TABLE knowledge_category_assignments(
  fact_id INTEGER NOT NULL REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  category_id INTEGER NOT NULL REFERENCES knowledge_categories(id) ON DELETE CASCADE,
  decision TEXT NOT NULL CHECK(decision IN ('automatic','manual','rejected')),
  reason TEXT NOT NULL DEFAULT '',
  evidence_id INTEGER REFERENCES knowledge_evidence(id) ON DELETE CASCADE,
  CHECK((decision='automatic' AND evidence_id IS NOT NULL AND reason<>'') OR
        (decision IN ('manual','rejected') AND evidence_id IS NULL)),
  PRIMARY KEY(fact_id,category_id));
CREATE INDEX knowledge_category_members ON knowledge_category_assignments(category_id,decision,fact_id);
CREATE TABLE knowledge_conflicts(
  id INTEGER PRIMARY KEY,
  left_fact INTEGER NOT NULL REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  right_fact INTEGER NOT NULL REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  left_evidence INTEGER NOT NULL REFERENCES knowledge_evidence(id) ON DELETE CASCADE,
  right_evidence INTEGER NOT NULL REFERENCES knowledge_evidence(id) ON DELETE CASCADE,
  explanation TEXT NOT NULL,
  state TEXT NOT NULL CHECK(state IN ('open','needs_recheck','resolved','deferred')),
  decision TEXT NOT NULL DEFAULT '',
  decision_reason TEXT NOT NULL DEFAULT '',
  CHECK(left_fact<right_fact),
  UNIQUE(left_fact,right_fact));
CREATE INDEX knowledge_conflict_left_evidence ON knowledge_conflicts(left_evidence);
CREATE INDEX knowledge_conflict_right_evidence ON knowledge_conflicts(right_evidence);
CREATE INDEX knowledge_conflict_right_fact ON knowledge_conflicts(right_fact);
CREATE INDEX knowledge_category_evidence ON knowledge_category_assignments(evidence_id);
CREATE INDEX knowledge_annotation_revision ON knowledge_annotations(revision_id);
CREATE TABLE knowledge_conflict_history(
  id INTEGER PRIMARY KEY,
  conflict_id INTEGER NOT NULL REFERENCES knowledge_conflicts(id) ON DELETE CASCADE,
  left_evidence INTEGER NOT NULL REFERENCES knowledge_evidence(id) ON DELETE CASCADE,
  right_evidence INTEGER NOT NULL REFERENCES knowledge_evidence(id) ON DELETE CASCADE,
  decision TEXT NOT NULL,
  reason TEXT NOT NULL,
  decided_at INTEGER NOT NULL);
CREATE INDEX knowledge_history_conflict ON knowledge_conflict_history(conflict_id);
CREATE INDEX knowledge_history_left_evidence ON knowledge_conflict_history(left_evidence);
CREATE INDEX knowledge_history_right_evidence ON knowledge_conflict_history(right_evidence);
CREATE TABLE knowledge_previews(
  token TEXT PRIMARY KEY,
  operation TEXT NOT NULL CHECK(operation='forget_item'),
  item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
  generation INTEGER NOT NULL,
  expires_at INTEGER NOT NULL);
CREATE TABLE knowledge_generation(id INTEGER PRIMARY KEY CHECK(id=1), value INTEGER NOT NULL);
INSERT INTO knowledge_generation(id,value) VALUES(1,0);
-- Every passage sent in one extraction request is an input of that request. result=1 marks the request whose
-- findings a passage currently holds; item_date=1 when one of them took its date from the item.
CREATE TABLE knowledge_extraction_inputs(
  batch_id INTEGER NOT NULL,
  chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
  result INTEGER NOT NULL CHECK(result IN (0,1)),
  item_date INTEGER NOT NULL DEFAULT 0 CHECK(item_date IN (0,1)),
  PRIMARY KEY(batch_id,chunk_id)) WITHOUT ROWID;
CREATE UNIQUE INDEX knowledge_extraction_result ON knowledge_extraction_inputs(chunk_id) WHERE result=1;
CREATE INDEX knowledge_extraction_member ON knowledge_extraction_inputs(chunk_id,batch_id);

INSERT INTO knowledge_revisions(item_id,sequence,source_hash,indexed_at,current)
  SELECT id,1,hash,indexed_at,1 FROM items;
INSERT INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision)
  SELECT DISTINCT v.id,c.id,c.text,'passage' FROM relations r
  JOIN chunks c ON c.id=r.chunk_id JOIN knowledge_revisions v ON v.item_id=c.item_id AND v.current=1;
INSERT OR IGNORE INTO knowledge_facts(subject,predicate,object,valid_from,valid_to)
  SELECT subject,predicate,object,valid_from,valid_to FROM relations WHERE chunk_id IS NOT NULL;
INSERT INTO knowledge_support_sets(fact_id,legacy_relation_id,state)
  SELECT f.id,min(r.id),'unverified' FROM relations r
  JOIN knowledge_facts f ON f.subject=r.subject AND f.predicate=r.predicate AND f.object=r.object
    AND f.unit='' AND f.scope='' AND f.valid_from=r.valid_from AND f.valid_to=r.valid_to
  WHERE r.chunk_id IS NOT NULL GROUP BY f.id,r.chunk_id;
INSERT INTO knowledge_support_evidence(support_id,evidence_id)
  SELECT s.id,e.id FROM knowledge_support_sets s JOIN relations r ON r.id=s.legacy_relation_id
  JOIN chunks c ON c.id=r.chunk_id JOIN knowledge_revisions v ON v.item_id=c.item_id AND v.current=1
  JOIN knowledge_evidence e ON e.revision_id=v.id AND e.chunk_id=c.id;
INSERT INTO knowledge_annotations(user_fact_id,revision_id)
  SELECT u.id,v.id FROM facts_user u
  LEFT JOIN items i ON (i.source=u.item_source AND i.ext_id=u.item_ext_id AND u.item_source<>'') OR
                       (i.source='threads' AND i.ext_id=u.thread_id AND u.item_source='' AND u.thread_id<>'')
  LEFT JOIN knowledge_revisions v ON v.item_id=i.id AND v.current=1;

CREATE TRIGGER knowledge_legacy_relation_removed BEFORE DELETE ON relations BEGIN
  UPDATE knowledge_support_sets SET legacy_relation_id=(
    SELECT min(r.id) FROM relations r WHERE r.id<>old.id AND r.chunk_id=old.chunk_id
      AND r.subject=old.subject AND r.predicate=old.predicate AND r.object=old.object
      AND r.valid_from=old.valid_from AND r.valid_to=old.valid_to),
    state=CASE WHEN EXISTS(SELECT 1 FROM relations r WHERE r.id<>old.id AND r.chunk_id=old.chunk_id
      AND r.subject=old.subject AND r.predicate=old.predicate AND r.object=old.object
      AND r.valid_from=old.valid_from AND r.valid_to=old.valid_to) OR state='superseded' THEN state ELSE 'unsupported' END
    WHERE legacy_relation_id=old.id;
END;
CREATE TRIGGER knowledge_forget_evidence BEFORE DELETE ON knowledge_evidence BEGIN
  DELETE FROM knowledge_support_sets WHERE id IN
    (SELECT support_id FROM knowledge_support_evidence WHERE evidence_id=old.id);
END;
CREATE TRIGGER knowledge_forget_required_fact BEFORE DELETE ON knowledge_facts BEGIN
  DELETE FROM knowledge_support_sets WHERE id IN
    (SELECT support_id FROM knowledge_support_facts WHERE required_fact_id=old.id);
END;
CREATE TRIGGER knowledge_support_member_removed AFTER DELETE ON knowledge_support_evidence BEGIN
  DELETE FROM knowledge_support_sets WHERE id=old.support_id;
END;
CREATE TRIGGER knowledge_required_member_removed AFTER DELETE ON knowledge_support_facts BEGIN
  DELETE FROM knowledge_support_sets WHERE id=old.support_id;
END;
CREATE TRIGGER knowledge_supersede_revision AFTER UPDATE OF current ON knowledge_revisions
WHEN old.current=1 AND new.current=0
BEGIN
  UPDATE knowledge_support_sets SET state='superseded' WHERE state IN ('current','unverified') AND id IN
    (SELECT se.support_id FROM knowledge_support_evidence se
     JOIN knowledge_evidence e ON e.id=se.evidence_id WHERE e.revision_id=old.id);
END;
CREATE TRIGGER knowledge_invalidated_support AFTER UPDATE OF state ON knowledge_support_sets
WHEN new.state IN ('superseded','unsupported') AND old.state IN ('current','unverified')
BEGIN
  DELETE FROM knowledge_category_assignments WHERE fact_id=new.fact_id AND decision='automatic'
    AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id
      WHERE s.fact_id=new.fact_id AND s.state IN ('current','unverified')
        AND se.evidence_id=knowledge_category_assignments.evidence_id);
  UPDATE knowledge_conflicts SET state='needs_recheck',decision='',decision_reason=''
    WHERE state<>'needs_recheck' AND
      ((left_fact=new.fact_id AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id
        WHERE s.fact_id=new.fact_id AND s.state IN ('current','unverified') AND se.evidence_id=knowledge_conflicts.left_evidence)) OR
       (right_fact=new.fact_id AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id
        WHERE s.fact_id=new.fact_id AND s.state IN ('current','unverified') AND se.evidence_id=knowledge_conflicts.right_evidence)));
  UPDATE knowledge_support_sets SET state='unsupported' WHERE state IN ('current','unverified')
    AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets WHERE fact_id=new.fact_id AND state IN ('current','unverified')) AND id IN
    (SELECT support_id FROM knowledge_support_facts WHERE required_fact_id=new.fact_id);
END;
CREATE TRIGGER knowledge_removed_support AFTER DELETE ON knowledge_support_sets
BEGIN
  DELETE FROM knowledge_category_assignments WHERE fact_id=old.fact_id AND decision='automatic'
    AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id
      WHERE s.fact_id=old.fact_id AND s.state IN ('current','unverified')
        AND se.evidence_id=knowledge_category_assignments.evidence_id);
  UPDATE knowledge_conflicts SET state='needs_recheck',decision='',decision_reason=''
    WHERE state<>'needs_recheck' AND
      ((left_fact=old.fact_id AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id
        WHERE s.fact_id=old.fact_id AND s.state IN ('current','unverified') AND se.evidence_id=knowledge_conflicts.left_evidence)) OR
       (right_fact=old.fact_id AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id
        WHERE s.fact_id=old.fact_id AND s.state IN ('current','unverified') AND se.evidence_id=knowledge_conflicts.right_evidence)));
  UPDATE knowledge_support_sets SET state='unsupported' WHERE state IN ('current','unverified')
    AND NOT EXISTS(SELECT 1 FROM knowledge_support_sets WHERE fact_id=old.fact_id AND state IN ('current','unverified')) AND id IN
    (SELECT support_id FROM knowledge_support_facts WHERE required_fact_id=old.fact_id);
END;
CREATE TRIGGER knowledge_empty_fact AFTER DELETE ON knowledge_support_sets BEGIN
  DELETE FROM knowledge_facts WHERE id=old.fact_id AND NOT EXISTS
    (SELECT 1 FROM knowledge_support_sets WHERE fact_id=old.fact_id);
END;
)sql") + [] {
        // A removed or re-dated passage was shown to the model beside others: their findings are rechecked too.
        const auto recheck = [](const std::string& name, const std::string& event, const std::string& self) {
            const std::string batches = "(SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id=old.id)";
            const std::string affected = "(SELECT d.chunk_id FROM knowledge_extraction_inputs d WHERE d.result=1" + self +
                                         " AND d.batch_id IN " + batches + ")";
            return "CREATE TRIGGER " + name + " " + event + " BEGIN\n"
                   "  DELETE FROM relations WHERE chunk_id IN " + affected + ";\n"
                   "  DELETE FROM mentions WHERE chunk_id IN " + affected + ";\n"
                   "  INSERT OR IGNORE INTO extract_queue(chunk_id,priority) SELECT d.chunk_id,1 FROM knowledge_extraction_inputs d"
                   " WHERE d.result=1" + self + " AND d.batch_id IN " + batches + ";\n"
                   "  UPDATE knowledge_extraction_inputs SET result=0,item_date=0 WHERE result=1" +
                   (self.empty() ? std::string() : " AND chunk_id<>old.id") + " AND batch_id IN " + batches + ";\n"
                   "  DELETE FROM knowledge_extraction_inputs WHERE batch_id IN " + batches +
                   " AND NOT EXISTS(SELECT 1 FROM knowledge_extraction_inputs r"
                   " WHERE r.batch_id=knowledge_extraction_inputs.batch_id AND r.result=1 AND r.chunk_id<>old.id);\n"
                   "END;\n";
        };
        return recheck("knowledge_extraction_input_removed", "BEFORE DELETE ON chunks", " AND d.chunk_id<>old.id") +
               recheck("knowledge_extraction_input_redated", "AFTER UPDATE OF at ON chunks WHEN old.at IS NOT new.at", "");
    }() + kg_writer_barrier_sql() + [] {
        std::string sql;
        for (const char* table : {"sources", "folders", "documents", "user_exclusions", "skipped", "items", "chunks",
                                  "entities", "entity_aliases", "mentions", "relations", "facts_user", "notes", "shares",
                                  "share_rules", "fact_tombstones", "knowledge_revisions", "knowledge_evidence", "knowledge_facts",
                                  "knowledge_support_sets", "knowledge_support_evidence", "knowledge_support_facts",
                                  "knowledge_annotations", "knowledge_categories", "knowledge_category_assignments",
                                  "knowledge_conflicts", "knowledge_conflict_history"}) {
            for (const char* verb : {"INSERT", "UPDATE", "DELETE"})
                sql += std::string("CREATE TRIGGER knowledge_generation_") + table + "_" + verb + " AFTER " + verb +
                       " ON " + table + " BEGIN UPDATE knowledge_generation SET value=value+1 WHERE id=1; END;\n";
        }
        return sql;
    }();
}

Result<std::unique_ptr<Database>> open_kg_database(const std::filesystem::path& file, const db_key& key, MigrationOutcome& outcome) {
    return open_and_migrate_consistently(file, key, kg_migrations(), outcome, register_kg_writer);
}

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
        // A passage can mention one entity by several names, and a merge brings two names of one passage together:
        // a mention is keyed by its name too, so neither is lost and a split can move each back (design D6).
        {5, R"sql(
CREATE TABLE mentions_by_name(
  chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
  entity_id INTEGER NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
  alias TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(chunk_id, entity_id, alias));
INSERT INTO mentions_by_name(chunk_id, entity_id, alias) SELECT chunk_id, entity_id, alias FROM mentions;
DROP TABLE mentions;
ALTER TABLE mentions_by_name RENAME TO mentions;
CREATE INDEX mentions_entity ON mentions(entity_id, alias);
)sql"},
        // improve-skowork-and-knowledge-control: revisions, exact evidence, canonical facts with independent and
        // required supports, categories, review and generation fences (design D6, task 7.6).
        {kKgEvidenceSchemaVersion, kg_evidence_migration_sql()},
        // Task 8.1: which facts automatic categorization has checked. A fact is checked again when a support gains
        // evidence (a new or carried-forward excerpt) or when a category is added or renamed; a check is progress,
        // not knowledge, so it does not change the knowledge generation.
        {kKgCategoryChecksSchemaVersion, R"sql(
CREATE TABLE knowledge_category_checks(
  fact_id INTEGER PRIMARY KEY REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  checked_at INTEGER NOT NULL);
CREATE TRIGGER knowledge_category_recheck_evidence AFTER INSERT ON knowledge_support_evidence BEGIN
  DELETE FROM knowledge_category_checks WHERE fact_id=(SELECT fact_id FROM knowledge_support_sets WHERE id=new.support_id);
END;
CREATE TRIGGER knowledge_category_recheck_added AFTER INSERT ON knowledge_categories BEGIN
  DELETE FROM knowledge_category_checks;
END;
CREATE TRIGGER knowledge_category_recheck_renamed AFTER UPDATE OF name ON knowledge_categories BEGIN
  DELETE FROM knowledge_category_checks;
END;
)sql"},
        // Task 8.2: which facts consistency analysis has checked, the kind of a possible inconsistency, and whether a
        // review that needed a recheck was compared again without being found. An excerpt that analysis binds inside a
        // passage the same support already cites is not new evidence: it clears neither check, so categorization and
        // consistency analysis cannot keep invalidating each other's progress.
        {kKgConsistencySchemaVersion, R"sql(
CREATE TABLE knowledge_consistency_checks(
  fact_id INTEGER PRIMARY KEY REFERENCES knowledge_facts(id) ON DELETE CASCADE,
  checked_at INTEGER NOT NULL);
CREATE INDEX knowledge_fact_object ON knowledge_facts(object,predicate);
ALTER TABLE knowledge_conflicts ADD COLUMN kind TEXT NOT NULL DEFAULT 'conflict' CHECK(kind IN ('conflict','uncertain_entity'));
ALTER TABLE knowledge_conflicts ADD COLUMN cleared INTEGER NOT NULL DEFAULT 0 CHECK(cleared IN (0,1));
DROP TRIGGER knowledge_category_recheck_evidence;
CREATE TRIGGER knowledge_analysis_recheck_evidence AFTER INSERT ON knowledge_support_evidence
WHEN NOT EXISTS(SELECT 1 FROM knowledge_evidence n
  JOIN knowledge_support_evidence se ON se.support_id=new.support_id AND se.evidence_id<>n.id
  JOIN knowledge_evidence o ON o.id=se.evidence_id
  WHERE n.id=new.evidence_id AND n.precision='exact' AND o.revision_id=n.revision_id AND o.chunk_id=n.chunk_id)
BEGIN
  DELETE FROM knowledge_category_checks WHERE fact_id=(SELECT fact_id FROM knowledge_support_sets WHERE id=new.support_id);
  DELETE FROM knowledge_consistency_checks WHERE fact_id=(SELECT fact_id FROM knowledge_support_sets WHERE id=new.support_id);
END;
CREATE TRIGGER knowledge_consistency_recheck_review AFTER UPDATE OF state ON knowledge_conflicts
WHEN new.state='needs_recheck' AND old.state<>'needs_recheck'
BEGIN
  UPDATE knowledge_conflicts SET cleared=0 WHERE id=new.id;
  DELETE FROM knowledge_consistency_checks WHERE fact_id IN (new.left_fact,new.right_fact);
END;
)sql"},
        // Task 8.3: a preview of an explicit import, update, exclusion or forget, bound to the knowledge generation and
        // policy it was made under. An import's reviewed passages are kept here (the database is encrypted) until it is
        // confirmed, cancelled or expires; its entries are the affected facts and passages, read page by page. Previews
        // are not knowledge, so they do not change the generation.
        {kKgPreviewSchemaVersion, R"sql(
CREATE TABLE knowledge_operation_previews(
  token TEXT PRIMARY KEY,
  operation TEXT NOT NULL CHECK(operation IN ('import','update','exclude','forget')),
  target TEXT NOT NULL,
  content TEXT NOT NULL DEFAULT '',
  content_hash TEXT NOT NULL DEFAULT '',
  policy TEXT NOT NULL,
  summary TEXT NOT NULL,
  generation INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL);
CREATE TABLE knowledge_preview_entries(
  token TEXT NOT NULL REFERENCES knowledge_operation_previews(token) ON DELETE CASCADE,
  kind TEXT NOT NULL CHECK(kind IN ('fact','passage')),
  ord INTEGER NOT NULL,
  ref INTEGER NOT NULL,
  effect TEXT NOT NULL,
  before INTEGER NOT NULL DEFAULT 0,
  after INTEGER NOT NULL DEFAULT 0,
  text TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(token,kind,ord)) WITHOUT ROWID;
)sql"},
        // Task 8.4: a durable change report per revision of an indexed item, written with the revision. Ids and counts
        // only, no content; it goes with its item. Reports are not knowledge, so they do not change the generation.
        {kKgSchemaVersion, R"sql(
CREATE TABLE knowledge_change_reports(
  id INTEGER PRIMARY KEY,
  item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
  revision INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  added INTEGER NOT NULL DEFAULT 0,
  kept INTEGER NOT NULL DEFAULT 0,
  redated INTEGER NOT NULL DEFAULT 0,
  rechecked INTEGER NOT NULL DEFAULT 0,
  removed INTEGER NOT NULL DEFAULT 0,
  facts INTEGER NOT NULL DEFAULT 0,
  unsupported INTEGER NOT NULL DEFAULT 0);
CREATE INDEX knowledge_change_reports_item ON knowledge_change_reports(item_id,id);
CREATE INDEX knowledge_change_reports_time ON knowledge_change_reports(created_at);
CREATE TABLE knowledge_change_report_facts(
  report_id INTEGER NOT NULL REFERENCES knowledge_change_reports(id) ON DELETE CASCADE,
  fact_id INTEGER NOT NULL,
  before INTEGER NOT NULL,
  after INTEGER NOT NULL,
  PRIMARY KEY(report_id,fact_id)) WITHOUT ROWID;
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

Status delete_orphan_entities(Database& db) {
    std::string sql = "DELETE FROM entities WHERE id NOT IN (SELECT entity_id FROM mentions) AND id NOT IN (SELECT entity_id FROM facts_user) "
                      "AND id NOT IN (SELECT subject FROM relations) AND id NOT IN (SELECT object FROM relations)";
    if (db.user_version() >= kKgEvidenceSchemaVersion)
        sql += " AND id NOT IN (SELECT subject FROM knowledge_facts) AND id NOT IN (SELECT object FROM knowledge_facts)";
    return db.exec(sql);
}

// Versioned evidence (design D6), called inside the transaction that replaces an item's passages and before any
// passage is dropped. Evidence is immutable, so an unchanged passage's evidence is copied to the replacement
// revision and every live reference follows the copy. What still references the previous revision when it stops
// being current is superseded by the schema; the previous rows remain as that revision's history.
Status advance_revision(Database& db, long long item, const std::string& hash, long long now, bool changed,
                        const std::vector<long long>& unchanged_chunks) {
    long long previous = 0;
    {
        auto current = db.prepare("SELECT id FROM knowledge_revisions WHERE item_id=?1 AND current=1");
        current.bind(1, item);
        if (current.step()) previous = current.col_int(0);
    }
    if (previous && !changed) return {};
    auto insert = db.prepare("INSERT INTO knowledge_revisions(item_id,sequence,source_hash,indexed_at,current) "
                             "SELECT ?1,ifnull(max(sequence),0)+1,?2,?3,?4 FROM knowledge_revisions WHERE item_id=?1");
    insert.bind(1, item).bind(2, hash).bind(3, now).bind(4, previous ? 0 : 1);
    if (Status s = insert.run(); !s) return s;
    if (!previous) return {};
    const std::string next = std::to_string(db.last_insert_id()), old = std::to_string(previous);
    if (!unchanged_chunks.empty()) {
        if (Status s = db.exec(
                "INSERT INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision,byte_start,byte_end) "
                "SELECT " + next + ",e.chunk_id,e.excerpt,e.precision,e.byte_start,e.byte_end FROM knowledge_evidence e "
                "JOIN chunks c ON c.id=e.chunk_id WHERE e.revision_id=" + old + " AND e.chunk_id IN (" + id_list(unchanged_chunks) + ") "
                "AND ((e.precision='passage' AND e.excerpt=c.text) OR (e.precision='exact' AND "
                "substr(CAST(c.text AS BLOB),e.byte_start+1,e.byte_end-e.byte_start)=CAST(e.excerpt AS BLOB)))");
            !s)
            return s;
        const std::string pair = "FROM knowledge_evidence o JOIN knowledge_evidence n ON n.revision_id=" + next +
                                 " AND n.chunk_id=o.chunk_id AND n.precision=o.precision AND n.byte_start IS o.byte_start"
                                 " AND n.byte_end IS o.byte_end WHERE o.revision_id=" + old;
        for (const auto& [table, column] : std::vector<std::pair<std::string, std::string>>{
                 {"knowledge_support_evidence", "evidence_id"}, {"knowledge_category_assignments", "evidence_id"},
                 {"knowledge_conflicts", "left_evidence"}, {"knowledge_conflicts", "right_evidence"}}) {
            if (Status s = db.exec("UPDATE " + table + " SET " + column + "=(SELECT n.id " + pair + " AND o.id=" + table + "." +
                                   column + ") WHERE " + column + " IN (SELECT o.id " + pair + ")");
                !s)
                return s;
        }
    }
    // A correction or user-stated fact belongs to the item, not to one version of its text.
    if (Status s = db.exec("UPDATE knowledge_annotations SET revision_id=" + next + " WHERE revision_id=" + old); !s) return s;
    if (Status s = db.exec("UPDATE knowledge_revisions SET current=0 WHERE id=" + old); !s) return s;
    // A finding already withdrawn for recheck whose excerpt did not carry over is history of the old revision, the
    // same as one the schema just superseded. This is what a change withdrawn in advance leaves behind.
    if (Status s = db.exec("UPDATE knowledge_support_sets SET state='superseded' WHERE state='unsupported' AND id IN "
                           "(SELECT se.support_id FROM knowledge_support_evidence se JOIN knowledge_evidence e ON e.id=se.evidence_id "
                           "WHERE e.revision_id=" + old + ")");
        !s)
        return s;
    if (Status s = db.exec("UPDATE knowledge_revisions SET current=1 WHERE id=" + next); !s) return s;
    // History is bounded: older revisions go with their excerpts, superseded supports and review history.
    return db.exec("DELETE FROM knowledge_revisions WHERE item_id=" + std::to_string(item) + " AND current=0 AND sequence<"
                   "(SELECT sequence FROM knowledge_revisions WHERE id=" + next + ")-" + std::to_string(kRetainedSupersededRevisions));
}

// Records a newly extracted relation as passage-level, unverified support of its canonical fact, exactly as the
// migration backfills an existing one: the extractor names one passage, not the exact words or every input.
Status record_relation_support(Database& db, long long relation, long long subject, const std::string& predicate, long long object,
                               const std::string& valid_from, const std::string& valid_to, long long chunk_id) {
    long long revision = 0;
    std::string text;
    {
        auto passage = db.prepare("SELECT v.id,c.text FROM chunks c JOIN knowledge_revisions v ON v.item_id=c.item_id AND v.current=1 "
                                  "WHERE c.id=?1");
        passage.bind(1, chunk_id);
        if (!passage.step()) return make_error("storage", "the passage has no current revision");
        revision = passage.col_int(0);
        text = passage.col_text(1);
    }
    auto fact_insert = db.prepare("INSERT OR IGNORE INTO knowledge_facts(subject,predicate,object,valid_from,valid_to) VALUES(?1,?2,?3,?4,?5)");
    fact_insert.bind(1, subject).bind(2, predicate).bind(3, object).bind(4, valid_from).bind(5, valid_to);
    if (Status s = fact_insert.run(); !s) return s;
    long long fact = 0, evidence = 0;
    {
        auto find = db.prepare("SELECT id FROM knowledge_facts WHERE subject=?1 AND predicate=?2 AND object=?3 AND unit='' AND scope='' "
                               "AND valid_from=?4 AND valid_to=?5");
        find.bind(1, subject).bind(2, predicate).bind(3, object).bind(4, valid_from).bind(5, valid_to);
        if (!find.step()) return make_error("storage", "the fact could not be recorded");
        fact = find.col_int(0);
    }
    auto evidence_insert = db.prepare("INSERT OR IGNORE INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision) VALUES(?1,?2,?3,'passage')");
    evidence_insert.bind(1, revision).bind(2, chunk_id).bind(3, text);
    if (Status s = evidence_insert.run(); !s) return s;
    {
        auto find = db.prepare("SELECT id FROM knowledge_evidence WHERE revision_id=?1 AND chunk_id=?2 AND precision='passage'");
        find.bind(1, revision).bind(2, chunk_id);
        if (!find.step()) return make_error("storage", "the evidence could not be recorded");
        evidence = find.col_int(0);
    }
    // A duplicate relation from the same passage shares the existing support; the schema moves the link when the
    // linked relation is deleted. A support that lost its relation to a recheck is restored by the same finding.
    long long revive = 0;
    {
        auto same = db.prepare("SELECT s.id,s.legacy_relation_id IS NOT NULL FROM knowledge_support_sets s "
                               "JOIN knowledge_support_evidence se ON se.support_id=s.id "
                               "WHERE s.fact_id=?1 AND se.evidence_id=?2 AND (s.legacy_relation_id IS NOT NULL OR (s.state='unsupported' "
                               "AND NOT EXISTS(SELECT 1 FROM knowledge_support_facts d WHERE d.support_id=s.id))) "
                               "ORDER BY s.legacy_relation_id IS NULL,s.id LIMIT 1");
        same.bind(1, fact).bind(2, evidence);
        if (same.step()) {
            if (same.col_int(1)) return {};
            revive = same.col_int(0);
        }
    }
    if (revive) {
        auto restore = db.prepare("UPDATE knowledge_support_sets SET legacy_relation_id=?1,state='unverified' WHERE id=?2");
        restore.bind(1, relation).bind(2, revive);
        return restore.run();
    }
    auto support = db.prepare("INSERT INTO knowledge_support_sets(fact_id,legacy_relation_id,state) VALUES(?1,?2,'unverified')");
    support.bind(1, fact).bind(2, relation);
    if (Status s = support.run(); !s) return s;
    auto link = db.prepare("INSERT INTO knowledge_support_evidence(support_id,evidence_id) VALUES(?1,?2)");
    link.bind(1, db.last_insert_id()).bind(2, evidence);
    return link.run();
}

// Called inside the same transaction as the local forget. Retain only the identity needed to withdraw a share.
Status queue_item_withdrawals(Database& db, const std::string& source, const std::optional<std::string>& ext_id) {
    // Older clients inferred administrator removal from absence in a username-scoped list. It was not proof of removal.
    if (auto s = db.exec("UPDATE shares SET state='unverified',error='The previous sharing state cannot be verified.' "
                         "WHERE (state='removed' AND error='Removed from team knowledge by an administrator.') "
                         "OR (state='withdrawn' AND error<>'acknowledged-delete-v1')"); !s) return s;
    const std::string where = ext_id ? "source=?1 AND ext_id=?2" : "source=?1";
    auto missing = db.prepare("SELECT count(*) FROM shares WHERE " + where +
                              " AND state NOT IN ('removed','withdrawn') AND remote_id=''");
    missing.bind(1, source);
    if (ext_id) missing.bind(2, *ext_id);
    if (!missing.step()) return make_error("storage", "Unable to check the shared item withdrawal identity.");
    if (missing.col_int(0)) return make_error("storage", "A shared item has no withdrawal identity.");
    auto removed = db.prepare("DELETE FROM shares WHERE " + where + " AND state IN ('removed','withdrawn')");
    removed.bind(1, source);
    if (ext_id) removed.bind(2, *ext_id);
    if (auto s = removed.run(); !s) return s;
    auto pending = db.prepare("UPDATE shares SET ext_id='withdrawal:' || remote_id, title='', groups='[]', item_hash='', "
                              "content_hash='', size=0, state='unsharing', error='', rule='', updated_at=0 WHERE " + where);
    pending.bind(1, source);
    if (ext_id) pending.bind(2, *ext_id);
    return pending.run();
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

// ---- Categories ---------------------------------------------------------------------------------------------------------

Result<long long> KgStore::create_category(const std::string& name) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge categories are not available yet.");
    const auto display = trim(name);
    const auto normalized = normalize_name(display);
    if (display.empty() || display.size() > 160 || normalized.empty() || normalized == normalize_name(kUncategorized))
        return make_error("invalid", "Enter a valid category name.");  // Uncategorized is the filter for facts without one
    auto s = db_->prepare("INSERT INTO knowledge_categories(name,normalized_name) VALUES(?1,?2)");
    s.bind(1, display).bind(2, normalized);
    if (auto status = s.run(); !status) return status.error();
    return db_->last_insert_id();
}

Status KgStore::rename_category(long long id, const std::string& name) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge categories are not available yet.");
    const auto display = trim(name);
    const auto normalized = normalize_name(display);
    if (display.empty() || display.size() > 160 || normalized.empty() || normalized == normalize_name(kUncategorized))
        return make_error("invalid", "Enter a valid category name.");  // Uncategorized is the filter for facts without one
    auto s = db_->prepare("UPDATE knowledge_categories SET name=?2,normalized_name=?3 WHERE id=?1");
    s.bind(1, id).bind(2, display).bind(3, normalized);
    if (auto status = s.run(); !status) return status;
    return db_->changes() ? Status{} : Status(make_error("not_found", "No such category."));
}

Status KgStore::delete_category(long long id) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge categories are not available yet.");
    auto s = db_->prepare("DELETE FROM knowledge_categories WHERE id=?1");
    s.bind(1, id);
    if (auto status = s.run(); !status) return status;
    return db_->changes() ? Status{} : Status(make_error("not_found", "No such category."));
}

Status KgStore::merge_categories(long long source, long long destination) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge categories are not available yet.");
    if (source == destination) return make_error("invalid", "Choose two different categories.");
    return db_->transaction([&]() -> Status {
        auto count = db_->prepare("SELECT count(*) FROM knowledge_categories WHERE id=?1 OR id=?2");
        count.bind(1, source).bind(2, destination);
        if (!count.step() || count.col_int(0) != 2) return make_error("not_found", "No such category.");
        auto move = db_->prepare(R"sql(
INSERT INTO knowledge_category_assignments(fact_id,category_id,decision,reason,evidence_id)
 SELECT fact_id,?2,decision,reason,evidence_id FROM knowledge_category_assignments WHERE category_id=?1
 ON CONFLICT(fact_id,category_id) DO UPDATE SET decision=excluded.decision,reason=excluded.reason,evidence_id=excluded.evidence_id
 WHERE knowledge_category_assignments.decision='automatic' AND excluded.decision<>'automatic')sql");
        move.bind(1, source).bind(2, destination);
        if (auto status = move.run(); !status) return status;
        auto remove = db_->prepare("DELETE FROM knowledge_categories WHERE id=?1");
        remove.bind(1, source);
        return remove.run();
    });
}

namespace {

// The analysis writes below run inside a transaction their caller opened after checking its graph snapshot.

std::optional<long long> generation_of(Database& db) {
    auto s = db.prepare("SELECT value FROM knowledge_generation WHERE id=1");
    if (!s.step()) return std::nullopt;
    return s.col_int(0);
}

bool active_exact_evidence(Database& db, long long fact, long long evidence_id) {
    auto evidence = db.prepare("SELECT 1 FROM knowledge_support_sets s "
                               "JOIN knowledge_support_evidence se ON se.support_id=s.id "
                               "JOIN knowledge_evidence e ON e.id=se.evidence_id "
                               "JOIN knowledge_revisions v ON v.id=e.revision_id "
                               "JOIN chunks c ON c.id=e.chunk_id AND c.item_id=v.item_id "
                               "WHERE s.fact_id=?1 AND s.state IN ('current','unverified') AND v.current=1 "
                               "AND e.id=?2 AND e.precision='exact' AND "
                               "CAST(e.excerpt AS BLOB)=substr(CAST(c.text AS BLOB),e.byte_start+1,e.byte_end-e.byte_start)");
    evidence.bind(1, fact).bind(2, evidence_id);
    return evidence.step();
}

// An exact excerpt of a passage that belongs to a current support of the fact, in the current revision.
Result<long long> bind_span(Database& db, long long fact, const AnalysisSpan& span) {
    if (fact <= 0 || span.chunk_id <= 0 || span.end <= span.begin || span.end - span.begin > 2048 ||
        span.end > static_cast<size_t>(std::numeric_limits<long long>::max()))
        return make_error("invalid", "Invalid analysis excerpt.");
    auto source = db.prepare(R"sql(
SELECT s.id,e.revision_id,c.text FROM knowledge_support_sets s
JOIN knowledge_support_evidence se ON se.support_id=s.id
JOIN knowledge_evidence e ON e.id=se.evidence_id
JOIN knowledge_revisions v ON v.id=e.revision_id
JOIN chunks c ON c.id=e.chunk_id AND c.item_id=v.item_id
WHERE s.fact_id=?1 AND s.state IN ('current','unverified') AND v.current=1 AND c.id=?2
 AND ((e.precision='passage' AND CAST(e.excerpt AS BLOB)=CAST(c.text AS BLOB)) OR
      (e.precision='exact' AND e.byte_start<=?3 AND e.byte_end>=?4 AND
       CAST(e.excerpt AS BLOB)=substr(CAST(c.text AS BLOB),e.byte_start+1,e.byte_end-e.byte_start)))
ORDER BY s.id,e.id LIMIT 1)sql");
    source.bind(1, fact).bind(2, span.chunk_id).bind(3, static_cast<long long>(span.begin)).bind(4, static_cast<long long>(span.end));
    if (!source.step()) return make_error("invalid", "The excerpt is not part of a current support for this fact.");
    const auto support_id = source.col_int(0), revision_id = source.col_int(1);
    const auto content = source.col_text(2);
    const auto continuation = [](char c) { return (static_cast<unsigned char>(c) & 0xc0) == 0x80; };
    if (span.end > content.size() || (span.begin && continuation(content[span.begin])) ||
        (span.end < content.size() && continuation(content[span.end])))
        return make_error("invalid", "The excerpt is outside the passage or splits a UTF-8 character.");
    const std::string excerpt = content.substr(span.begin, span.end - span.begin);
    auto insert = db.prepare("INSERT OR IGNORE INTO knowledge_evidence(revision_id,chunk_id,excerpt,precision,byte_start,byte_end) "
                             "VALUES(?1,?2,?3,'exact',?4,?5)");
    insert.bind(1, revision_id).bind(2, span.chunk_id).bind(3, excerpt).bind(4, static_cast<long long>(span.begin))
        .bind(5, static_cast<long long>(span.end));
    if (auto inserted = insert.run(); !inserted) return inserted.error();
    auto find = db.prepare("SELECT id FROM knowledge_evidence WHERE revision_id=?1 AND chunk_id=?2 "
                           "AND precision='exact' AND byte_start=?3 AND byte_end=?4 AND CAST(excerpt AS BLOB)=CAST(?5 AS BLOB)");
    find.bind(1, revision_id).bind(2, span.chunk_id).bind(3, static_cast<long long>(span.begin)).bind(4, static_cast<long long>(span.end))
        .bind(5, excerpt);
    if (!find.step()) return make_error("stale", "The stored excerpt no longer matches the passage.");
    const long long evidence_id = find.col_int(0);
    auto attach = db.prepare("INSERT OR IGNORE INTO knowledge_support_evidence(support_id,evidence_id) VALUES(?1,?2)");
    attach.bind(1, support_id).bind(2, evidence_id);
    if (auto attached = attach.run(); !attached) return attached.error();
    return evidence_id;
}

// A manual decision always wins over an automatic one; an automatic suggestion only replaces another.
Status write_assignment(Database& db, long long fact, long long category, const std::string& decision, const std::string& reason,
                        long long evidence_id) {
    if (decision == "automatic" && !active_exact_evidence(db, fact, evidence_id))
        return make_error("invalid", "Category evidence does not support this fact.");
    auto s = db.prepare(R"sql(
INSERT INTO knowledge_category_assignments(fact_id,category_id,decision,reason,evidence_id) VALUES(?1,?2,?3,?4,?5)
 ON CONFLICT(fact_id,category_id) DO UPDATE SET decision=excluded.decision,reason=excluded.reason,evidence_id=excluded.evidence_id
 WHERE excluded.decision<>'automatic' OR knowledge_category_assignments.decision='automatic')sql");
    s.bind(1, fact).bind(2, category).bind(3, decision).bind(4, reason);
    if (decision == "automatic") s.bind(5, evidence_id);
    else s.bind_null(5);
    return s.run();
}

Result<long long> write_conflict(Database& db, long long left_fact, long long right_fact, long long left_evidence, long long right_evidence,
                                 const std::string& explanation, const std::string& kind = "conflict") {
    if (left_fact <= 0 || right_fact <= 0 || left_fact == right_fact || trim(explanation).empty() || explanation.size() > 512 ||
        (kind != "conflict" && kind != "uncertain_entity"))
        return make_error("invalid", "A possible inconsistency needs two facts and an explanation.");
    // Before the consistency schema a review has no kind and cannot be cleared.
    const bool kinds = db.user_version() >= kKgConsistencySchemaVersion;
    if (!kinds && kind != "conflict") return make_error("schema", "Possible inconsistency kinds are not available yet.");
    if (left_fact > right_fact) {
        std::swap(left_fact, right_fact);
        std::swap(left_evidence, right_evidence);
    }
    if (!active_exact_evidence(db, left_fact, left_evidence) || !active_exact_evidence(db, right_fact, right_evidence))
        return make_error("invalid", "Both facts need current, exact supporting excerpts.");
    // A review found again is no longer cleared; its kind follows the latest analysis while it is undecided.
    auto upsert = db.prepare(std::string(kinds ? R"sql(
INSERT INTO knowledge_conflicts(left_fact,right_fact,left_evidence,right_evidence,explanation,state,kind)
 VALUES(?1,?2,?3,?4,?5,'open',?6)
 ON CONFLICT(left_fact,right_fact) DO UPDATE SET
 cleared=0,
 kind=CASE WHEN left_evidence<>excluded.left_evidence OR right_evidence<>excluded.right_evidence
                OR state IN ('open','needs_recheck') THEN excluded.kind ELSE kind END,)sql"
                                               : R"sql(
INSERT INTO knowledge_conflicts(left_fact,right_fact,left_evidence,right_evidence,explanation,state)
 VALUES(?1,?2,?3,?4,?5,'open')
 ON CONFLICT(left_fact,right_fact) DO UPDATE SET)sql") + R"sql(
 state=CASE WHEN left_evidence<>excluded.left_evidence OR right_evidence<>excluded.right_evidence OR state='needs_recheck'
            THEN 'open' ELSE state END,
 decision=CASE WHEN left_evidence<>excluded.left_evidence OR right_evidence<>excluded.right_evidence OR state='needs_recheck'
               THEN '' ELSE decision END,
 decision_reason=CASE WHEN left_evidence<>excluded.left_evidence OR right_evidence<>excluded.right_evidence OR state='needs_recheck'
                      THEN '' ELSE decision_reason END,
 explanation=CASE WHEN left_evidence<>excluded.left_evidence OR right_evidence<>excluded.right_evidence
                        OR state IN ('open','needs_recheck') THEN excluded.explanation ELSE explanation END,
 left_evidence=excluded.left_evidence,right_evidence=excluded.right_evidence)sql");
    upsert.bind(1, left_fact).bind(2, right_fact).bind(3, left_evidence).bind(4, right_evidence).bind(5, explanation);
    if (kinds) upsert.bind(6, kind);
    if (auto result = upsert.run(); !result) return result.error();
    auto find = db.prepare("SELECT id FROM knowledge_conflicts WHERE left_fact=?1 AND right_fact=?2");
    find.bind(1, left_fact).bind(2, right_fact);
    if (!find.step()) return make_error("storage", "The review item could not be found.");
    return find.col_int(0);
}

}  // namespace

Status KgStore::set_category_assignment(long long fact, long long category, const std::string& decision,
                                        const std::string& reason, long long evidence_id,
                                        std::optional<long long> expected_generation) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge categories are not available yet.");
    if ((decision != "automatic" && decision != "manual" && decision != "rejected") || reason.size() > 512)
        return make_error("invalid", "Invalid category decision or explanation.");
    return db_->transaction([&]() -> Status {
        if (decision == "automatic") {
            if (evidence_id <= 0 || trim(reason).empty() || !expected_generation)
                return make_error("invalid", "Automatic categories need an explanation, evidence and a knowledge snapshot.");
            const auto current_generation = generation_of(*db_);
            if (!current_generation || *current_generation != *expected_generation)
                return make_error("stale", "Knowledge changed while the category was being analyzed.");
        } else if (evidence_id != 0) return make_error("invalid", "Manual category decisions do not take model evidence.");
        return write_assignment(*db_, fact, category, decision, reason, evidence_id);
    });
}

Status KgStore::apply_analysis(const AnalysisProposals& proposals, long long expected_generation,
                               const std::function<bool()>& still_permitted, const std::vector<long long>& categorized,
                               const std::vector<long long>& consistency_checked, const std::vector<long long>& cleared) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge analysis is not available yet.");
    for (const auto& c : proposals.categories)
        if (c.category_id <= 0 || trim(c.reason).empty() || c.reason.size() > 512)
            return make_error("invalid", "A category suggestion needs a category and an explanation.");
    return db_->transaction([&]() -> Status {
        // The graph generation changes with every revision, exclusion, Source setting and fact change, so an equal
        // generation means the analysis read exactly what is stored now. Policy kept outside the store is asked here,
        // under the same lock, so nothing can change between this check and the commit.
        const auto generation = generation_of(*db_);
        if (!generation || *generation != expected_generation) return make_error("stale", "Knowledge changed during analysis.");
        if (still_permitted && !still_permitted()) return make_error("stale", "Analysis is no longer permitted for this knowledge.");
        for (const auto& c : proposals.categories) {
            const auto evidence = bind_span(*db_, c.fact_id, c.evidence);
            if (!evidence) return evidence.error();
            if (Status s = write_assignment(*db_, c.fact_id, c.category_id, "automatic", c.reason, *evidence); !s) return s;
        }
        for (const auto& c : proposals.conflicts) {
            const auto left = bind_span(*db_, c.left_id, c.left);
            if (!left) return left.error();
            const auto right = bind_span(*db_, c.right_id, c.right);
            if (!right) return right.error();
            if (const auto recorded = write_conflict(*db_, c.left_id, c.right_id, *left, *right, c.reason, c.kind); !recorded)
                return recorded.error();
        }
        if ((!consistency_checked.empty() || !cleared.empty()) && db_->user_version() < kKgConsistencySchemaVersion)
            return make_error("schema", "Consistency progress is not available yet.");
        // A review proposed again above is open; one that still needs a recheck was compared and not found again.
        for (long long id : cleared) {
            auto clear = db_->prepare("UPDATE knowledge_conflicts SET cleared=1 WHERE id=?1 AND state='needs_recheck'");
            clear.bind(1, id);
            if (auto done = clear.run(); !done) return done;
        }
        if (!consistency_checked.empty()) {
            auto check = db_->prepare("INSERT OR REPLACE INTO knowledge_consistency_checks(fact_id,checked_at) "
                                      "SELECT id,CAST(strftime('%s','now') AS INTEGER)*1000 FROM knowledge_facts WHERE id=?1");
            for (long long fact : consistency_checked) {
                check.reset();
                check.bind(1, fact);
                if (auto recorded = check.run(); !recorded) return recorded;
            }
        }
        // Recorded last: binding an excerpt above adds evidence, which clears a fact's earlier check.
        if (!categorized.empty()) {
            if (db_->user_version() < kKgCategoryChecksSchemaVersion) return make_error("schema", "Categorization progress is not available yet.");
            auto check = db_->prepare("INSERT OR REPLACE INTO knowledge_category_checks(fact_id,checked_at) "
                                      "SELECT id,CAST(strftime('%s','now') AS INTEGER)*1000 FROM knowledge_facts WHERE id=?1");
            for (long long fact : categorized) {
                check.reset();
                check.bind(1, fact);
                if (auto recorded = check.run(); !recorded) return recorded;
            }
        }
        return {};
    });
}


Status KgStore::reset_category_assignment(long long fact, long long category) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge categories are not available yet.");
    auto s = db_->prepare("DELETE FROM knowledge_category_assignments WHERE fact_id=?1 AND category_id=?2");
    s.bind(1, fact).bind(2, category);
    return s.run();
}

std::vector<CategoryRow> KgStore::categories() {
    const std::lock_guard lock(db_->mutex());
    std::vector<CategoryRow> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion) return result;
    auto s = db_->prepare("SELECT id,name FROM knowledge_categories ORDER BY normalized_name,id");
    while (s.step()) result.push_back({s.col_int(0), s.col_text(1)});
    return result;
}

std::vector<CategoryAssignment> KgStore::category_assignments(long long fact) {
    const std::lock_guard lock(db_->mutex());
    std::vector<CategoryAssignment> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion) return result;
    auto s = db_->prepare("SELECT a.fact_id,a.category_id,ifnull(a.evidence_id,0),a.decision,a.reason,c.name,"
                          "ifnull(e.chunk_id,0),ifnull(e.excerpt,'') FROM knowledge_category_assignments a "
                          "JOIN knowledge_categories c ON c.id=a.category_id "
                          "LEFT JOIN knowledge_evidence e ON e.id=a.evidence_id "
                          "WHERE a.fact_id=?1 ORDER BY c.normalized_name,a.category_id");
    s.bind(1, fact);
    while (s.step())
        result.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_text(3), s.col_text(4), s.col_text(5), s.col_int(6),
                          s.col_text(7)});
    return result;
}

std::vector<long long> KgStore::uncategorized_facts(int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<long long> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion || limit <= 0) return result;
    auto s = db_->prepare("SELECT f.id FROM knowledge_facts f WHERE EXISTS (SELECT 1 FROM knowledge_support_sets s "
                          "WHERE s.fact_id=f.id AND s.state IN ('current','unverified')) "
                          "AND NOT EXISTS (SELECT 1 FROM knowledge_category_assignments a "
                          "WHERE a.fact_id=f.id AND a.decision IN ('automatic','manual')) ORDER BY f.id LIMIT ?1");
    s.bind(1, std::min(limit, 1000));
    while (s.step()) result.push_back(s.col_int(0));
    return result;
}

std::vector<long long> KgStore::fact_page(std::optional<long long> category, long long before, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<long long> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion || limit <= 0) return result;
    std::string sql = "SELECT f.id FROM knowledge_facts f WHERE (?1<=0 OR f.id<?1) AND EXISTS (SELECT 1 FROM knowledge_support_sets s "
                      "WHERE s.fact_id=f.id AND s.state IN ('current','unverified'))";
    if (category && *category > 0)
        sql += " AND EXISTS (SELECT 1 FROM knowledge_category_assignments a WHERE a.fact_id=f.id AND a.category_id=?3 "
               "AND a.decision IN ('automatic','manual'))";
    else if (category)
        sql += " AND NOT EXISTS (SELECT 1 FROM knowledge_category_assignments a WHERE a.fact_id=f.id AND a.decision IN ('automatic','manual'))";
    sql += " ORDER BY f.id DESC LIMIT ?2";
    auto s = db_->prepare(sql);
    s.bind(1, before).bind(2, std::min(limit, 1000));
    if (category && *category > 0) s.bind(3, *category);
    while (s.step()) result.push_back(s.col_int(0));
    return result;
}

std::vector<CategoryAssignment> KgStore::category_suggestions(long long before_fact, long long after_category, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<CategoryAssignment> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion || limit <= 0) return result;
    auto s = db_->prepare("SELECT a.fact_id,a.category_id,ifnull(a.evidence_id,0),a.decision,a.reason,c.name,"
                          "ifnull(e.chunk_id,0),ifnull(e.excerpt,'') FROM knowledge_category_assignments a "
                          "JOIN knowledge_categories c ON c.id=a.category_id "
                          "LEFT JOIN knowledge_evidence e ON e.id=a.evidence_id "
                          "WHERE a.decision='automatic' AND (?1<=0 OR a.fact_id<?1 OR (a.fact_id=?1 AND a.category_id>?2)) "
                          "ORDER BY a.fact_id DESC,a.category_id LIMIT ?3");
    s.bind(1, before_fact).bind(2, after_category).bind(3, std::min(limit, 1000));
    while (s.step())
        result.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_text(3), s.col_text(4), s.col_text(5), s.col_int(6),
                          s.col_text(7)});
    return result;
}

std::optional<long long> KgStore::knowledge_generation() {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return std::nullopt;
    auto s = db_->prepare("SELECT value FROM knowledge_generation WHERE id=1");
    if (!s.step()) return std::nullopt;
    return s.col_int(0);
}

Result<BoundAnalysisEvidence> KgStore::bind_analysis_evidence(long long fact, const AnalysisSpan& span,
                                                               long long expected_generation) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge evidence is not available yet.");
    BoundAnalysisEvidence result;
    auto status = db_->transaction([&]() -> Status {
        const auto generation = generation_of(*db_);
        if (!generation || *generation != expected_generation)
            return make_error("stale", "Knowledge changed while the excerpt was being analyzed.");
        const auto evidence = bind_span(*db_, fact, span);
        if (!evidence) return evidence.error();
        result.evidence_id = *evidence;
        const auto current = generation_of(*db_);
        if (!current) return make_error("sqlite", "Could not read the knowledge snapshot.");
        result.generation = *current;
        return {};
    });
    if (!status) return status.error();
    return result;
}

Result<long long> KgStore::record_conflict(long long left_fact, long long right_fact, long long left_evidence,
                                           long long right_evidence, const std::string& explanation, long long expected_generation) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge review is not available yet.");
    long long id = 0;
    auto status = db_->transaction([&]() -> Status {
        const auto generation = generation_of(*db_);
        if (!generation || *generation != expected_generation) return make_error("stale", "Knowledge changed during consistency analysis.");
        const auto recorded = write_conflict(*db_, left_fact, right_fact, left_evidence, right_evidence, explanation);
        if (!recorded) return recorded.error();
        id = *recorded;
        return {};
    });
    if (!status) return status.error();
    return id;
}

Status KgStore::decide_conflict(long long id, const std::string& decision, const std::string& reason,
                                long long now, long long expected_generation) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge review is not available yet.");
    if ((decision != "keep_both" && decision != "prefer_left" && decision != "prefer_right" && decision != "defer") ||
        reason.size() > 512 || now <= 0 || ((decision == "prefer_left" || decision == "prefer_right") && trim(reason).empty()))
        return make_error("invalid", "Invalid review decision or reason.");
    return db_->transaction([&]() -> Status {
        const auto generation = knowledge_generation();
        if (!generation || *generation != expected_generation) return make_error("stale", "Knowledge changed while this decision was open.");
        auto current = db_->prepare("SELECT left_fact,right_fact,left_evidence,right_evidence,state FROM knowledge_conflicts WHERE id=?1");
        current.bind(1, id);
        if (!current.step()) return make_error("not_found", "No such review item.");
        const auto left = current.col_int(0), right = current.col_int(1);
        const auto left_evidence = current.col_int(2), right_evidence = current.col_int(3);
        if (current.col_text(4) == "needs_recheck" || !active_exact_evidence(*db_, left, left_evidence) ||
            !active_exact_evidence(*db_, right, right_evidence))
            return make_error("stale", "The reviewed evidence needs to be checked again.");
        auto history = db_->prepare("INSERT INTO knowledge_conflict_history(conflict_id,left_evidence,right_evidence,decision,reason,decided_at) "
                                    "VALUES(?1,?2,?3,?4,?5,?6)");
        history.bind(1, id).bind(2, left_evidence).bind(3, right_evidence).bind(4, decision).bind(5, reason).bind(6, now);
        if (auto result = history.run(); !result) return result;
        auto update = db_->prepare("UPDATE knowledge_conflicts SET state=?2,decision=?3,decision_reason=?4 WHERE id=?1");
        update.bind(1, id).bind(2, decision == "defer" ? "deferred" : "resolved").bind(3, decision).bind(4, reason);
        return update.run();
    });
}

Status KgStore::reopen_conflict(long long id, long long expected_generation) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge review is not available yet.");
    return db_->transaction([&]() -> Status {
        const auto generation = knowledge_generation();
        if (!generation || *generation != expected_generation) return make_error("stale", "Knowledge changed while this decision was open.");
        auto s = db_->prepare("UPDATE knowledge_conflicts SET state='open',decision='',decision_reason='' "
                              "WHERE id=?1 AND state IN ('resolved','deferred')");
        s.bind(1, id);
        if (auto result = s.run(); !result) return result;
        return db_->changes() ? Status{} : Status(make_error("stale", "The review item cannot be reopened."));
    });
}

std::vector<ConflictRow> KgStore::conflicts(int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<ConflictRow> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion || limit <= 0) return result;
    const bool kinds = db_->user_version() >= kKgConsistencySchemaVersion;
    auto s = db_->prepare(std::string("SELECT id,left_fact,right_fact,left_evidence,right_evidence,explanation,state,decision,decision_reason,") +
                          (kinds ? "kind,cleared" : "'conflict',0") +
                          " FROM knowledge_conflicts ORDER BY CASE state WHEN 'open' THEN 0 WHEN 'needs_recheck' THEN 1 "
                          "WHEN 'deferred' THEN 2 ELSE 3 END,id DESC LIMIT ?1");
    s.bind(1, std::min(limit, 1000));
    while (s.step())
        result.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4), s.col_text(5), s.col_text(6),
                          s.col_text(7), s.col_text(8), s.col_text(9), s.col_int(10) != 0});
    return result;
}

int KgStore::conflict_rank(const std::string& state) {
    return state == "open" ? 0 : state == "needs_recheck" ? 1 : state == "deferred" ? 2 : 3;
}

std::vector<ConflictRow> KgStore::conflicts_page(int after_rank, long long after_id, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<ConflictRow> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion || limit <= 0) return result;
    const bool kinds = db_->user_version() >= kKgConsistencySchemaVersion;
    auto s = db_->prepare(std::string("SELECT id,left_fact,right_fact,left_evidence,right_evidence,explanation,state,decision,decision_reason,") +
                          (kinds ? "kind,cleared" : "'conflict',0") +
                          ",rank FROM (SELECT *,CASE state WHEN 'open' THEN 0 WHEN 'needs_recheck' THEN 1 WHEN 'deferred' THEN 2 ELSE 3 END "
                          "AS rank FROM knowledge_conflicts) WHERE ?1<0 OR rank>?1 OR (rank=?1 AND id<?2) ORDER BY rank,id DESC LIMIT ?3");
    s.bind(1, after_rank).bind(2, after_id).bind(3, std::min(limit, 1000));
    while (s.step())
        result.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4), s.col_text(5), s.col_text(6),
                          s.col_text(7), s.col_text(8), s.col_text(9), s.col_int(10) != 0});
    return result;
}

std::optional<ConflictRow> KgStore::conflict(long long id) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return std::nullopt;
    const bool kinds = db_->user_version() >= kKgConsistencySchemaVersion;
    auto s = db_->prepare(std::string("SELECT id,left_fact,right_fact,left_evidence,right_evidence,explanation,state,decision,decision_reason,") +
                          (kinds ? "kind,cleared" : "'conflict',0") + " FROM knowledge_conflicts WHERE id=?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return ConflictRow{s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4), s.col_text(5), s.col_text(6),
                       s.col_text(7), s.col_text(8), s.col_text(9), s.col_int(10) != 0};
}

std::vector<ConflictDecision> KgStore::conflict_history(long long id) {
    const std::lock_guard lock(db_->mutex());
    std::vector<ConflictDecision> result;
    if (db_->user_version() < kKgEvidenceSchemaVersion) return result;
    auto s = db_->prepare("SELECT left_evidence,right_evidence,decided_at,decision,reason "
                          "FROM knowledge_conflict_history WHERE conflict_id=?1 ORDER BY id");
    s.bind(1, id);
    while (s.step()) result.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_text(3), s.col_text(4)});
    return result;
}

// ---- Knowledge state (Retrieval Respects Knowledge State; design D7, knowledge state filter) ---------------------------

namespace {

// The fact was the other side of a review the user resolved by preferring one side. A review whose evidence changed is
// set back to needs_recheck by the schema and clears its decision, so the rejection lapses with it.
std::string rejected_sql(const std::string& fact) {
    return "EXISTS(SELECT 1 FROM knowledge_conflicts k WHERE k.state='resolved' AND ((k.decision='prefer_left' AND k.right_fact=" +
           fact + ") OR (k.decision='prefer_right' AND k.left_fact=" + fact + ")))";
}

bool fact_rejected(Database& db, long long fact) {
    auto s = db.prepare("SELECT " + rejected_sql("?1"));
    s.bind(1, fact);
    return s.step() && s.col_int(0);
}

bool fact_supported(Database& db, long long fact) {
    auto s = db.prepare("SELECT 1 FROM knowledge_support_sets WHERE fact_id=?1 AND state IN ('current','unverified') LIMIT 1");
    s.bind(1, fact);
    return s.step();
}

std::optional<KnowledgeFactRow> read_fact(Database& db, long long fact) {
    auto s = db.prepare("SELECT f.id,f.subject,f.object,s.name,f.predicate,o.name,f.unit,f.scope,f.valid_from,f.valid_to "
                        "FROM knowledge_facts f JOIN entities s ON s.id=f.subject JOIN entities o ON o.id=f.object WHERE f.id=?1");
    s.bind(1, fact);
    if (!s.step()) return std::nullopt;
    return KnowledgeFactRow{s.col_int(0), s.col_int(1), s.col_int(2), s.col_text(3), s.col_text(4), s.col_text(5),
                            s.col_text(6), s.col_text(7), s.col_text(8), s.col_text(9)};
}

std::optional<FactCitation> read_citation(Database& db, long long evidence) {
    auto s = db.prepare("SELECT e.id,v.item_id,ifnull(e.chunk_id,0),v.sequence,v.current,e.excerpt,e.precision "
                        "FROM knowledge_evidence e JOIN knowledge_revisions v ON v.id=e.revision_id WHERE e.id=?1");
    s.bind(1, evidence);
    if (!s.step()) return std::nullopt;
    return FactCitation{s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4) != 0, s.col_text(5), s.col_text(6), {}};
}

}  // namespace

std::optional<long long> KgStore::fact_of_relation(const RelationRow& r) {
    const std::lock_guard lock(db_->mutex());
    if (!r.chunk_id || db_->user_version() < kKgEvidenceSchemaVersion) return std::nullopt;
    auto s = db_->prepare("SELECT id FROM knowledge_facts WHERE subject=?1 AND predicate=?2 AND object=?3 AND unit='' AND scope='' "
                          "AND valid_from=?4 AND valid_to=?5");
    s.bind(1, r.subject).bind(2, r.predicate).bind(3, r.object).bind(4, r.valid_from).bind(5, r.valid_to);
    if (!s.step()) return std::nullopt;
    return s.col_int(0);
}

std::optional<FactCitation> KgStore::evidence_citation(long long evidence_id) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return std::nullopt;
    return read_citation(*db_, evidence_id);
}

std::optional<FactState> KgStore::fact_state(long long fact_id, bool history) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return std::nullopt;
    auto fact = read_fact(*db_, fact_id);
    if (!fact) return std::nullopt;
    FactState out;
    out.fact = std::move(*fact);
    bool current = false, unsupported = false;
    {
        auto s = db_->prepare("SELECT DISTINCT state FROM knowledge_support_sets WHERE fact_id=?1");
        s.bind(1, fact_id);
        while (s.step()) {
            const auto state = s.col_text(0);
            current = current || state == "current" || state == "unverified";
            unsupported = unsupported || state == "unsupported";
        }
    }
    {
        // Several supports can share one excerpt; each is cited once, current revisions first.
        auto s = db_->prepare(std::string(
            "SELECT e.id,v.item_id,ifnull(e.chunk_id,0),v.sequence,v.current,e.excerpt,e.precision,"
            "min(CASE s.state WHEN 'current' THEN 0 WHEN 'unverified' THEN 1 WHEN 'superseded' THEN 2 ELSE 3 END) "
            "FROM knowledge_support_sets s JOIN knowledge_support_evidence se ON se.support_id=s.id "
            "JOIN knowledge_evidence e ON e.id=se.evidence_id JOIN knowledge_revisions v ON v.id=e.revision_id "
            "WHERE s.fact_id=?1") + (history ? "" : " AND s.state IN ('current','unverified') AND v.current=1") +
            " GROUP BY e.id ORDER BY v.current DESC,v.sequence DESC,e.id LIMIT 50");
        s.bind(1, fact_id);
        static const char* const kSupport[] = {"current", "unverified", "superseded", "unsupported"};
        while (s.step())
            out.citations.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4) != 0, s.col_text(5),
                                     s.col_text(6), kSupport[std::clamp<long long>(s.col_int(7), 0, 3)]});
    }
    {
        auto s = db_->prepare("SELECT c.id,c.name FROM knowledge_category_assignments a JOIN knowledge_categories c ON c.id=a.category_id "
                              "WHERE a.fact_id=?1 AND a.decision IN ('automatic','manual') ORDER BY c.normalized_name,c.id");
        s.bind(1, fact_id);
        while (s.step()) out.categories.push_back({s.col_int(0), s.col_text(1)});
    }
    if (fact_rejected(*db_, fact_id)) out.state = "rejected";
    else if (!current) out.state = unsupported ? "unsupported" : "superseded";
    else {
        // An unresolved review counts only while its other side is current knowledge too.
        // A recheck that compared both facts again without finding the disagreement no longer disputes them.
        const bool kinds = db_->user_version() >= kKgConsistencySchemaVersion;
        auto s = db_->prepare(std::string("SELECT id,CASE WHEN left_fact=?1 THEN right_fact ELSE left_fact END,state,explanation,"
                              "CASE WHEN left_fact=?1 THEN left_evidence ELSE right_evidence END,"
                              "CASE WHEN left_fact=?1 THEN right_evidence ELSE left_evidence END,") +
                              (kinds ? "kind" : "'conflict'") +
                              " FROM knowledge_conflicts WHERE (left_fact=?1 OR right_fact=?1) "
                              "AND state IN ('open','deferred','needs_recheck')" + (kinds ? " AND cleared=0" : "") + " ORDER BY id");
        s.bind(1, fact_id);
        struct Row { long long id, other, own_evidence, other_evidence; std::string state, explanation, kind; };
        std::vector<Row> rows;
        while (s.step())
            rows.push_back({s.col_int(0), s.col_int(1), s.col_int(4), s.col_int(5), s.col_text(2), s.col_text(3), s.col_text(6)});
        for (const auto& r : rows) {
            if (!fact_supported(*db_, r.other) || fact_rejected(*db_, r.other)) continue;
            auto other = read_fact(*db_, r.other);
            if (!other) continue;
            out.disputes.push_back({r.id, r.state, r.explanation, std::move(*other), read_citation(*db_, r.own_evidence),
                                    read_citation(*db_, r.other_evidence), r.kind});
        }
        out.state = out.disputes.empty() ? "current" : "disputed";
    }
    return out;
}

std::vector<long long> KgStore::facts_about(long long entity_id, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<long long> out;
    if (db_->user_version() < kKgEvidenceSchemaVersion || limit <= 0) return out;
    auto s = db_->prepare("SELECT id FROM knowledge_facts WHERE subject=?1 UNION SELECT id FROM knowledge_facts WHERE object=?1 "
                          "ORDER BY id DESC LIMIT ?2");
    s.bind(1, entity_id).bind(2, std::min(limit, 1000));
    while (s.step()) out.push_back(s.col_int(0));
    return out;
}

namespace {

// Facts not yet analyzed by the analysis whose progress checks records, most recently changed knowledge first
// (Bounded Knowledge Analysis): facts whose newest current excerpt is newest, then facts by id, which also covers
// facts supported only by other facts. At most wanted (and 2,000), without those in skip.
std::vector<long long> changed_first(Database& db, const std::string& checks, size_t wanted, const std::set<long long>& skip) {
    wanted = std::min<size_t>(wanted, 2000);
    std::vector<long long> ids;
    std::set<long long> seen;
    const auto take = [&](Statement& s) {
        while (ids.size() < wanted && s.step()) {
            const long long id = s.col_int(0);
            if (!skip.count(id) && seen.insert(id).second) ids.push_back(id);
        }
    };
    // Scans excerpts newest first and stops when enough facts are found; rows of facts already checked are skipped.
    auto recent = db.prepare("SELECT s.fact_id FROM knowledge_evidence e JOIN knowledge_support_evidence se ON se.evidence_id=e.id "
                             "JOIN knowledge_support_sets s ON s.id=se.support_id WHERE s.state IN ('current','unverified') "
                             "AND NOT EXISTS(SELECT 1 FROM " + checks + " k WHERE k.fact_id=s.fact_id) ORDER BY e.id DESC");
    take(recent);
    if (ids.size() < wanted) {
        auto rest = db.prepare("SELECT f.id FROM knowledge_facts f WHERE NOT EXISTS(SELECT 1 FROM " + checks + " k WHERE k.fact_id=f.id) "
                               "AND EXISTS(SELECT 1 FROM knowledge_support_sets s WHERE s.fact_id=f.id AND s.state IN ('current','unverified')) "
                               "ORDER BY f.id LIMIT ?1");
        rest.bind(1, static_cast<long long>(wanted + skip.size() + seen.size()));
        take(rest);
    }
    return ids;
}

}  // namespace

CategoryWork KgStore::category_work(int limit, const std::set<long long>& skip) {
    const std::lock_guard lock(db_->mutex());
    CategoryWork out;
    if (db_->user_version() < kKgCategoryChecksSchemaVersion || limit <= 0) return out;
    const auto generation = generation_of(*db_);
    if (!generation) return out;
    out.generation = *generation;
    {
        auto s = db_->prepare("SELECT id,name FROM knowledge_categories ORDER BY normalized_name,id");
        while (s.step()) out.categories.push_back({s.col_int(0), s.col_text(1)});
    }
    if (out.categories.empty()) return out;
    std::vector<long long> ids = changed_first(*db_, "knowledge_category_checks", static_cast<size_t>(limit) + skip.size(), skip);
    // Only whole passages a current support cites: the model quotes from them, and the quote must bind to that support.
    auto passages = db_->prepare(
        "SELECT DISTINCT c.id,c.item_id,c.heading,c.text,ifnull(c.at,0) FROM knowledge_support_sets s "
        "JOIN knowledge_support_evidence se ON se.support_id=s.id JOIN knowledge_evidence e ON e.id=se.evidence_id "
        "JOIN knowledge_revisions v ON v.id=e.revision_id JOIN chunks c ON c.id=e.chunk_id AND c.item_id=v.item_id "
        "WHERE s.fact_id=?1 AND s.state IN ('current','unverified') AND v.current=1 AND e.precision='passage' "
        "AND CAST(e.excerpt AS BLOB)=CAST(c.text AS BLOB) ORDER BY c.id LIMIT 3");
    for (long long id : ids) {
        if (static_cast<int>(out.facts.size()) >= limit) break;
        if (fact_rejected(*db_, id)) continue;
        auto fact = read_fact(*db_, id);
        if (!fact) continue;
        CategoryCandidate candidate{std::move(*fact), {}};
        passages.reset();
        passages.bind(1, id);
        while (passages.step())
            candidate.passages.push_back({passages.col_int(0), passages.col_int(1), passages.col_text(2), passages.col_text(3),
                                          passages.col_int(4)});
        out.facts.push_back(std::move(candidate));
    }
    return out;
}

long long KgStore::category_pending() {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgCategoryChecksSchemaVersion) return 0;
    auto s = db_->prepare(
        "SELECT count(*) FROM knowledge_facts f WHERE NOT EXISTS(SELECT 1 FROM knowledge_category_checks k WHERE k.fact_id=f.id) "
        "AND EXISTS(SELECT 1 FROM knowledge_support_sets s WHERE s.fact_id=f.id AND s.state IN ('current','unverified')) "
        "AND EXISTS(SELECT 1 FROM knowledge_categories)");
    return s.step() ? s.col_int(0) : 0;
}

ConsistencyWork KgStore::consistency_work(int limit, int related, int passage_limit, const std::set<long long>& skip) {
    const std::lock_guard lock(db_->mutex());
    ConsistencyWork out;
    if (db_->user_version() < kKgConsistencySchemaVersion || limit <= 0 || related < 0 || passage_limit <= 0) return out;
    const auto generation = generation_of(*db_);
    if (!generation) return out;
    out.generation = *generation;
    std::vector<long long> ids = changed_first(*db_, "knowledge_consistency_checks", static_cast<size_t>(limit) * 4 + skip.size(), skip);
    auto passages = db_->prepare(
        "SELECT DISTINCT c.id,c.item_id,c.heading,c.text,ifnull(c.at,0) FROM knowledge_support_sets s "
        "JOIN knowledge_support_evidence se ON se.support_id=s.id JOIN knowledge_evidence e ON e.id=se.evidence_id "
        "JOIN knowledge_revisions v ON v.id=e.revision_id JOIN chunks c ON c.id=e.chunk_id AND c.item_id=v.item_id "
        "WHERE s.fact_id=?1 AND s.state IN ('current','unverified') AND v.current=1 AND e.precision='passage' "
        "AND CAST(e.excerpt AS BLOB)=CAST(c.text AS BLOB) ORDER BY c.id LIMIT ?2");
    const auto load = [&](long long id) -> bool {
        if (out.facts.count(id)) return true;
        if (!fact_supported(*db_, id) || fact_rejected(*db_, id)) return false;
        auto fact = read_fact(*db_, id);
        if (!fact) return false;
        CategoryCandidate f{std::move(*fact), {}};
        passages.reset();
        passages.bind(1, id).bind(2, static_cast<long long>(passage_limit));
        while (passages.step())
            f.passages.push_back({passages.col_int(0), passages.col_int(1), passages.col_text(2), passages.col_text(3),
                                  passages.col_int(4)});
        out.facts.emplace(id, std::move(f));
        return true;
    };
    // Related facts in order of relevance; each query is bounded and served by an index on the subject or object.
    const long long fetch = std::max(1, related) * 3LL;
    auto same_claim = db_->prepare("SELECT id FROM knowledge_facts WHERE subject=?1 AND predicate=?2 AND id<>?3 ORDER BY id DESC LIMIT ?4");
    auto same_value = db_->prepare("SELECT id FROM knowledge_facts WHERE object=?1 AND predicate=?2 AND id<>?3 ORDER BY id DESC LIMIT ?4");
    auto shared = db_->prepare("SELECT id FROM (SELECT id FROM knowledge_facts WHERE subject IN (?1,?2) AND id<>?3 "
                               "UNION SELECT id FROM knowledge_facts WHERE object IN (?1,?2) AND id<>?3) ORDER BY id DESC LIMIT ?4");
    auto rechecks = db_->prepare("SELECT id,left_fact,right_fact FROM knowledge_conflicts "
                                 "WHERE (left_fact=?1 OR right_fact=?1) AND state='needs_recheck' AND cleared=0 ORDER BY id LIMIT 8");
    for (long long id : ids) {
        if (static_cast<int>(out.targets.size()) >= limit) break;
        if (!load(id)) continue;
        const KnowledgeFactRow fact = out.facts.at(id).fact;
        ConsistencyTarget target{id, {}};
        const auto add = [&](long long other) {
            if (other == id || std::find(target.related.begin(), target.related.end(), other) != target.related.end()) return;
            if (load(other)) target.related.push_back(other);
        };
        // The other side of a review awaiting a recheck is always compared, beyond the bound.
        rechecks.reset();
        rechecks.bind(1, id);
        while (rechecks.step()) {
            const long long other = rechecks.col_int(1) == id ? rechecks.col_int(2) : rechecks.col_int(1);
            add(other);
            if (std::find(target.related.begin(), target.related.end(), other) != target.related.end())
                out.rechecks.push_back({rechecks.col_int(0), rechecks.col_int(1), rechecks.col_int(2)});
        }
        const size_t bound = target.related.size() + static_cast<size_t>(related);
        for (auto* q : {&same_claim, &same_value, &shared}) {
            if (target.related.size() >= bound) break;
            q->reset();
            if (q == &shared) q->bind(1, fact.subject).bind(2, fact.object);
            else q->bind(1, q == &same_claim ? fact.subject : fact.object).bind(2, fact.predicate);
            q->bind(3, id).bind(4, fetch);
            std::vector<long long> found;
            while (q->step()) found.push_back(q->col_int(0));
            for (long long other : found) {
                if (target.related.size() >= bound) break;
                add(other);
            }
        }
        out.targets.push_back(std::move(target));
    }
    return out;
}

long long KgStore::consistency_pending() {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgConsistencySchemaVersion) return 0;
    auto s = db_->prepare(
        "SELECT count(*) FROM knowledge_facts f WHERE NOT EXISTS(SELECT 1 FROM knowledge_consistency_checks k WHERE k.fact_id=f.id) "
        "AND EXISTS(SELECT 1 FROM knowledge_support_sets s WHERE s.fact_id=f.id AND s.state IN ('current','unverified'))");
    return s.step() ? s.col_int(0) : 0;
}

std::optional<long long> KgStore::find_category(const std::string& name) {
    const auto normalized = normalize_name(trim(name));
    if (normalized.empty()) return std::nullopt;
    if (normalized == normalize_name(kUncategorized)) return 0;
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return std::nullopt;
    auto s = db_->prepare("SELECT id FROM knowledge_categories WHERE normalized_name=?1");
    s.bind(1, normalized);
    if (!s.step()) return std::nullopt;
    return s.col_int(0);
}

std::unordered_set<long long> KgStore::chunks_in_category(const std::vector<long long>& chunk_ids, long long category) {
    const std::lock_guard lock(db_->mutex());
    std::unordered_set<long long> out;
    if (db_->user_version() < kKgEvidenceSchemaVersion) {
        if (category == 0) out.insert(chunk_ids.begin(), chunk_ids.end());
        return out;
    }
    // The current, not rejected facts the passage's current excerpts support. A passage belongs to a category when one
    // of them is assigned to it, and to Uncategorized when one of them has no accepted category or it supports none.
    const std::string supported =
        "SELECT 1 FROM knowledge_evidence e JOIN knowledge_revisions v ON v.id=e.revision_id AND v.current=1 "
        "JOIN knowledge_support_evidence se ON se.evidence_id=e.id "
        "JOIN knowledge_support_sets s ON s.id=se.support_id AND s.state IN ('current','unverified') "
        "WHERE e.chunk_id=?1 AND NOT " + rejected_sql("s.fact_id");
    auto in_category = db_->prepare(supported + " AND EXISTS(SELECT 1 FROM knowledge_category_assignments a WHERE a.fact_id=s.fact_id "
                                                "AND a.decision IN ('automatic','manual') AND a.category_id=?2) LIMIT 1");
    auto any = db_->prepare(supported + " LIMIT 1");
    auto uncategorized = db_->prepare(supported + " AND NOT EXISTS(SELECT 1 FROM knowledge_category_assignments a "
                                                  "WHERE a.fact_id=s.fact_id AND a.decision IN ('automatic','manual')) LIMIT 1");
    for (const long long chunk : chunk_ids) {
        bool keep = false;
        if (category != 0) {
            in_category.reset();
            in_category.bind(1, chunk).bind(2, category);
            keep = in_category.step();
        } else {
            uncategorized.reset();
            uncategorized.bind(1, chunk);
            any.reset();
            any.bind(1, chunk);
            keep = uncategorized.step() || !any.step();
        }
        if (keep) out.insert(chunk);
    }
    return out;
}

Result<KnowledgeImpactPreview> KgStore::preview_evidence_removal(const std::unordered_set<long long>& removed) {
    return preview_evidence_snapshot(removed, std::nullopt);
}

Result<KnowledgeImpactPreview> KgStore::preview_item_removal(long long item_id) {
    if (item_id <= 0) return make_error("invalid", "Choose an indexed item.");
    return preview_evidence_snapshot({}, item_id);
}

Result<KnowledgeImpactPreview> KgStore::preview_evidence_snapshot(std::unordered_set<long long> removed,
                                                                  std::optional<long long> item_id) {
    std::vector<ImpactFact> facts;
    std::unordered_set<long long> evidence;
    long long generation = 0;
    {
        const std::lock_guard lock(db_->mutex());
        if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge impact previews are not available yet.");
        if (auto status = db_->exec("BEGIN"); !status) return status.error();
        const auto read = [&]() -> Status {
            const auto g = knowledge_generation();
            if (!g) return make_error("storage", "The knowledge generation is unavailable.");
            generation = *g;
            if (item_id) {
                auto item = db_->prepare("SELECT 1 FROM items WHERE id=?1");
                item.bind(1, *item_id);
                if (!item.step()) {
                    if (!item.status()) return item.status();
                    return make_error("not_found", "No such indexed item.");
                }
                auto item_evidence = db_->prepare("SELECT e.id FROM knowledge_evidence e "
                                                  "JOIN knowledge_revisions v ON v.id=e.revision_id WHERE v.item_id=?1");
                item_evidence.bind(1, *item_id);
                while (item_evidence.step()) {
                    if (removed.size() >= 2'000'000) return make_error("too_large", "The knowledge graph exceeds the preview limit.");
                    removed.insert(item_evidence.col_int(0));
                }
                if (!item_evidence.status()) return item_evidence.status();
            }
            std::unordered_map<long long, size_t> fact_index;
            auto fs = db_->prepare("SELECT id FROM knowledge_facts ORDER BY id");
            while (fs.step()) {
                if (facts.size() >= 100'000) return make_error("too_large", "The knowledge graph exceeds the preview limit.");
                const auto id = fs.col_int(0);
                fact_index.emplace(id, facts.size());
                facts.push_back({id, {}});
            }
            if (!fs.status()) return fs.status();
            std::unordered_map<long long, std::pair<size_t, size_t>> support_index;
            auto ss = db_->prepare("SELECT id,fact_id,state FROM knowledge_support_sets ORDER BY id");
            while (ss.step()) {
                if (support_index.size() >= 400'000) return make_error("too_large", "The knowledge graph exceeds the preview limit.");
                const auto fact = fact_index.find(ss.col_int(1));
                if (fact == fact_index.end()) return make_error("storage", "A support refers to a missing fact.");
                auto& supports = facts[fact->second].supports;
                const auto state = ss.col_text(2);
                support_index.emplace(ss.col_int(0), std::pair{fact->second, supports.size()});
                supports.push_back({state == "current" || state == "unverified", {}, {}});
            }
            if (!ss.status()) return ss.status();
            auto es = db_->prepare("SELECT id FROM knowledge_evidence");
            while (es.step()) {
                if (evidence.size() >= 2'000'000) return make_error("too_large", "The knowledge graph exceeds the preview limit.");
                evidence.insert(es.col_int(0));
            }
            if (!es.status()) return es.status();
            size_t input_count = 0;
            auto se = db_->prepare("SELECT support_id,evidence_id FROM knowledge_support_evidence");
            while (se.step()) {
                if (++input_count > 2'000'000) return make_error("too_large", "The knowledge graph exceeds the preview limit.");
                const auto support = support_index.find(se.col_int(0));
                if (support == support_index.end()) return make_error("storage", "Evidence refers to a missing support.");
                facts[support->second.first].supports[support->second.second].evidence.push_back(se.col_int(1));
            }
            if (!se.status()) return se.status();
            auto sf = db_->prepare("SELECT support_id,required_fact_id FROM knowledge_support_facts");
            while (sf.step()) {
                if (++input_count > 2'000'000) return make_error("too_large", "The knowledge graph exceeds the preview limit.");
                const auto support = support_index.find(sf.col_int(0));
                if (support == support_index.end()) return make_error("storage", "A dependency refers to a missing support.");
                facts[support->second.first].supports[support->second.second].required_facts.push_back(sf.col_int(1));
            }
            return sf.status();
        }();
        if (!read) {
            (void)db_->exec("ROLLBACK");
            return read.error();
        }
        if (auto status = db_->exec("COMMIT"); !status) {
            (void)db_->exec("ROLLBACK");
            return status.error();
        }
    }
    auto impact = reconcile_impact(facts, evidence, removed);
    if (!impact) return make_error("invalid_graph", "The knowledge snapshot is incomplete or contains cyclic dependencies.");
    return KnowledgeImpactPreview{generation, false, std::move(*impact)};
}

Result<PreparedItemRemoval> KgStore::prepare_item_removal(long long item_id, long long now) {
    constexpr long long kLifetime = 10 * 60 * 1000;
    if (now <= 0 || now > std::numeric_limits<long long>::max() - kLifetime)
        return make_error("invalid", "Invalid preview time.");
    auto impact = preview_item_removal(item_id);
    if (!impact) return impact.error();
    const auto token = random_urlsafe(24);
    if (token.size() != 32) return make_error("crypto", "Could not create a preview token.");
    const std::lock_guard lock(db_->mutex());
    const auto status = db_->transaction([&]() -> Status {
        const auto generation = knowledge_generation();
        if (!generation || *generation != impact->generation) return make_error("stale", "Knowledge changed while the preview was prepared.");
        auto expired = db_->prepare("DELETE FROM knowledge_previews WHERE expires_at<=?1");
        expired.bind(1, now);
        if (auto result = expired.run(); !result) return result;
        auto count = db_->prepare("SELECT count(*) FROM knowledge_previews");
        if (!count.step()) {
            if (!count.status()) return count.status();
            return make_error("database", "Could not count knowledge previews.");
        }
        if (count.col_int(0) >= 256) return make_error("capacity", "Too many active knowledge previews; cancel one or try again later.");
        auto insert = db_->prepare("INSERT INTO knowledge_previews(token,operation,item_id,generation,expires_at) "
                                   "VALUES(?1,'forget_item',?2,?3,?4)");
        insert.bind(1, token).bind(2, item_id).bind(3, impact->generation).bind(4, now + kLifetime);
        return insert.run();
    });
    if (!status) return status.error();
    return PreparedItemRemoval{token, std::move(*impact)};
}

Status KgStore::cancel_item_removal(const std::string& token) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge impact previews are not available yet.");
    if (token.size() != 32) return make_error("invalid", "Invalid preview token.");
    auto remove = db_->prepare("DELETE FROM knowledge_previews WHERE token=?1 AND operation='forget_item'");
    remove.bind(1, token);
    return remove.run();
}

Status KgStore::confirm_item_removal(const std::string& token, bool accept_incomplete, long long now) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgEvidenceSchemaVersion) return make_error("schema", "Knowledge impact previews are not available yet.");
    if (token.size() != 32 || now <= 0) return make_error("invalid", "Invalid preview confirmation.");
    if (!accept_incomplete) return make_error("incomplete", "This preview does not include all analysis or publication effects.");
    return db_->transaction([&]() -> Status {
        long long id = 0, expected = 0, expires = 0;
        std::string source, ext_id;
        {
            auto preview = db_->prepare("SELECT p.item_id,p.generation,p.expires_at,i.source,i.ext_id "
                                        "FROM knowledge_previews p JOIN items i ON i.id=p.item_id "
                                        "WHERE p.token=?1 AND p.operation='forget_item'");
            preview.bind(1, token);
            if (!preview.step()) {
                if (!preview.status()) return preview.status();
                return make_error("stale", "The preview is no longer available.");
            }
            id = preview.col_int(0);
            expected = preview.col_int(1);
            expires = preview.col_int(2);
            source = preview.col_text(3);
            ext_id = preview.col_text(4);
        }
        const auto generation = knowledge_generation();
        if (now >= expires || !generation || *generation != expected)
            return make_error("stale", "Knowledge changed or the preview expired; review it again.");
        if (auto result = purge_recovery_copy(); !result) return result;
        if (auto result = queue_item_withdrawals(*db_, source, ext_id); !result) return result;
        if (source != "notes") {
            auto exclusion = db_->prepare("INSERT OR IGNORE INTO user_exclusions(kind,value) VALUES('item',?1)");
            exclusion.bind(1, source + ":" + ext_id);
            if (auto result = exclusion.run(); !result) return result;
        }
        auto facts = db_->prepare("DELETE FROM facts_user WHERE (item_source=?1 AND item_ext_id=?2) OR (?1='threads' AND thread_id=?2)");
        facts.bind(1, source).bind(2, ext_id);
        if (auto result = facts.run(); !result) return result;
        auto tombstones = db_->prepare("DELETE FROM fact_tombstones WHERE source=?1 AND ext_id=?2");
        tombstones.bind(1, source).bind(2, ext_id);
        if (auto result = tombstones.run(); !result) return result;
        auto item = db_->prepare("DELETE FROM items WHERE id=?1");
        item.bind(1, id);
        if (auto result = item.run(); !result) return result;
        if (db_->changes() != 1) return make_error("stale", "The indexed item is no longer available.");
        if (source == "notes") {
            auto note = db_->prepare("DELETE FROM notes WHERE CAST(id AS TEXT)=?1");
            note.bind(1, ext_id);
            if (auto result = note.run(); !result) return result;
        }
        auto skipped = db_->prepare("DELETE FROM skipped WHERE source=?1 AND ext_id=?2");
        skipped.bind(1, source).bind(2, ext_id);
        if (auto result = skipped.run(); !result) return result;
        return delete_orphan_entities(*db_);
    });
}

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

Status KgStore::set_file_source_with_recheck(const SourceState& st, bool changed) {
    const auto token = changed ? random_urlsafe(24) : std::string{};
    if (changed && token.size() != 32) return make_error("crypto", "Could not create a file recheck marker.");
    const std::lock_guard lock(db_->mutex());
    return db_->transaction([&]() -> Status {
        auto source = db_->prepare("INSERT OR REPLACE INTO sources(kind, user_enabled, paused, last_update, last_error) VALUES(?1,?2,?3,?4,?5)");
        source.bind(1, st.kind).bind(2, st.user_enabled ? 1 : 0).bind(3, st.paused ? 1 : 0).bind(4, st.last_update).bind(5, st.last_error);
        if (auto result = source.run(); !result) return result;
        if (!changed) return {};
        auto marker = db_->prepare("INSERT OR REPLACE INTO meta(key,value) VALUES('files.recheck_required',?1)");
        marker.bind(1, token);
        return marker.run();
    });
}

Status KgStore::clear_file_recheck(const std::string& token) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM meta WHERE key='files.recheck_required' AND value=?1 AND EXISTS "
                          "(SELECT 1 FROM sources WHERE kind='files' AND user_enabled=1 AND paused=0 AND last_error='')");
    s.bind(1, token);
    return s.run();
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
    return db_->transaction([&]() -> Status {
        if (auto status = purge_recovery_copy(); !status) return status;
        auto s = db_->prepare("DELETE FROM folders WHERE path=?1");
        s.bind(1, path);
        return s.run();
    });
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
    return db_->transaction([&]() -> Status {
        if (auto status = purge_recovery_copy(); !status) return status;
        auto s = db_->prepare("DELETE FROM documents WHERE path=?1");
        s.bind(1, path);
        return s.run();
    });
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

Status KgStore::delete_note(long long id) {
    const std::lock_guard lock(db_->mutex());
    if (!note(id)) return make_error("not_found", "no such note");
    const auto result = db_->transaction([&]() -> Status {
        if (auto s = purge_recovery_copy(); !s) return s;
        const auto sid = std::to_string(id);
        if (auto s = queue_item_withdrawals(*db_, "notes", sid); !s) return s;
        auto facts = db_->prepare("DELETE FROM facts_user WHERE item_source='notes' AND item_ext_id=?1");
        facts.bind(1, sid);
        if (auto s = facts.run(); !s) return s;
        auto tombstones = db_->prepare("DELETE FROM fact_tombstones WHERE source='notes' AND ext_id=?1");
        tombstones.bind(1, sid);
        if (auto s = tombstones.run(); !s) return s;
        auto item = db_->prepare("DELETE FROM items WHERE source='notes' AND ext_id=?1");
        item.bind(1, sid);
        if (auto s = item.run(); !s) return s;
        auto skipped = db_->prepare("DELETE FROM skipped WHERE source='notes' AND ext_id=?1");
        skipped.bind(1, sid);
        if (auto s = skipped.run(); !s) return s;
        auto original = db_->prepare("DELETE FROM notes WHERE id=?1");
        original.bind(1, id);
        if (auto s = original.run(); !s) return s;
        if (db_->changes() != 1) return make_error("storage", "The note could not be deleted.");
        return delete_orphan_entities(*db_);
    });
    if (!result) return result;
    return {};
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

std::vector<long long> KgStore::note_ids() {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT id FROM notes");
    std::vector<long long> out;
    while (s.step()) out.push_back(s.col_int(0));
    return out;
}

std::vector<NoteRow> KgStore::notes_page(int limit, std::optional<std::pair<long long, long long>> before) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kNoteColumns +
                          " FROM notes WHERE (?1=0 OR updated_at<?2 OR (updated_at=?2 AND id<?1))"
                          " ORDER BY updated_at DESC, id DESC LIMIT ?3");
    s.bind(1, before ? before->second : 0).bind(2, before ? before->first : 0).bind(3, static_cast<long long>(limit));
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

namespace {

// A revision or removal touching more than kStepThreshold passages first withdraws its findings, kWithdrawStep
// passages per transaction. Below the threshold one transaction is faster overall and short enough.
constexpr size_t kStepThreshold = 2048, kWithdrawStep = 256;

// How a new version of an item maps onto its stored passages. Identical passages keep their identity and
// embeddings, including when they move; a heading change is a changed passage; duplicates match one at a time.
struct PassagePlan {
    std::vector<long long> old_ids;
    std::map<long long, long long> old_dates, old_ords;
    std::vector<long long> retained;  // per new passage: the stored passage it keeps, or 0
    std::set<long long> used;
    bool unchanged_context = false;
    size_t inserted() const { return static_cast<size_t>(std::count(retained.begin(), retained.end(), 0LL)); }
    size_t dropped() const { return old_ids.size() - used.size(); }
};

// The hidden item key of a real item: one stage per item, retired items suffixed with their own id.
std::string hidden_key(const std::string& source, const std::string& ext_id) { return source + ":" + ext_id; }

PassagePlan plan_passages(Database& db, long long item_id, const std::optional<ItemRow>& previous, const ItemInput& in) {
    PassagePlan plan;
    std::map<std::pair<std::string, std::string>, std::queue<long long>> available;
    std::vector<Chunk> old_passages;
    auto old = db.prepare("SELECT id, heading, text, at, ord FROM chunks WHERE item_id=?1 ORDER BY ord, id");
    old.bind(1, item_id);
    while (old.step()) {
        const long long chunk_id = old.col_int(0);
        plan.old_ids.push_back(chunk_id);
        plan.old_dates[chunk_id] = old.col_int(3);
        plan.old_ords[chunk_id] = old.col_int(4);
        old_passages.push_back({old.col_text(1), old.col_text(2), old.col_int(3)});
        available[{old.col_text(1), old.col_text(2)}].push(chunk_id);
    }
    plan.unchanged_context = previous && previous->updated_at == in.updated_at && old_passages.size() == in.chunks.size();
    for (size_t n = 0; plan.unchanged_context && n < in.chunks.size(); ++n)
        if (old_passages[n].heading != in.chunks[n].heading || old_passages[n].text != in.chunks[n].text ||
            old_passages[n].at != in.chunks[n].at)
            plan.unchanged_context = false;
    for (const auto& c : in.chunks) {
        auto& matches = available[{c.heading, c.text}];
        if (matches.empty()) plan.retained.push_back(0);
        else {
            plan.retained.push_back(matches.front());
            plan.used.insert(matches.front());
            matches.pop();
        }
    }
    return plan;
}

// Whether a kept passage's findings must be extracted again. Legacy relations lack complete dependency and date
// provenance, so without a record they are rechecked whenever the item's passages or date context change. The
// recorded extraction inputs narrow that: a removed or re-dated input clears the record itself, so what is left to
// check is the passage's own date and a date taken from the item.
// `recorded` is kRecordedInputs prepared once by the caller, because a large item asks for every kept passage; null
// without the evidence schema.
constexpr const char* kRecordedInputs = "SELECT item_date FROM knowledge_extraction_inputs WHERE chunk_id=?1 AND result=1";

bool recheck_retained(Statement* recorded, const PassagePlan& plan, const std::optional<ItemRow>& previous, const ItemInput& in,
                      long long chunk_id, long long at) {
    if (!previous) return false;
    const bool redated = plan.old_dates.at(chunk_id) != at;
    if (redated) return true;
    if (plan.unchanged_context) return false;
    if (!recorded) return true;
    recorded->reset();
    recorded->bind(1, chunk_id);
    if (!recorded->step()) return true;
    return recorded->col_int(0) != 0 && previous->updated_at != in.updated_at;
}

// Splits a stepped operation so that each step's cascade stays bounded. Withdrawing a finding or deleting an excerpt
// also invalidates or removes every fact derived from it, through required facts, so a step is sized by that work
// rather than by its count: each id costs 1 plus the facts that depend on what it supports (to the depth bound),
// and a step takes ids in order up to kWithdrawStep of cost, at least one. `seeds` turns an id list into
// "SELECT k, fact" rows: the facts each id currently supports. Costs are read kCostedPerLock ids per lock hold.
std::vector<std::vector<long long>> cascade_steps(Database& db, const std::vector<long long>& ids,
                                                  const std::function<std::string(const std::string&)>& seeds) {
    constexpr size_t kCostedPerLock = 1024;
    std::map<long long, long long> dependents;
    for (size_t at = 0; at < ids.size(); at += kCostedPerLock) {
        const std::lock_guard lock(db.mutex());
        const std::vector<long long> part(ids.begin() + static_cast<std::ptrdiff_t>(at),
                                          ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), at + kCostedPerLock)));
        auto s = db.prepare("WITH RECURSIVE dep(k,fact,d) AS (SELECT k,fact,0 FROM (" + seeds(id_list(part)) +
                            ") UNION SELECT dep.k,s.fact_id,dep.d+1 FROM dep JOIN knowledge_support_facts x ON x.required_fact_id=dep.fact "
                            "JOIN knowledge_support_sets s ON s.id=x.support_id WHERE dep.d<" + std::to_string(kMaxDerivationDepth) +
                            ") SELECT k,sum(d>0) FROM dep GROUP BY k");
        while (s.step()) dependents[s.col_int(0)] = s.col_int(1);
    }
    std::vector<std::vector<long long>> steps;
    long long cost = 0;
    for (const long long id : ids) {
        const auto found = dependents.find(id);
        const long long own = 1 + (found == dependents.end() ? 0 : found->second);
        if (steps.empty() || (cost + own > static_cast<long long>(kWithdrawStep) && !steps.back().empty())) {
            steps.emplace_back();
            cost = 0;
        }
        steps.back().push_back(id);
        cost += own;
    }
    return steps;
}

}  // namespace

// ---- Operation previews (Knowledge Impact Preview, Revision-Safe Impact Confirmation; task 8.3) -------------------------

ItemChangePlan KgStore::plan_item_change(const ItemInput& in) {
    const std::lock_guard lock(db_->mutex());
    ItemChangePlan out;
    const auto previous = item(in.source, in.ext_id);
    const auto plan = plan_passages(*db_, previous ? previous->id : 0, previous, in);
    out.previous_hash = previous ? previous->hash : std::string{};
    std::optional<Statement> recorded;
    if (db_->user_version() >= kKgEvidenceSchemaVersion) recorded.emplace(db_->prepare(kRecordedInputs));
    // The same decisions put_item makes for each passage.
    for (size_t n = 0; n < in.chunks.size(); ++n) {
        const auto& c = in.chunks[n];
        const long long kept = plan.retained[n];
        PassageChange p{static_cast<int>(n), kept, "added", c.heading, c.text};
        if (kept) {
            if (plan.old_dates.at(kept) != c.at) {
                p.effect = "redated";
                out.redated.push_back(kept);
            } else if (recheck_retained(recorded ? &*recorded : nullptr, plan, previous, in, kept, c.at)) {
                p.effect = "rechecked";
                out.rechecked.push_back(kept);
            } else p.effect = "kept";
        }
        out.passages.push_back(std::move(p));
    }
    auto text = db_->prepare("SELECT heading,text FROM chunks WHERE id=?1");
    for (long long id : plan.old_ids) {
        if (plan.used.count(id)) continue;
        out.removed.push_back(id);
        text.reset();
        text.bind(1, id);
        PassageChange p{-1, id, "removed", {}, {}};
        if (text.step()) {
            p.heading = text.col_text(0);
            p.text = text.col_text(1);
        }
        out.passages.push_back(std::move(p));
    }
    return out;
}

Result<KnowledgeImpactPreview> KgStore::preview_passage_changes(const std::vector<long long>& removed, const std::vector<long long>& redated,
                                                                const std::vector<long long>& rechecked, const std::vector<long long>& items) {
    constexpr size_t kPart = 500;
    const auto in_parts = [&](const std::vector<long long>& ids, const std::function<void(const std::string&)>& each) {
        for (size_t at = 0; at < ids.size(); at += kPart)
            each(id_list(std::vector<long long>(ids.begin() + static_cast<std::ptrdiff_t>(at),
                                                ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), at + kPart)))));
    };
    // The graph is read twice (the ids here, the support graph in preview_evidence_snapshot); both must be one
    // generation, or the preview is prepared again.
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::unordered_set<long long> evidence;
        long long generation = 0, categories = 0, reviews = 0;
        {
            const std::lock_guard lock(db_->mutex());
            if (db_->user_version() < kKgPreviewSchemaVersion) return make_error("schema", "Knowledge impact previews are not available yet.");
            const auto g = generation_of(*db_);
            if (!g) return make_error("storage", "The knowledge generation is unavailable.");
            generation = *g;
            std::set<long long> chunks(removed.begin(), removed.end());
            chunks.insert(redated.begin(), redated.end());
            chunks.insert(rechecked.begin(), rechecked.end());
            std::vector<long long> shifted(removed);  // their extraction requests are checked again
            shifted.insert(shifted.end(), redated.begin(), redated.end());
            for (long long id : items) {
                auto of_item = db_->prepare("SELECT id FROM chunks WHERE item_id=?1");
                of_item.bind(1, id);
                while (of_item.step()) {
                    chunks.insert(of_item.col_int(0));
                    shifted.push_back(of_item.col_int(0));
                }
                auto all = db_->prepare("SELECT e.id FROM knowledge_evidence e JOIN knowledge_revisions v ON v.id=e.revision_id WHERE v.item_id=?1");
                all.bind(1, id);
                while (all.step()) evidence.insert(all.col_int(0));
            }
            in_parts(shifted, [&](const std::string& ids) {
                auto beside = db_->prepare("SELECT DISTINCT d.chunk_id FROM knowledge_extraction_inputs d WHERE d.result=1 AND d.batch_id IN "
                                           "(SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id IN (" + ids + "))");
                while (beside.step()) chunks.insert(beside.col_int(0));
            });
            in_parts(std::vector<long long>(chunks.begin(), chunks.end()), [&](const std::string& ids) {
                auto current = db_->prepare("SELECT e.id FROM knowledge_evidence e JOIN knowledge_revisions v ON v.id=e.revision_id "
                                            "AND v.current=1 WHERE e.chunk_id IN (" + ids + ")");
                while (current.step()) evidence.insert(current.col_int(0));
            });
            const std::vector<long long> withdrawn(evidence.begin(), evidence.end());
            in_parts(withdrawn, [&](const std::string& ids) {
                auto c = db_->prepare("SELECT count(*) FROM knowledge_category_assignments WHERE decision='automatic' AND evidence_id IN (" + ids + ")");
                if (c.step()) categories += c.col_int(0);
            });
            // A review counts once even when both of its excerpts are withdrawn.
            std::set<long long> affected_reviews;
            in_parts(withdrawn, [&](const std::string& ids) {
                auto r = db_->prepare("SELECT id FROM knowledge_conflicts WHERE state<>'needs_recheck' AND (left_evidence IN (" + ids +
                                      ") OR right_evidence IN (" + ids + "))");
                while (r.step()) affected_reviews.insert(r.col_int(0));
            });
            reviews = static_cast<long long>(affected_reviews.size());
        }
        auto impact = preview_evidence_snapshot(evidence, std::nullopt);
        if (!impact) return impact.error();
        if (impact->generation != generation) continue;
        std::erase_if(impact->facts, [](const FactImpact& f) { return f.after >= f.before; });
        impact->evidence = static_cast<long long>(evidence.size());
        impact->automatic_categories = categories;
        impact->reviews = reviews;
        return impact;
    }
    return make_error("stale", "Knowledge kept changing while the preview was prepared; try again.");
}

Result<std::string> KgStore::save_operation_preview(OperationPreview p, const std::vector<PreviewEntry>& entries) {
    constexpr long long kLifetime = 30 * 60 * 1000;
    const auto token = random_urlsafe(24);
    if (token.size() != 32) return make_error("crypto", "Could not create a preview token.");
    if (p.created_at <= 0 || p.created_at > std::numeric_limits<long long>::max() - kLifetime)
        return make_error("invalid", "Invalid preview time.");
    p.token = token;
    p.expires_at = p.created_at + kLifetime;
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgPreviewSchemaVersion) return make_error("schema", "Knowledge impact previews are not available yet.");
    const auto status = db_->transaction([&]() -> Status {
        auto expired = db_->prepare("DELETE FROM knowledge_operation_previews WHERE expires_at<=?1");
        expired.bind(1, p.created_at);
        if (auto s = expired.run(); !s) return s;
        auto count = db_->prepare("SELECT count(*) FROM knowledge_operation_previews");
        if (!count.step()) return make_error("storage", "Could not count knowledge previews.");
        if (count.col_int(0) >= 32) return make_error("capacity", "Too many open previews; confirm or cancel one first.");
        auto insert = db_->prepare("INSERT INTO knowledge_operation_previews(token,operation,target,content,content_hash,policy,summary,"
                                   "generation,created_at,expires_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
        insert.bind(1, p.token).bind(2, p.operation).bind(3, p.target).bind(4, p.content).bind(5, p.content_hash).bind(6, p.policy)
            .bind(7, p.summary).bind(8, p.generation).bind(9, p.created_at).bind(10, p.expires_at);
        if (auto s = insert.run(); !s) return s;
        auto entry = db_->prepare("INSERT INTO knowledge_preview_entries(token,kind,ord,ref,effect,before,after,text) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
        std::map<std::string, long long> ord;
        for (const auto& e : entries) {
            entry.reset();
            entry.bind(1, p.token).bind(2, e.kind).bind(3, ord[e.kind]++).bind(4, e.ref).bind(5, e.effect)
                .bind(6, static_cast<long long>(e.before)).bind(7, static_cast<long long>(e.after)).bind(8, e.text);
            if (auto s = entry.run(); !s) return s;
        }
        return {};
    });
    if (!status) return status.error();
    return token;
}

std::optional<OperationPreview> KgStore::operation_preview(const std::string& token) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgPreviewSchemaVersion || token.size() != 32) return std::nullopt;
    auto s = db_->prepare("SELECT token,operation,target,content,content_hash,policy,summary,generation,created_at,expires_at "
                          "FROM knowledge_operation_previews WHERE token=?1");
    s.bind(1, token);
    if (!s.step()) return std::nullopt;
    return OperationPreview{s.col_text(0), s.col_text(1), s.col_text(2), s.col_text(3), s.col_text(4), s.col_text(5), s.col_text(6),
                            s.col_int(7), s.col_int(8), s.col_int(9)};
}

std::vector<PreviewEntry> KgStore::preview_entries(const std::string& token, const std::string& kind, int offset, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<PreviewEntry> out;
    if (db_->user_version() < kKgPreviewSchemaVersion || limit <= 0 || offset < 0) return out;
    auto s = db_->prepare("SELECT kind,effect,ref,before,after,text FROM knowledge_preview_entries WHERE token=?1 AND kind=?2 "
                          "AND ord>=?3 ORDER BY ord LIMIT ?4");
    s.bind(1, token).bind(2, kind).bind(3, static_cast<long long>(offset)).bind(4, static_cast<long long>(std::min(limit, 500)));
    while (s.step())
        out.push_back({s.col_text(0), s.col_text(1), s.col_int(2), static_cast<int>(s.col_int(3)), static_cast<int>(s.col_int(4)), s.col_text(5)});
    return out;
}

long long KgStore::preview_entry_count(const std::string& token, const std::string& kind) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgPreviewSchemaVersion) return 0;
    auto s = db_->prepare("SELECT count(*) FROM knowledge_preview_entries WHERE token=?1 AND kind=?2");
    s.bind(1, token).bind(2, kind);
    return s.step() ? s.col_int(0) : 0;
}

Status KgStore::delete_operation_preview(const std::string& token) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgPreviewSchemaVersion) return {};
    auto s = db_->prepare("DELETE FROM knowledge_operation_previews WHERE token=?1");
    s.bind(1, token);
    return s.run();
}

Status KgStore::delete_item_previews(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgPreviewSchemaVersion) return {};
    auto s = db_->prepare("DELETE FROM knowledge_operation_previews WHERE json_extract(target,'$.source')=?1 "
                          "AND (?2='' OR json_extract(target,'$.ext_id')=?2)");
    s.bind(1, source).bind(2, ext_id);
    return s.run();
}

Status KgStore::purge_expired_previews(long long now) {
    const std::lock_guard lock(db_->mutex());
    if (db_->user_version() < kKgPreviewSchemaVersion) return {};
    auto s = db_->prepare("DELETE FROM knowledge_operation_previews WHERE expires_at<=?1");
    s.bind(1, now);
    return s.run();
}

// ---- Source change reports (task 8.4) ----------------------------------------------------------------------------------

std::map<long long, long long> KgStore::support_counts_at_risk(long long item_id, bool findings_only) {
    // Every fact one of whose current supports rests on a passage of the item, or on a passage analyzed in one request
    // with one (its findings are extracted again when that passage moves), and the facts derived from those. A
    // superset of what a revision can affect: facts that keep their supports are left out of the report afterwards.
    std::map<long long, long long> out;
    // After a stepped withdrawal only findings recorded since then can still be taken: those on the item's passages.
    if (findings_only) {
        auto s = db_->prepare(R"sql(
WITH RECURSIVE f(id) AS (
   SELECT s.fact_id FROM chunks c JOIN relations r ON r.chunk_id=c.id JOIN knowledge_support_sets s ON s.legacy_relation_id=r.id
     WHERE c.item_id=?1 AND s.state IN ('current','unverified')
   UNION SELECT s.fact_id FROM f JOIN knowledge_support_facts sf ON sf.required_fact_id=f.id JOIN knowledge_support_sets s ON s.id=sf.support_id)
SELECT f.id,(SELECT count(*) FROM knowledge_support_sets s WHERE s.fact_id=f.id AND s.state IN ('current','unverified')) FROM f)sql");
        s.bind(1, item_id);
        while (s.step()) out[s.col_int(0)] = s.col_int(1);
        return out;
    }
    auto s = db_->prepare(R"sql(
WITH RECURSIVE
 own(id) AS (SELECT id FROM chunks WHERE item_id=?1),
 ch(id) AS (SELECT id FROM own UNION SELECT d.chunk_id FROM knowledge_extraction_inputs d WHERE d.result=1
            AND d.batch_id IN (SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id IN (SELECT id FROM own))),
 f(id) AS (
   SELECT s.fact_id FROM knowledge_evidence e JOIN knowledge_support_evidence se ON se.evidence_id=e.id
     JOIN knowledge_support_sets s ON s.id=se.support_id WHERE e.chunk_id IN (SELECT id FROM ch) AND s.state IN ('current','unverified')
   UNION SELECT s.fact_id FROM relations r JOIN knowledge_support_sets s ON s.legacy_relation_id=r.id
     WHERE r.chunk_id IN (SELECT id FROM ch) AND s.state IN ('current','unverified')
   UNION SELECT s.fact_id FROM f JOIN knowledge_support_facts sf ON sf.required_fact_id=f.id JOIN knowledge_support_sets s ON s.id=sf.support_id)
SELECT f.id,(SELECT count(*) FROM knowledge_support_sets s WHERE s.fact_id=f.id AND s.state IN ('current','unverified')) FROM f)sql");
    s.bind(1, item_id);
    while (s.step()) out[s.col_int(0)] = s.col_int(1);
    return out;
}

std::vector<ChangeReport> KgStore::change_reports(long long before_id, int limit, long long item_id) {
    const std::lock_guard lock(db_->mutex());
    std::vector<ChangeReport> out;
    if (db_->user_version() < kKgSchemaVersion || limit <= 0) return out;
    auto s = db_->prepare("SELECT r.id,r.item_id,r.revision,r.created_at,r.added,r.kept,r.redated,r.rechecked,r.removed,r.facts,r.unsupported,"
                          "i.source,i.ext_id,i.title FROM knowledge_change_reports r JOIN items i ON i.id=r.item_id "
                          "WHERE (?1=0 OR r.id<?1) AND (?2=0 OR r.item_id=?2) ORDER BY r.id DESC LIMIT ?3");
    s.bind(1, before_id).bind(2, item_id).bind(3, static_cast<long long>(std::min(limit, 500)));
    while (s.step())
        out.push_back({s.col_int(0), s.col_int(1), s.col_int(2), s.col_int(3), s.col_int(4), s.col_int(5), s.col_int(6), s.col_int(7),
                       s.col_int(8), s.col_int(9), s.col_int(10), s.col_text(11), s.col_text(12), s.col_text(13)});
    return out;
}

std::optional<ChangeReport> KgStore::change_report(long long id) {
    if (id <= 0) return std::nullopt;
    auto one = change_reports(id + 1, 1);
    if (one.empty() || one.front().id != id) return std::nullopt;
    return one.front();
}

std::vector<ChangeReportFact> KgStore::change_report_facts(long long report_id, int offset, int limit) {
    const std::lock_guard lock(db_->mutex());
    std::vector<ChangeReportFact> out;
    if (db_->user_version() < kKgSchemaVersion || limit <= 0 || offset < 0) return out;
    auto s = db_->prepare("SELECT fact_id,before,after FROM knowledge_change_report_facts WHERE report_id=?1 "
                          "ORDER BY after>0,fact_id LIMIT ?2 OFFSET ?3");
    s.bind(1, report_id).bind(2, static_cast<long long>(std::min(limit, 500))).bind(3, static_cast<long long>(offset));
    while (s.step()) out.push_back({s.col_int(0), s.col_int(1), s.col_int(2)});
    return out;
}

Status KgStore::withdraw_findings(const std::vector<long long>& chunk_ids) {
    const auto steps = cascade_steps(*db_, chunk_ids, [](const std::string& ids) {
        return "SELECT r.chunk_id AS k,s.fact_id AS fact FROM relations r JOIN knowledge_support_sets s ON s.legacy_relation_id=r.id "
               "WHERE r.chunk_id IN (" + ids + ") AND s.state IN ('current','unverified')";
    });
    for (size_t at = 0; at < steps.size(); ++at) {
        // Each step is its own transaction, and the lock is released between steps so that searches, the indexer
        // and a concurrent forget are not held behind one large item. The lock is not fair: pause so a waiting
        // thread actually gets it.
        if (at) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::lock_guard lock(db_->mutex());
        if (!sqlite3_get_autocommit(db_->raw())) return {};
        const std::string ids = id_list(steps[at]);
        const auto step = db_->transaction([&]() -> Status {
            // A passage that had findings is queued again first, so that if the change never completes the
            // passage recovers its findings instead of silently keeping fewer.
            if (Status s = db_->exec("INSERT OR IGNORE INTO extract_queue(chunk_id,priority) "
                                     "SELECT chunk_id,0 FROM relations WHERE chunk_id IN (" + ids + ") "
                                     "UNION SELECT chunk_id,0 FROM mentions WHERE chunk_id IN (" + ids + ")");
                !s)
                return s;
            if (Status s = db_->exec("UPDATE knowledge_extraction_inputs SET result=0,item_date=0 WHERE result=1 AND chunk_id IN (" + ids + ")"); !s)
                return s;
            if (Status s = db_->exec("DELETE FROM relations WHERE chunk_id IN (" + ids + ")"); !s) return s;
            return db_->exec("DELETE FROM mentions WHERE chunk_id IN (" + ids + ")");
        });
        if (!step) return step;
    }
    return {};
}

Status KgStore::prepare_item_change(const ItemInput& in) {
    // Leftovers of an earlier switch first, so that retired passages stay bounded.
    if (Status s = finish_item_changes(); !s) return s;
    std::vector<long long> withdraw;
    {
        const std::lock_guard lock(db_->mutex());
        if (db_->user_version() < kKgEvidenceSchemaVersion || !sqlite3_get_autocommit(db_->raw())) return {};
        // A new item has nothing to withdraw; it may still be staged below.
        const auto previous = item(in.source, in.ext_id);
        const auto plan = plan_passages(*db_, previous ? previous->id : 0, previous, in);
        std::vector<long long> moved;  // removed or re-dated: the passages extracted beside them are rechecked too
        for (const long long id : plan.old_ids)
            if (!plan.used.count(id)) moved.push_back(id);
        withdraw = moved;
        auto recorded = db_->prepare(kRecordedInputs);
        for (size_t n = 0; previous && n < in.chunks.size(); ++n) {
            const long long id = plan.retained[n];
            if (!id) continue;
            if (plan.old_dates.at(id) != in.chunks[n].at) moved.push_back(id);
            if (recheck_retained(&recorded, plan, previous, in, id, in.chunks[n].at)) withdraw.push_back(id);
        }
        if (withdraw.size() <= kStepThreshold) withdraw.clear();
        else if (!moved.empty()) {
            auto beside = db_->prepare("SELECT DISTINCT d.chunk_id FROM knowledge_extraction_inputs d WHERE d.result=1 AND d.batch_id IN "
                                       "(SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id IN (" + id_list(moved) + "))");
            while (beside.step()) withdraw.push_back(beside.col_int(0));
        }
        // Withdrawn in steps before the switch: the change report starts from the supports as they are now.
        if (!withdraw.empty() && previous && db_->user_version() >= kKgSchemaVersion) {
            auto& [hash, counts] = report_before_[hidden_key(in.source, in.ext_id)];
            if (hash != in.hash) counts.clear();
            hash = in.hash;
            for (const auto& [fact, n] : support_counts_at_risk(previous->id, false)) counts[fact] = std::max(counts[fact], n);
        }
    }
    std::sort(withdraw.begin(), withdraw.end());
    withdraw.erase(std::unique(withdraw.begin(), withdraw.end()), withdraw.end());
    if (Status s = withdraw_findings(withdraw); !s) return s;
    return stage_passages(in);
}

// The new passages of a large change are written beforehand, in steps, to a hidden item that no reader treats as a
// Source; the switch in put_item then moves them to the item instead of inserting and indexing them. A stage is keyed
// by the item and the content hash, and by each passage's position in the new content, so an interrupted stage
// resumes. The switch checks every staged passage against the content it writes.
Status KgStore::stage_passages(const ItemInput& in) {
    const std::string key = hidden_key(in.source, in.ext_id);
    std::vector<size_t> pending;
    long long stage = 0;
    {
        const std::lock_guard lock(db_->mutex());
        if (db_->user_version() < kKgEvidenceSchemaVersion || !sqlite3_get_autocommit(db_->raw())) return {};
        const auto previous = item(in.source, in.ext_id);
        const auto plan = plan_passages(*db_, previous ? previous->id : 0, previous, in);
        if (plan.inserted() + plan.dropped() <= kStepThreshold) return {};
        const auto created = db_->transaction([&]() -> Status {
            // The stage of another version of the item is discarded with the retired passages.
            auto retire = db_->prepare("UPDATE items SET source=?1, ext_id=ext_id||'#'||id WHERE source=?2 AND ext_id=?3 AND hash<>?4");
            retire.bind(1, kRetiredSource).bind(2, kStagedSource).bind(3, key).bind(4, in.hash);
            if (Status s = retire.run(); !s) return s;
            auto create = db_->prepare("INSERT OR IGNORE INTO items(source, ext_id, hash) VALUES(?1,?2,?3)");
            create.bind(1, kStagedSource).bind(2, key).bind(3, in.hash);
            if (Status s = create.run(); !s) return s;
            auto find = db_->prepare("SELECT id FROM items WHERE source=?1 AND ext_id=?2");
            find.bind(1, kStagedSource).bind(2, key);
            if (!find.step()) return make_error("storage", "the staged passages could not be recorded");
            stage = find.col_int(0);
            return {};
        });
        if (!created) return created;
        std::set<long long> staged;
        auto existing = db_->prepare("SELECT ord FROM chunks WHERE item_id=?1");
        existing.bind(1, stage);
        while (existing.step()) staged.insert(existing.col_int(0));
        for (size_t n = 0; n < in.chunks.size(); ++n)
            if (!plan.retained[n] && !staged.count(static_cast<long long>(n))) pending.push_back(n);
    }
    for (size_t at = 0; at < pending.size(); at += kWithdrawStep) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::lock_guard lock(db_->mutex());
        if (!sqlite3_get_autocommit(db_->raw())) return {};
        bool current = false;
        const auto step = db_->transaction([&]() -> Status {
            // A forget, a discard or a newer version may have retired the stage meanwhile.
            auto check = db_->prepare("SELECT 1 FROM items WHERE id=?1 AND source=?2 AND hash=?3");
            check.bind(1, stage).bind(2, kStagedSource).bind(3, in.hash);
            if (!(current = check.step())) return {};
            auto insert = db_->prepare("INSERT INTO chunks(item_id, ord, heading, text, at, tokens) VALUES(?1,?2,?3,?4,?5,?6)");
            for (size_t k = at; k < std::min(pending.size(), at + kWithdrawStep); ++k) {
                const auto& c = in.chunks[pending[k]];
                insert.reset();
                insert.bind(1, stage).bind(2, static_cast<long long>(pending[k])).bind(3, c.heading).bind(4, c.text).bind(5, c.at);
                insert.bind(6, estimate_tokens(c.text));
                if (Status s = insert.run(); !s) return s;
            }
            return {};
        });
        if (!step) return step;
        if (!current) return {};
    }
    return {};
}

Status KgStore::finish_item_changes() {
    const std::string retired = std::string("SELECT c.id FROM items i JOIN chunks c ON c.item_id=i.id WHERE i.source='") + kRetiredSource +
                                "' LIMIT " + std::to_string(kWithdrawStep);
    for (bool first = true;; first = false) {
        if (!first) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::lock_guard lock(db_->mutex());
        if (db_->user_version() < kKgEvidenceSchemaVersion || !sqlite3_get_autocommit(db_->raw())) return {};
        int removed = 0;
        const auto step = db_->transaction([&]() -> Status {
            if (Status s = db_->exec("DELETE FROM chunks WHERE id IN (" + retired + ")"); !s) return s;
            if ((removed = db_->changes())) return {};
            return db_->exec(std::string("DELETE FROM items WHERE source='") + kRetiredSource + "'");
        });
        if (!step) return step;
        if (!removed) return {};
    }
}

Status KgStore::discard_item_change(const std::string& source, const std::string& ext_id) {
    {
        const std::lock_guard lock(db_->mutex());
        if (db_->user_version() < kKgEvidenceSchemaVersion) return {};
        report_before_.erase(hidden_key(source, ext_id));
        auto retire = db_->prepare("UPDATE items SET source=?1, ext_id=ext_id||'#'||id WHERE source=?2 AND ext_id=?3");
        retire.bind(1, kRetiredSource).bind(2, kStagedSource).bind(3, hidden_key(source, ext_id));
        if (Status s = retire.run(); !s) return s;
    }
    return finish_item_changes();
}

Status KgStore::prepare_item_removal_steps(const std::string& source, const std::string& ext_id) {
    long long item_id = 0;
    std::vector<long long> passages;
    {
        const std::lock_guard lock(db_->mutex());
        if (db_->user_version() < kKgEvidenceSchemaVersion || !sqlite3_get_autocommit(db_->raw())) return {};
        const auto existing = item(source, ext_id);
        if (!existing) return {};
        item_id = existing->id;
        auto s = db_->prepare("SELECT id FROM chunks WHERE item_id=?1 ORDER BY id");
        s.bind(1, item_id);
        while (s.step()) passages.push_back(s.col_int(0));
        if (passages.size() <= kStepThreshold) return {};
    }
    if (Status s = withdraw_findings(passages); !s) return s;
    // Then the item's excerpts, which takes the supports, category assignments and review entries that rest on them.
    // Excerpts recorded meanwhile are deleted with the item.
    std::vector<long long> excerpts;
    {
        const std::lock_guard lock(db_->mutex());
        auto s = db_->prepare("SELECT e.id FROM knowledge_evidence e JOIN knowledge_revisions v ON v.id=e.revision_id WHERE v.item_id=?1 "
                              "ORDER BY e.id");
        s.bind(1, item_id);
        while (s.step()) excerpts.push_back(s.col_int(0));
    }
    const auto steps = cascade_steps(*db_, excerpts, [](const std::string& ids) {
        return "SELECT se.evidence_id AS k,s.fact_id AS fact FROM knowledge_support_evidence se "
               "JOIN knowledge_support_sets s ON s.id=se.support_id WHERE se.evidence_id IN (" + ids + ")";
    });
    for (const auto& part : steps) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::lock_guard lock(db_->mutex());
        if (!sqlite3_get_autocommit(db_->raw())) return {};
        const auto step = db_->transaction([&]() -> Status {
            return db_->exec("DELETE FROM knowledge_evidence WHERE id IN (" + id_list(part) + ")");
        });
        if (!step) return step;
    }
    return {};
}

namespace {

// In the transaction that removes an item: its stage is retired, and so are the passages of a large item whose
// findings and excerpts were already withdrawn in steps, so that the transaction does not delete and unindex every
// passage while holding the writer. Retired passages are no longer part of any Source; finish_item_changes() deletes
// them. A passage still holding findings is left to be deleted with the item, so its triggers run here.
Status retire_removed_passages(Database& db, const std::string& source, const std::string& ext_id) {
    const std::string key = hidden_key(source, ext_id);
    auto discard = db.prepare("UPDATE items SET source=?1, ext_id=ext_id||'#'||id WHERE source=?2 AND ext_id=?3");
    discard.bind(1, kRetiredSource).bind(2, kStagedSource).bind(3, key);
    if (Status s = discard.run(); !s) return s;
    long long item = 0, passages = 0;
    {
        auto s = db.prepare("SELECT i.id, (SELECT count(*) FROM chunks WHERE item_id=i.id) FROM items i WHERE i.source=?1 AND i.ext_id=?2");
        s.bind(1, source).bind(2, ext_id);
        if (!s.step()) return {};
        item = s.col_int(0);
        passages = s.col_int(1);
    }
    if (passages <= static_cast<long long>(kStepThreshold)) return {};
    // Named like any retired stage, key#id: the next rowid, so unique.
    auto create = db.prepare("INSERT INTO items(source, ext_id) SELECT ?1, ?2||'#'||(max(id)+1) FROM items");
    create.bind(1, kRetiredSource).bind(2, key);
    if (Status s = create.run(); !s) return s;
    const std::string retired = std::to_string(db.last_insert_id()), from = std::to_string(item);
    return db.exec(
        "UPDATE chunks SET item_id=" + retired + " WHERE item_id=" + from +
        " AND NOT EXISTS(SELECT 1 FROM relations r WHERE r.chunk_id=chunks.id) AND NOT EXISTS(SELECT 1 FROM mentions m WHERE m.chunk_id=chunks.id)"
        " AND NOT EXISTS(SELECT 1 FROM knowledge_extraction_inputs d WHERE d.result=1 AND d.batch_id IN"
        " (SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id=chunks.id));"
        "DELETE FROM extract_queue WHERE chunk_id IN (SELECT id FROM chunks WHERE item_id=" + retired + ");"
        "DELETE FROM embed_queue WHERE chunk_id IN (SELECT id FROM chunks WHERE item_id=" + retired + ");");
}

}  // namespace

Result<long long> KgStore::put_item(const ItemInput& in, long long now) {
    // Optional: withdraws and stages a large change in steps; the write completes the change either way.
    (void)prepare_item_change(in);
    return put_prepared_item(in, now);
}

Result<long long> KgStore::put_prepared_item(const ItemInput& in, long long now) {
    std::unique_lock lock(db_->mutex());
    long long id = 0, report_id = 0;
    const std::string report_key = hidden_key(in.source, in.ext_id);
    std::map<long long, long long> before;  // a change report's facts with their supports before the write
    Status st = db_->transaction([&]() -> Status {
        const auto previous = item(in.source, in.ext_id);
        // A revision of an existing item gets a change report: the supports its facts have before the write.
        const bool report = previous && db_->user_version() >= kKgSchemaVersion;
        if (report) {
            // A large change counted its supports before withdrawing them in steps; the switch adds only what was
            // recorded on the item since, so it does not scan every passage again while holding the writer.
            const auto withdrawn = report_before_.find(report_key);
            const bool staged_counts = withdrawn != report_before_.end() && withdrawn->second.first == in.hash;
            before = support_counts_at_risk(previous->id, staged_counts);
            if (staged_counts)
                for (const auto& [fact, n] : withdrawn->second.second) before[fact] = std::max(before[fact], n);
        }
        if (!in.legacy_tombstone_hash.empty()) {
            auto carry = db_->prepare(
                "INSERT OR IGNORE INTO fact_tombstones(source, ext_id, item_hash, fact_key) "
                "SELECT source, ext_id, ?1, fact_key FROM fact_tombstones "
                "WHERE source=?2 AND ext_id=?3 AND item_hash=?4 "
                "AND EXISTS(SELECT 1 FROM items WHERE source=?2 AND ext_id=?3 AND hash=?4)");
            carry.bind(1, in.hash).bind(2, in.source).bind(3, in.ext_id).bind(4, in.legacy_tombstone_hash);
            if (Status s = carry.run(); !s) return s;
        }
        auto i = db_->prepare(
            "INSERT INTO items(source, ext_id, title, ref, hash, created_at, updated_at, indexed_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
            "ON CONFLICT(source, ext_id) DO UPDATE SET title=excluded.title, ref=excluded.ref, hash=excluded.hash, "
            "created_at=excluded.created_at, updated_at=excluded.updated_at, indexed_at=excluded.indexed_at");
        i.bind(1, in.source).bind(2, in.ext_id).bind(3, in.title).bind(4, in.ref).bind(5, in.hash).bind(6, in.created_at);
        i.bind(7, in.updated_at).bind(8, now);
        if (Status s = i.run(); !s) return s;
        auto existing = db_->prepare("SELECT id FROM items WHERE source=?1 AND ext_id=?2");
        existing.bind(1, in.source).bind(2, in.ext_id);
        if (!existing.step()) return make_error("storage", "the indexed item could not be found");
        id = existing.col_int(0);
        const auto plan = plan_passages(*db_, id, previous, in);
        const auto& old_ids = plan.old_ids;
        const auto& old_dates = plan.old_dates;
        const auto& retained = plan.retained;
        const auto& used = plan.used;
        const bool evidence_schema = db_->user_version() >= kKgEvidenceSchemaVersion;
        if (evidence_schema) {
            // A passage keeps its evidence only when its heading, text and date are all unchanged.
            std::vector<long long> unchanged;
            bool changed = !previous || previous->hash != in.hash || used.size() != old_ids.size();
            for (size_t n = 0; n < in.chunks.size(); ++n) {
                if (retained[n] && old_dates.at(retained[n]) == in.chunks[n].at) unchanged.push_back(retained[n]);
                else changed = true;
            }
            if (Status s = advance_revision(*db_, id, in.hash, now, changed, unchanged); !s) return s;
        }
        // A large item would run thousands of single-row statements here, and this transaction is what other writers
        // wait for: passages are handled as lists, one statement per step, and the rest is prepared once.
        auto update = db_->prepare("UPDATE chunks SET ord=?1, at=?2 WHERE id=?3");
        auto ci = db_->prepare("INSERT INTO chunks(item_id, ord, heading, text, at, tokens) VALUES(?1,?2,?3,?4,?5,?6)");
        const std::string item_id = std::to_string(id), priority = std::to_string(in.priority);
        const auto each = [&](const std::string& head, const std::vector<long long>& ids, const std::string& tail = {}) -> Status {
            return ids.empty() ? Status{} : db_->exec(head + "(" + id_list(ids) + ")" + tail);
        };
        // A staged change: its new passages are already written and indexed under the hidden stage, and the passages
        // it replaces are retired there, to be deleted in steps after the switch.
        const std::string key = hidden_key(in.source, in.ext_id);
        long long stage = 0;
        std::map<long long, ChunkRow> staged;  // by position in the new content
        if (evidence_schema) {
            auto s = db_->prepare("SELECT id FROM items WHERE source=?1 AND ext_id=?2 AND hash=?3");
            s.bind(1, kStagedSource).bind(2, key).bind(3, in.hash);
            if (s.step()) stage = s.col_int(0);
        }
        if (stage) {
            auto s = db_->prepare("SELECT id, ord, heading, text, at FROM chunks WHERE item_id=?1");
            s.bind(1, stage);
            while (s.step()) staged[s.col_int(1)] = {s.col_int(0), stage, s.col_text(2), s.col_text(3), s.col_int(4)};
        }
        std::optional<Statement> recorded;
        if (evidence_schema) recorded.emplace(db_->prepare(kRecordedInputs));
        std::vector<long long> dropped;
        for (const long long chunk_id : old_ids)
            if (!used.count(chunk_id)) dropped.push_back(chunk_id);
        // Only a passage that holds no findings and shares no extraction request with current findings is retired;
        // any other is deleted now, so that its triggers withdraw those findings in this transaction.
        if (stage)
            if (Status s = each("UPDATE chunks SET item_id=" + std::to_string(stage) + " WHERE id IN ", dropped,
                                " AND NOT EXISTS(SELECT 1 FROM relations r WHERE r.chunk_id=chunks.id)"
                                " AND NOT EXISTS(SELECT 1 FROM mentions m WHERE m.chunk_id=chunks.id)"
                                " AND NOT EXISTS(SELECT 1 FROM knowledge_extraction_inputs d WHERE d.result=1 AND d.batch_id IN"
                                " (SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id=chunks.id))");
                !s)
                return s;
        if (Status s = each("DELETE FROM chunks WHERE item_id=" + item_id + " AND id IN ", dropped); !s) return s;
        std::vector<long long> rechecked, adopted, added;
        long long kept_count = 0, redated_count = 0, rechecked_count = 0;
        int ord = 0;
        for (const auto& c : in.chunks) {
            const long long existing_id = retained[static_cast<size_t>(ord)];
            if (existing_id) {
                // A passage that stays in place with its date is not rewritten.
                if (plan.old_ords.at(existing_id) != ord || old_dates.at(existing_id) != c.at) {
                    update.reset();
                    update.bind(1, ord).bind(2, c.at).bind(3, existing_id);
                    if (Status s = update.run(); !s) return s;
                }
                ++ord;
                const bool again = recheck_retained(recorded ? &*recorded : nullptr, plan, previous, in, existing_id, c.at);
                if (again) rechecked.push_back(existing_id);
                // As plan_item_change reports it: a re-dated passage, else a rechecked one, else kept.
                if (old_dates.at(existing_id) != c.at) ++redated_count;
                else if (again) ++rechecked_count;
                else ++kept_count;
                continue;
            }
            if (const auto ready = staged.find(ord); ready != staged.end() && ready->second.heading == c.heading &&
                                                     ready->second.text == c.text && ready->second.at == c.at) {
                adopted.push_back(ready->second.id);
                ++ord;
                continue;
            }
            ci.reset();
            ci.bind(1, id).bind(2, ord++).bind(3, c.heading).bind(4, c.text).bind(5, c.at).bind(6, estimate_tokens(c.text));
            if (Status s = ci.run(); !s) return s;
            added.push_back(db_->last_insert_id());
        }
        if (Status s = each("UPDATE chunks SET item_id=" + item_id + " WHERE id IN ", adopted); !s) return s;
        added.insert(added.end(), adopted.begin(), adopted.end());
        if (Status s = each("INSERT INTO embed_queue(chunk_id, priority) SELECT id," + priority + " FROM chunks WHERE id IN ", added); !s) return s;
        if (in.extract)
            if (Status s = each("INSERT INTO extract_queue(chunk_id, priority) SELECT id," + priority + " FROM chunks WHERE id IN ", added); !s)
                return s;
        // Kept passages whose findings are extracted again.
        if (evidence_schema)
            if (Status s = each("UPDATE knowledge_extraction_inputs SET result=0,item_date=0 WHERE result=1 AND chunk_id IN ", rechecked); !s)
                return s;
        if (Status s = each("DELETE FROM relations WHERE chunk_id IN ", rechecked); !s) return s;
        if (Status s = each("DELETE FROM mentions WHERE chunk_id IN ", rechecked); !s) return s;
        if (Status s = in.extract ? each("INSERT INTO extract_queue(chunk_id, priority) SELECT id," + priority + " FROM chunks WHERE id IN ",
                                         rechecked, " ON CONFLICT(chunk_id) DO UPDATE SET priority=max(priority, excluded.priority)")
                                  : each("DELETE FROM extract_queue WHERE chunk_id IN ", rechecked);
            !s)
            return s;
        if (stage) {
            // What is left under the stage is retired: nothing may pick it up for embedding or extraction.
            const std::string left = "(SELECT id FROM chunks WHERE item_id=" + std::to_string(stage) + ")";
            if (Status s = db_->exec("DELETE FROM extract_queue WHERE chunk_id IN " + left + "; DELETE FROM embed_queue WHERE chunk_id IN " + left);
                !s)
                return s;
        }
        if (evidence_schema) {
            // Any stage of the item, including one of another version, is retired by a completed change.
            auto discard = db_->prepare("UPDATE items SET source=?1, ext_id=ext_id||'#'||id WHERE source=?2 AND ext_id=?3");
            discard.bind(1, kRetiredSource).bind(2, kStagedSource).bind(3, key);
            if (Status s = discard.run(); !s) return s;
        }
        if (report && (previous->hash != in.hash || !dropped.empty() || !added.empty() || redated_count || rechecked_count)) {
            // The report is written with the revision; its facts are counted right after, outside this transaction
            // (facts=-1 until then), so that a large switch does not hold the writer to count them.
            auto revision = db_->prepare("SELECT sequence FROM knowledge_revisions WHERE item_id=?1 AND current=1");
            revision.bind(1, id);
            const long long sequence = revision.step() ? revision.col_int(0) : 0;
            auto r = db_->prepare("INSERT INTO knowledge_change_reports(item_id,revision,created_at,added,kept,redated,rechecked,removed,facts,"
                                  "unsupported) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,-1,0)");
            r.bind(1, id).bind(2, sequence).bind(3, now).bind(4, static_cast<long long>(added.size())).bind(5, kept_count).bind(6, redated_count);
            r.bind(7, rechecked_count).bind(8, static_cast<long long>(dropped.size()));
            if (Status s = r.run(); !s) return s;
            report_id = db_->last_insert_id();
            // Retention: the newest 500 reports, none older than 90 days.
            auto prune = db_->prepare("DELETE FROM knowledge_change_reports WHERE created_at<?1 OR id<=(SELECT id FROM knowledge_change_reports "
                                      "ORDER BY id DESC LIMIT 1 OFFSET 500)");
            prune.bind(1, now - 90LL * 24 * 60 * 60 * 1000);
            if (Status s = prune.run(); !s) return s;
        } else before.clear();
        auto sk = db_->prepare("DELETE FROM skipped WHERE source=?1 AND ext_id=?2");
        sk.bind(1, in.source).bind(2, in.ext_id);
        return sk.run();
    });
    if (!st) return st.error();
    report_before_.erase(report_key);
    delete_orphan_entities(*db_);
    if (report_id) {
        // Other readers and writers go first; then the facts that lost support in the write, those left without any
        // first, at most 1,000 listed. A write in that moment could still lower a count, and the report would then
        // include it.
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
        std::vector<ChangeReportFact> lost;
        long long unsupported = 0;
        auto count = db_->prepare("SELECT count(*) FROM knowledge_support_sets WHERE fact_id=?1 AND state IN ('current','unverified')");
        for (const auto& [fact, n] : before) {
            count.reset();
            count.bind(1, fact);
            const long long now_count = count.step() ? count.col_int(0) : 0;
            if (now_count >= n) continue;
            lost.push_back({fact, n, now_count});
            if (!now_count) ++unsupported;
        }
        std::sort(lost.begin(), lost.end(), [](const ChangeReportFact& a, const ChangeReportFact& b) {
            return std::pair{a.after > 0, a.fact_id} < std::pair{b.after > 0, b.fact_id};
        });
        const auto counted = db_->transaction([&]() -> Status {
            auto entry = db_->prepare("INSERT INTO knowledge_change_report_facts(report_id,fact_id,before,after) "
                                      "SELECT ?1,?2,?3,?4 WHERE EXISTS(SELECT 1 FROM knowledge_change_reports WHERE id=?1)");
            for (size_t k = 0; k < std::min<size_t>(lost.size(), 1000); ++k) {
                entry.reset();
                entry.bind(1, report_id).bind(2, lost[k].fact_id).bind(3, lost[k].before).bind(4, lost[k].after);
                if (Status s = entry.run(); !s) return s;
            }
            auto done = db_->prepare("UPDATE knowledge_change_reports SET facts=?2,unsupported=?3 WHERE id=?1");
            done.bind(1, report_id).bind(2, static_cast<long long>(lost.size())).bind(3, unsupported);
            return done.run();
        });
        (void)counted;  // if not, the report stays marked as not counted (facts=-1)
    }
    return id;
}

Status KgStore::purge_recovery_copy() {
    const std::lock_guard lock(db_->mutex());
    // Serialize even standalone callers with a concurrent migration snapshot. Callers that delete knowledge
    // must also hold this writer reservation until their deletion commits.
    if (sqlite3_get_autocommit(db_->raw()))
        return db_->transaction([&] { return purge_recovery_copy(); });
    auto backup = db_->path();
    backup += ".bak";
    auto staging = db_->path();
    staging += ".bak.staging";
    for (const auto& path : {backup, std::filesystem::path(backup.wstring() + L"-wal"),
                            std::filesystem::path(backup.wstring() + L"-shm"), staging,
                            std::filesystem::path(staging.wstring() + L"-wal"), std::filesystem::path(staging.wstring() + L"-shm")}) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) return make_error("storage", "Cannot remove the knowledge recovery copy: " + ec.message());
    }
    return {};
}

Status KgStore::forget_thread(const std::string& thread_id) {
    const std::lock_guard lock(db_->mutex());
    const auto result = db_->transaction([&]() -> Status {
        if (auto s = purge_recovery_copy(); !s) return s;
        auto exclusion = db_->prepare("INSERT OR IGNORE INTO user_exclusions(kind,value) VALUES('item',?1)");
        exclusion.bind(1, "threads:" + thread_id);
        if (auto s = exclusion.run(); !s) return s;
        auto facts = db_->prepare("DELETE FROM facts_user WHERE thread_id=?1 OR (item_source='threads' AND item_ext_id=?1)");
        facts.bind(1, thread_id);
        if (auto s = facts.run(); !s) return s;
        auto tombstones = db_->prepare("DELETE FROM fact_tombstones WHERE source='threads' AND ext_id=?1");
        tombstones.bind(1, thread_id);
        if (auto s = tombstones.run(); !s) return s;
        auto item = db_->prepare("DELETE FROM items WHERE source='threads' AND ext_id=?1");
        item.bind(1, thread_id);
        if (auto s = item.run(); !s) return s;
        auto skipped = db_->prepare("DELETE FROM skipped WHERE source='threads' AND ext_id=?1");
        skipped.bind(1, thread_id);
        if (auto s = skipped.run(); !s) return s;
        return delete_orphan_entities(*db_);
    });
    if (!result) return result;
    return {};
}

bool KgStore::remove_item(const std::string& source, const std::string& ext_id, bool forget) {
    // Optional, as for a revision: the transaction below removes everything that is left.
    (void)prepare_item_removal_steps(source, ext_id);
    std::unique_lock lock(db_->mutex());
    bool removed = false;
    const auto result = db_->transaction([&]() -> Status {
        if (auto s = purge_recovery_copy(); !s) return s;
        if (forget)
            if (auto s = queue_item_withdrawals(*db_, source, ext_id); !s) return s;
        auto facts = db_->prepare("DELETE FROM facts_user WHERE (item_source=?1 AND item_ext_id=?2) OR (?1='threads' AND thread_id=?2)");
        facts.bind(1, source).bind(2, ext_id);
        if (auto s = facts.run(); !s) return s;
        if (forget) {
            auto tombstones = db_->prepare("DELETE FROM fact_tombstones WHERE source=?1 AND ext_id=?2");
            tombstones.bind(1, source).bind(2, ext_id);
            if (auto s = tombstones.run(); !s) return s;
        }
        if (db_->user_version() >= kKgEvidenceSchemaVersion)
            if (auto s = retire_removed_passages(*db_, source, ext_id); !s) return s;
        auto s = db_->prepare("DELETE FROM items WHERE source=?1 AND ext_id=?2");
        s.bind(1, source).bind(2, ext_id);
        if (auto status = s.run(); !status) return status;
        removed = db_->changes() > 0;
        auto k = db_->prepare("DELETE FROM skipped WHERE source=?1 AND ext_id=?2");
        k.bind(1, source).bind(2, ext_id);
        if (auto status = k.run(); !status) return status;
        return removed ? delete_orphan_entities(*db_) : Status{};
    });
    if (!result) return false;
    lock.unlock();
    // The retired passages hold the forgotten text: delete them now rather than with a later change.
    (void)finish_item_changes();
    return removed;
}

bool KgStore::remove_item(long long id, bool forget) {
    // Not locked across both calls, so that a large removal can release the lock between its steps.
    const auto existing = item(id);
    return existing && remove_item(existing->source, existing->ext_id, forget);
}

std::vector<std::string> KgStore::item_ids(const std::string& source) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT ext_id FROM items WHERE source=?1");
    s.bind(1, source);
    std::vector<std::string> out;
    while (s.step()) out.push_back(s.col_text(0));
    return out;
}

int KgStore::remove_source(const std::string& kind, bool forget) {
    std::unique_lock lock(db_->mutex());
    int n = 0;
    const auto result = db_->transaction([&]() -> Status {
        if (auto s = purge_recovery_copy(); !s) return s;
        if (db_->user_version() >= kKgEvidenceSchemaVersion) {
            // Staged passages of the Source's items go with them.
            auto discard = db_->prepare("UPDATE items SET source=?1, ext_id=ext_id||'#'||id WHERE source=?2 AND substr(ext_id,1,length(?3)+1)=?3||':'");
            discard.bind(1, kRetiredSource).bind(2, kStagedSource).bind(3, kind);
            if (auto s = discard.run(); !s) return s;
        }
        if (forget) {
            if (auto s = queue_item_withdrawals(*db_, kind, std::nullopt); !s) return s;
            if (kind == "files")
                if (auto s = db_->exec("DELETE FROM share_rules"); !s) return s;
            auto disable = db_->prepare("INSERT INTO sources(kind,user_enabled) VALUES(?1,0) "
                                        "ON CONFLICT(kind) DO UPDATE SET user_enabled=0");
            disable.bind(1, kind);
            if (auto s = disable.run(); !s) return s;
        }
        auto facts = db_->prepare("DELETE FROM facts_user WHERE item_source=?1 OR "
                                  "(?1='threads' AND thread_id IN (SELECT ext_id FROM items WHERE source='threads'))");
        facts.bind(1, kind);
        if (auto s = facts.run(); !s) return s;
        auto s = db_->prepare("DELETE FROM items WHERE source=?1");
        s.bind(1, kind);
        if (auto status = s.run(); !status) return status;
        n = db_->changes();
        if (forget && kind == "notes")
            if (auto status = db_->exec("DELETE FROM notes"); !status) return status;
        if (forget) {
            auto tombstones = db_->prepare("DELETE FROM fact_tombstones WHERE source=?1");
            tombstones.bind(1, kind);
            if (auto status = tombstones.run(); !status) return status;
        }
        auto k = db_->prepare("DELETE FROM skipped WHERE source=?1");
        k.bind(1, kind);
        if (auto status = k.run(); !status) return status;
        return delete_orphan_entities(*db_);
    });
    if (!result) return -1;
    lock.unlock();
    (void)finish_item_changes();
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

std::vector<ItemRow> KgStore::find_items(const std::string& query, int limit,
                                         std::optional<std::pair<long long, long long>> before) {
    std::vector<ItemRow> out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns +
                          " FROM items WHERE (instr(lower(title), lower(?1)) > 0 OR instr(lower(ref), lower(?1)) > 0)"
                          " AND source NOT IN (?5,?6)"
                          " AND NOT EXISTS (SELECT 1 FROM user_exclusions u WHERE u.kind='item' AND u.value=items.source||':'||items.ext_id)"
                          " AND (?3=0 OR updated_at<?4 OR (updated_at=?4 AND id<?3))"
                          " ORDER BY updated_at DESC, id DESC LIMIT ?2");
    s.bind(1, query).bind(2, static_cast<long long>(limit)).bind(3, before ? before->second : 0).bind(4, before ? before->first : 0);
    s.bind(5, kStagedSource).bind(6, kRetiredSource);
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

std::vector<ItemQueueCounts> KgStore::item_queue_counts(const std::string& source) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT i.ext_id, count(c.id), count(e.chunk_id), count(x.chunk_id) FROM items i "
                          "LEFT JOIN chunks c ON c.item_id=i.id "
                          "LEFT JOIN embed_queue e ON e.chunk_id=c.id LEFT JOIN extract_queue x ON x.chunk_id=c.id "
                          "WHERE i.source=?1 GROUP BY i.id");
    s.bind(1, source);
    std::vector<ItemQueueCounts> out;
    while (s.step()) out.push_back({s.col_text(0), s.col_int(1), s.col_int(2), s.col_int(3)});
    return out;
}

long long KgStore::skipped_count(const std::string& source) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT count(*) FROM skipped WHERE source=?1");
    s.bind(1, source);
    return s.step() ? s.col_int(0) : 0;
}

std::vector<BrowseRow> KgStore::browse(const std::string& source, const std::string& query, int limit, bool extraction,
                                       std::optional<std::pair<long long, long long>> before) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns +
                          ", (SELECT ifnull(sum(length(c.text)), 0) FROM chunks c WHERE c.item_id=items.id)"
                          ", (SELECT count(*) FROM chunks c WHERE c.item_id=items.id AND (c.id IN (SELECT chunk_id FROM embed_queue)"
                          " OR (?4 AND c.id IN (SELECT chunk_id FROM extract_queue))))"
                          " FROM items WHERE source=?1 AND (?2='' OR instr(lower(title), lower(?2)) > 0 OR instr(lower(ref), lower(?2)) > 0)"
                          " AND NOT EXISTS (SELECT 1 FROM user_exclusions u WHERE u.kind='item' AND u.value=items.source||':'||items.ext_id)"
                          " AND (?5=0 OR updated_at<?6 OR (updated_at=?6 AND id<?5))"
                          " ORDER BY updated_at DESC, id DESC LIMIT ?3");
    s.bind(1, source).bind(2, query).bind(3, static_cast<long long>(limit)).bind(4, extraction ? 1 : 0)
        .bind(5, before ? before->second : 0).bind(6, before ? before->first : 0);
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

long long KgStore::item_extraction_pending(long long item_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT count(*) FROM chunks c JOIN extract_queue q ON q.chunk_id=c.id WHERE c.item_id=?1");
    s.bind(1, item_id);
    return s.step() ? s.col_int(0) : 0;
}

// ---- Full text ----------------------------------------------------------------------------------------------------------

std::vector<long long> KgStore::fts(const std::string& query, int limit) {
    std::vector<long long> out;
    const std::string q = fts_query(query);
    if (q.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    // Staged and retired passages are indexed but belong to no Source.
    auto s = db_->prepare("SELECT f.rowid FROM chunks_fts f JOIN chunks c ON c.id=f.rowid JOIN items i ON i.id=c.item_id "
                          "WHERE chunks_fts MATCH ?1 AND i.source NOT IN (?3,?4) ORDER BY f.rank LIMIT ?2");
    s.bind(1, q).bind(2, limit).bind(3, kStagedSource).bind(4, kRetiredSource);
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
        "INSERT OR IGNORE INTO embed_queue(chunk_id, priority) SELECT c.id, 0 FROM chunks c JOIN items i ON i.id=c.item_id "
        "WHERE i.source NOT IN (?3,?4) AND NOT EXISTS "
        "(SELECT 1 FROM vec_bin v JOIN vec_models m ON m.id=v.model_id WHERE v.chunk_id=c.id AND m.model=?1 AND m.dim=?2)");
    s.bind(1, model).bind(2, dim).bind(3, kStagedSource).bind(4, kRetiredSource);
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

std::vector<ChunkRow> KgStore::extract_batch(int n, const std::vector<long long>& deferred) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string(
        "SELECT c.id, c.item_id, c.heading, c.text, c.at FROM extract_queue q JOIN chunks c ON c.id=q.chunk_id "
        ) + (deferred.empty() ? "" : "WHERE q.chunk_id NOT IN (" + id_list(deferred) + ") ") +
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

Status KgStore::record_extraction(const std::vector<long long>& chunk_ids, const std::vector<long long>& item_dated) {
    const std::lock_guard lock(db_->mutex());
    if (chunk_ids.empty() || db_->user_version() < kKgEvidenceSchemaVersion) return {};
    const auto write = [&]() -> Status {
        const std::string ids = id_list(chunk_ids);
        long long batch = 0;
        {
            auto next = db_->prepare("SELECT coalesce(max(batch_id),0)+1 FROM knowledge_extraction_inputs");
            if (!next.step()) return make_error("storage", "The extraction inputs could not be recorded.");
            batch = next.col_int(0);
        }
        if (Status s = db_->exec("UPDATE knowledge_extraction_inputs SET result=0,item_date=0 WHERE result=1 AND chunk_id IN (" + ids + ")"); !s)
            return s;
        for (const long long id : chunk_ids) {
            auto add = db_->prepare("INSERT INTO knowledge_extraction_inputs(batch_id,chunk_id,result,item_date) "
                                    "SELECT ?1,id,1,?3 FROM chunks WHERE id=?2");
            add.bind(1, batch).bind(2, id);
            add.bind(3, std::find(item_dated.begin(), item_dated.end(), id) != item_dated.end() ? 1 : 0);
            if (Status s = add.run(); !s) return s;
        }
        // An earlier request none of whose passages still holds its findings no longer explains anything.
        return db_->exec("DELETE FROM knowledge_extraction_inputs WHERE batch_id<>" + std::to_string(batch) +
                         " AND batch_id IN (SELECT batch_id FROM knowledge_extraction_inputs WHERE chunk_id IN (" + ids + "))"
                         " AND NOT EXISTS(SELECT 1 FROM knowledge_extraction_inputs r"
                         " WHERE r.batch_id=knowledge_extraction_inputs.batch_id AND r.result=1)");
    };
    return sqlite3_get_autocommit(db_->raw()) ? db_->transaction(write) : write();
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
    std::vector<EntityRow> out;
    for (auto& row : find_entities_page(query, limit)) out.push_back(std::move(row.entity));
    return out;
}

std::vector<EntitySearchRow> KgStore::find_entities_page(const std::string& query, int limit,
                                                          const std::optional<EntityCursor>& after) {
    std::vector<EntitySearchRow> out;
    if (limit <= 0) return out;
    const std::lock_guard lock(db_->mutex());
    const std::string norm = normalize_name(query);
    // Stable ordering also lets callers skip inaccessible hits without losing later matches.
    auto s = db_->prepare(std::string("WITH scored AS (SELECT ") + kEntityColumns +
                          ", CASE WHEN ?1='' THEN 0 ELSE COALESCE((SELECT max(CASE WHEN a.norm=?1 THEN 2 "
                          "WHEN a.norm LIKE ?2 THEN 1 ELSE 0 END) FROM entity_aliases a WHERE a.entity_id=entities.id),0) END AS score "
                          "FROM entities) SELECT " + kEntityColumns + ", score FROM scored "
                          "WHERE (?1='' OR score>0 OR email=?3) AND (?5=0 OR score<?6 OR "
                          "(score=?6 AND (name>?7 OR (name=?7 AND id>?5)))) ORDER BY score DESC, name, id LIMIT ?4");
    s.bind(1, norm).bind(2, "%" + norm + "%").bind(3, to_lower(trim(query))).bind(4, limit)
        .bind(5, after ? after->id : 0LL).bind(6, after ? after->score : 0LL).bind(7, after ? after->name : std::string{});
    while (s.step()) out.push_back({read_entity(s), s.col_int(8)});
    return out;
}

bool KgStore::entity_has_directory_relation(long long entity_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("SELECT 1 FROM relations WHERE (subject=?1 OR object=?1) AND chunk_id IS NULL LIMIT 1");
    s.bind(1, entity_id);
    return s.step();
}

std::vector<ItemRow> KgStore::entity_evidence_items(long long entity_id, long long after_item_id, int limit) {
    std::vector<ItemRow> out;
    if (limit <= 0) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns + " FROM items WHERE id>?2 AND id IN ("
                          "SELECT c.item_id FROM mentions m JOIN chunks c ON c.id=m.chunk_id WHERE m.entity_id=?1 "
                          "UNION SELECT c.item_id FROM relations r JOIN chunks c ON c.id=r.chunk_id "
                          "WHERE r.subject=?1 OR r.object=?1) "
                          "AND NOT EXISTS (SELECT 1 FROM user_exclusions u WHERE u.kind='item' AND u.value=items.source||':'||items.ext_id) "
                          "ORDER BY id LIMIT ?3");
    s.bind(1, entity_id).bind(2, after_item_id).bind(3, limit);
    while (s.step()) out.push_back(read_item(s));
    return out;
}

std::vector<ItemRow> KgStore::entity_alias_items(long long entity_id, const std::string& normalized_alias, long long after_item_id, int limit) {
    std::vector<ItemRow> out;
    if (limit <= 0 || normalized_alias.empty()) return out;
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("SELECT ") + kItemColumns + " FROM items WHERE id>?3 AND id IN ("
                          "SELECT c.item_id FROM mentions m JOIN chunks c ON c.id=m.chunk_id "
                          "WHERE m.entity_id=?1 AND m.alias=?2) "
                          "AND NOT EXISTS (SELECT 1 FROM user_exclusions u WHERE u.kind='item' AND u.value=items.source||':'||items.ext_id) "
                          "ORDER BY id LIMIT ?4");
    s.bind(1, entity_id).bind(2, normalized_alias).bind(3, after_item_id).bind(4, limit);
    while (s.step()) out.push_back(read_item(s));
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
    long long id = 0;
    const auto write = [&]() -> Status {
        auto s = db_->prepare(
            "INSERT INTO relations(subject, predicate, object, confidence, valid_from, valid_to, chunk_id) VALUES(?1,?2,?3,?4,?5,?6,?7)");
        s.bind(1, subject).bind(2, predicate).bind(3, object).bind(4, confidence).bind(5, valid_from).bind(6, valid_to);
        if (chunk_id) s.bind(7, chunk_id);
        else s.bind_null(7);
        if (Status st = s.run(); !st) return st;
        id = db_->last_insert_id();
        // A relation without a passage (directory enrichment) has no item evidence to record.
        if (!chunk_id || db_->user_version() < kKgEvidenceSchemaVersion) return {};
        return record_relation_support(*db_, id, subject, predicate, object, valid_from, valid_to, chunk_id);
    };
    // The relation and its evidence are one write; join the caller's transaction when there is one.
    if (sqlite3_get_autocommit(db_->raw())) return db_->transaction(write) ? id : 0;
    if (write()) return id;
    if (id) {
        auto undo = db_->prepare("DELETE FROM relations WHERE id=?1");
        undo.bind(1, id);
        (void)undo.run();
    }
    return 0;
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
    return db_->transaction([&]() -> Status {
        if (auto status = purge_recovery_copy(); !status) return status;
        auto s = db_->prepare("DELETE FROM entities WHERE id=?1");
        s.bind(1, id);
        if (Status st = s.run(); !st) return st;
        if (db_->changes() == 0) return make_error("not_found", "no such entity");
        return {};
    });
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
    const long long id = db_->last_insert_id();
    if (db_->user_version() >= kKgEvidenceSchemaVersion) {
        // The same link the migration backfills: the stated item, else the Thread the fact was stated in, else none.
        auto note = db_->prepare(
            "INSERT INTO knowledge_annotations(user_fact_id,revision_id) VALUES(?1,(SELECT v.id FROM items i "
            "JOIN knowledge_revisions v ON v.item_id=i.id AND v.current=1 WHERE (?2<>'' AND i.source=?2 AND i.ext_id=?3) "
            "OR (?2='' AND ?4<>'' AND i.source='threads' AND i.ext_id=?4)))");
        note.bind(1, id).bind(2, item_source).bind(3, item_ext_id).bind(4, thread_id);
        if (!note.run()) {
            auto undo = db_->prepare("DELETE FROM facts_user WHERE id=?1");
            undo.bind(1, id);
            (void)undo.run();
            return 0;
        }
    }
    return id;
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

Status KgStore::repair_ambiguous_shares() {
    const std::lock_guard lock(db_->mutex());
    return db_->exec("UPDATE shares SET state='unverified',error='The previous sharing state cannot be verified.' "
                     "WHERE (state='removed' AND error='Removed from team knowledge by an administrator.') "
                     "OR (state='withdrawn' AND error<>'acknowledged-delete-v1')");
}

Status KgStore::mark_shares_unsharing() {
    const std::lock_guard lock(db_->mutex());
    if (auto s = repair_ambiguous_shares(); !s) return s;
    return db_->exec("UPDATE shares SET state='unsharing',error='Waiting for verified removal.' WHERE state NOT IN ('removed','withdrawn')");
}

Status KgStore::mark_share_removed(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("UPDATE shares SET state='removed',error='' WHERE source=?1 AND ext_id=?2 AND state='unsharing'");
    s.bind(1, source).bind(2, ext_id);
    if (auto result = s.run(); !result) return result;
    if (db_->changes() != 1) return make_error("stale", "The shared item changed during removal.");
    return {};
}

Status KgStore::put_share(const ShareRow& r) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare(std::string("INSERT OR REPLACE INTO shares(") + kShareColumns + ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
    s.bind(1, r.source).bind(2, r.ext_id).bind(3, r.remote_id).bind(4, r.title).bind(5, json(r.groups).dump()).bind(6, r.item_hash);
    s.bind(7, r.content_hash).bind(8, r.size).bind(9, r.state).bind(10, r.error).bind(11, r.rule).bind(12, r.updated_at);
    return s.run();
}

Status KgStore::remove_share(const std::string& source, const std::string& ext_id) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM shares WHERE source=?1 AND ext_id=?2");
    s.bind(1, source).bind(2, ext_id);
    return s.run();
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

Status KgStore::remove_share_rule(const std::string& folder) {
    const std::lock_guard lock(db_->mutex());
    auto s = db_->prepare("DELETE FROM share_rules WHERE folder=?1");
    s.bind(1, folder);
    return s.run();
}

Status KgStore::clear(bool all) {
    const std::lock_guard lock(db_->mutex());
    // The day's budgets stay: forgetting must not let backfill spend the budget a second time.
    return db_->transaction([&]() -> Status {
        if (auto s = purge_recovery_copy(); !s) return s;
        if (auto s = db_->exec("DELETE FROM items; DELETE FROM entities; DELETE FROM skipped; DELETE FROM meta; DELETE FROM fact_tombstones;"); !s)
            return s;
        if (all && db_->user_version() >= kKgEvidenceSchemaVersion)
            if (auto s = db_->exec("DELETE FROM knowledge_categories;"); !s) return s;
        if (all)
            return db_->exec("DELETE FROM sources; DELETE FROM folders; DELETE FROM user_exclusions; DELETE FROM documents; DELETE FROM notes; "
                             "DELETE FROM shares; DELETE FROM share_rules;");
        return {};
    });
}

}  // namespace wb::knowledge
