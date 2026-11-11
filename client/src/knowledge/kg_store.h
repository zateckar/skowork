// kg.db, the knowledge store in the data root (client/knowledge-graph Local-Only Knowledge Store; add-knowledge-graph
// design D1): Sources, items, chunks with an FTS5 index, binary and int8 vectors, the embedding and extraction queues,
// entities with aliases, relations, mentions and user-stated facts, and the daily budgets. One encrypted file with its
// own key; every derived row hangs off its item or entity, so deleting those cascades. add-knowledge-sharing adds single
// documents, notes, fact tombstones, and the shared items with their share rules.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "knowledge/analysis.h"
#include "knowledge/core.h"
#include "knowledge/reconcile.h"
#include "storage/database.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wb::knowledge {

const std::vector<Migration>& kg_migrations();

// Register the connection-local capability before a knowledge migration runs. The guard SQL is installed only
// alongside the versioned evidence schema; until then it can be exercised on an isolated copied database.
Status register_kg_writer(Database& db);
std::string kg_writer_barrier_sql();
// Versioned evidence migration, currently qualified against copied fixtures before activation.
std::string kg_evidence_migration_sql();
// The schema version the evidence migration installs, one past the last production migration (design D6).
inline constexpr int kKgEvidenceSchemaVersion = 6;
// The longest chain of facts a derivation may form through required facts; the evidence schema enforces it.
inline constexpr int kMaxDerivationDepth = 32;
// Superseded revisions of an item whose excerpts are kept as history beside the current one.
inline constexpr int kRetainedSupersededRevisions = 4;
// Hidden items: the new passages of a large change before it is switched to current, and passages a switch or
// forget replaced, until they are deleted in steps. Neither is a Source; readers exclude them.
inline constexpr const char* kStagedSource = "~staged";
inline constexpr const char* kRetiredSource = "~retired";
Result<std::unique_ptr<Database>> open_kg_database(const std::filesystem::path& file, const db_key& key, MigrationOutcome& outcome);

// The Source kinds (client/knowledge-graph Source Enablement).
const std::vector<std::string>& source_kinds();

struct SourceState {
    std::string kind;
    bool user_enabled = true;
    bool paused = false;
    long long last_update = 0;  // unix ms
    std::string last_error;
};

struct CategoryRow {
    long long id = 0;
    std::string name;
};

struct CategoryAssignment {
    long long fact_id = 0, category_id = 0, evidence_id = 0;
    std::string decision, reason;
};

struct ConflictRow {
    long long id = 0, left_fact = 0, right_fact = 0, left_evidence = 0, right_evidence = 0;
    std::string explanation, state, decision, decision_reason;
};

struct ConflictDecision {
    long long left_evidence = 0, right_evidence = 0, decided_at = 0;
    std::string decision, reason;
};

struct KnowledgeImpactPreview {
    long long generation = 0;
    bool complete = false;  // Extraction, conflict checks and publication differences are not included yet.
    std::vector<FactImpact> facts;
};

struct PreparedItemRemoval {
    std::string token;
    KnowledgeImpactPreview impact;
};

struct ItemInput {
    std::string source, ext_id, title, ref, hash;
    long long created_at = 0, updated_at = 0;  // unix ms
    int priority = 1;                          // 2 new or changed, 1 incremental, 0 backfill (embedding and extraction order)
    std::vector<Chunk> chunks;
    bool extract = true;  // queue the chunks for entity extraction
    std::string legacy_tombstone_hash = {};  // conservatively carry deleted-fact protections across fingerprint upgrade
};

struct ItemRow {
    long long id = 0;
    std::string source, ext_id, title, ref;
    long long created_at = 0, updated_at = 0, indexed_at = 0;
    std::string hash;  // the item version; older file items can still contain size and modification time
};

// An item of a Source as the Knowledge view lists it: indexed text size and whether embedding or extraction is pending.
struct BrowseRow {
    ItemRow item;
    long long size = 0, pending = 0;
};

struct ItemQueueCounts {
    std::string ext_id;
    long long chunks = 0, embed_pending = 0, extract_pending = 0;
};

// A note of the notes Source (add-knowledge-sharing Knowledge Notes).
struct NoteRow {
    long long id = 0;
    std::string title, text;
    long long created_at = 0, updated_at = 0;
    // Saved from mail or a Chat (add-save-as-note design D1): kind mail | chat, the mail item's or Chat's id, and its
    // subject or name when it was saved. Empty for a note written by hand.
    std::string origin_kind, origin_ref, origin_title;
};

// A shared item (add-knowledge-sharing design D3): the local item, its id in control-plane/team-knowledge, the Groups,
// the item version and content hash last published, and the share rule (a folder) it came from.
struct ShareRow {
    std::string source, ext_id, remote_id, title;
    std::vector<std::string> groups;
    std::string item_hash, content_hash;
    long long size = 0;
    std::string state;  // shared | error | unsharing | unverified | withdrawn | removed
    std::string error, rule;
    long long updated_at = 0;
};

struct ChunkRow {
    long long id = 0, item_id = 0;
    std::string heading, text;
    long long at = 0;
};

struct EntityRow {
    long long id = 0;
    std::string type, name, email, department, title, manager;
    long long enriched_at = 0;
    std::vector<std::string> aliases;
};

struct EntityCursor {
    long long score = 0;
    std::string name;
    long long id = 0;
};
struct EntitySearchRow {
    EntityRow entity;
    long long score = 0;
};

struct RelationRow {
    long long id = 0, subject = 0, object = 0, chunk_id = 0;
    std::string predicate, valid_from, valid_to;
    double confidence = 0;
    std::string subject_name, object_name;
};

struct FactRow {
    long long id = 0, entity_id = 0;
    std::string text, thread_id;
    long long created_at = 0;
    std::string item_source, item_ext_id;  // a fact the user corrected keeps its item as provenance
};

struct SourceCounts {
    long long items = 0, chunks = 0, embed_pending = 0, extract_pending = 0, skipped = 0;
};

struct BoundAnalysisEvidence {
    long long evidence_id = 0, generation = 0;
};

class KgStore {
public:
    explicit KgStore(std::unique_ptr<Database> db) : db_(std::move(db)) {}
    [[nodiscard]] Database& db() { return *db_; }

    // Category records organize local facts; they do not change existing publications. Available after evidence migration.
    Result<long long> create_category(const std::string& name);
    Status rename_category(long long category_id, const std::string& name);
    Status delete_category(long long category_id);
    Status merge_categories(long long source_id, long long destination_id);
    Status set_category_assignment(long long fact_id, long long category_id, const std::string& decision,
                                   const std::string& reason, long long evidence_id = 0,
                                   std::optional<long long> expected_generation = std::nullopt);
    Status reset_category_assignment(long long fact_id, long long category_id);
    std::vector<CategoryRow> categories();
    std::vector<CategoryAssignment> category_assignments(long long fact_id);
    std::vector<long long> uncategorized_facts(int limit);
    std::optional<long long> knowledge_generation();
    // Bind a validated analysis span to an existing support in the same graph snapshot.
    Result<BoundAnalysisEvidence> bind_analysis_evidence(long long fact_id, const AnalysisSpan& span,
                                                         long long expected_generation);
    Result<long long> record_conflict(long long left_fact, long long right_fact, long long left_evidence,
                                      long long right_evidence, const std::string& explanation, long long expected_generation);
    // Saves one analysis's validated proposals in a single transaction, or none of them: the graph generation must
    // still be the one the analysis read, `still_permitted` (the caller's current policy for the analyzed Sources and
    // the analysis kind) is asked under the store lock, and every excerpt is bound to a current support in the current
    // revision. Automatic categories never replace manual or rejected decisions.
    Status apply_analysis(const AnalysisProposals& proposals, long long expected_generation,
                          const std::function<bool()>& still_permitted = {});
    Status decide_conflict(long long conflict_id, const std::string& decision, const std::string& reason,
                           long long now, long long expected_generation);
    Status reopen_conflict(long long conflict_id, long long expected_generation);
    std::vector<ConflictRow> conflicts(int limit);
    std::vector<ConflictDecision> conflict_history(long long conflict_id);
    Result<KnowledgeImpactPreview> preview_evidence_removal(const std::unordered_set<long long>& evidence_ids);
    Result<KnowledgeImpactPreview> preview_item_removal(long long item_id);
    Result<PreparedItemRemoval> prepare_item_removal(long long item_id, long long now);
    Status cancel_item_removal(const std::string& token);
    Status confirm_item_removal(const std::string& token, bool accept_incomplete, long long now);

    // ---- Sources, folders, exclusions ----
    SourceState source(const std::string& kind);
    void set_source(const SourceState& s);
    Status set_file_source_with_recheck(const SourceState& s, bool changed);
    Status clear_file_recheck(const std::string& token);
    void touch_source(const std::string& kind, long long at, const std::string& error);
    std::vector<std::string> folders();
    Status add_folder(const std::string& path);
    Status remove_folder(const std::string& path);
    // kind: path (a glob), type (a file extension) or item (source:ext_id).
    std::vector<std::pair<std::string, std::string>> user_exclusions();
    Status add_user_exclusion(const std::string& kind, const std::string& value);
    Status remove_user_exclusion(const std::string& kind, const std::string& value);
    void set_skipped(const std::string& source, const std::string& ext_id, const std::string& reason);
    void clear_skipped(const std::string& source, const std::string& ext_id);
    std::vector<std::pair<std::string, std::string>> skipped(const std::string& source, int limit);
    long long skipped_count(const std::string& source);
    // Single documents of the files Source (File Ingestion Scope), by path.
    std::vector<std::string> documents();
    Status add_document(const std::string& path);
    Status remove_document(const std::string& path);

    // ---- Notes ----
    // Inserts (id 0) or updates a note; returns its id, 0 when there is no such note.
    // A new note (id 0) takes the origin of `origin`; saving an existing note keeps the one it has.
    long long save_note(long long id, const std::string& title, const std::string& text, long long now, const NoteRow& origin = {});
    Status delete_note(long long id);
    std::optional<NoteRow> note(long long id);
    std::vector<NoteRow> notes();
    std::vector<long long> note_ids();
    std::vector<NoteRow> notes_page(int limit, std::optional<std::pair<long long, long long>> before = std::nullopt);

    // ---- Items and chunks ----
    // The stored content hash of an item, or nullopt when it is not indexed.
    std::optional<std::string> item_hash(const std::string& source, const std::string& ext_id);
    // Inserts or replaces the item and its chunks; new chunks are queued for embedding (and for extraction).
    Result<long long> put_item(const ItemInput& in, long long now);
    // put_item without its prepare_item_change, for a caller that called that itself before taking the lock.
    Result<long long> put_prepared_item(const ItemInput& in, long long now);
    // With the evidence schema, a large change first withdraws the findings it will invalidate and stages the new
    // passages in a hidden item, both in short transactions; put_item then only switches the staged passages over.
    // put_item does this itself, but a caller holding the database lock around put_item should call it before taking
    // the lock so that other writers can run between the steps, and call finish_item_changes() after releasing it.
    Status prepare_item_change(const ItemInput& in);
    // Deletes, in short transactions, passages that a switch, discard or forget retired.
    Status finish_item_changes();
    // Discards the staged passages of an item whose change will not be written (not eligible, cancelled).
    Status discard_item_change(const std::string& source, const std::string& ext_id);
    // A migration backup can contain forgotten knowledge. Failure must prevent acknowledging the forget operation.
    Status purge_recovery_copy();
    Status forget_thread(const std::string& thread_id);
    bool remove_item(const std::string& source, const std::string& ext_id, bool forget = false);
    bool remove_item(long long id, bool forget = false);
    std::vector<std::string> item_ids(const std::string& source);  // external ids
    int remove_source(const std::string& kind, bool forget = false);  // every item; explicit forget also disables the Source
    std::optional<ItemRow> item(long long id);
    std::optional<ItemRow> item(const std::string& source, const std::string& ext_id);
    // Items whose title or reference contains `query` (case-insensitive for ASCII), newest first.
    std::vector<ItemRow> find_items(const std::string& query, int limit,
                                    std::optional<std::pair<long long, long long>> before = std::nullopt);
    std::vector<ChunkRow> chunks(const std::vector<long long>& ids);
    std::vector<ChunkRow> chunks_of_item(long long item_id);
    SourceCounts counts(const std::string& source);
    std::vector<ItemQueueCounts> item_queue_counts(const std::string& source);
    // Items of a Source whose title or reference contains `query` (all when empty), newest first. Pending counts
    // chunks waiting for embedding, and for extraction when `extraction` is on.
    std::vector<BrowseRow> browse(const std::string& source, const std::string& query, int limit, bool extraction,
                                  std::optional<std::pair<long long, long long>> before = std::nullopt);  // updated_at, id
    long long item_pending(long long item_id, bool extraction);
    long long item_extraction_pending(long long item_id);

    // ---- Full text ----
    // BM25 order; the query is reduced to its words (any word matches).
    std::vector<long long> fts(const std::string& query, int limit);

    // ---- Embeddings ----
    // The next chunks waiting for embedding: highest priority first, then newest.
    std::vector<ChunkRow> embed_batch(int n);
    long long embed_pending();
    void store_vectors(const std::vector<std::pair<long long, std::vector<float>>>& vectors, const std::string& model, int dim);
    // Chunks are dequeued without a vector (the text is empty or the gateway refused it as invalid).
    void drop_from_embed_queue(const std::vector<long long>& ids);
    // The model and dimension semantic search uses (the latest completed embedding run); empty when none.
    std::pair<std::string, int> active_embedding();
    void set_active_embedding(const std::string& model, int dim);
    // Re-embedding after a model or dimension change: every chunk without a vector of (model, dim) is queued at backfill
    // priority. Returns how many were queued.
    long long queue_reembed(const std::string& model, int dim);
    // Deletes vectors of any other model or dimension (the re-index completed).
    void drop_other_vectors(const std::string& model, int dim);
    long long vectors_of(const std::string& model, int dim);
    // Binary Hamming scan over the vectors of (model, dim), then int8 rescoring of the best `coarse`.
    std::vector<std::pair<long long, double>> vector_search(const std::vector<float>& query, const std::string& model, int dim,
                                                            int limit, int coarse = 200);

    // ---- Extraction ----
    std::vector<ChunkRow> extract_batch(int n, const std::vector<long long>& deferred = {});
    long long extract_pending();
    void mark_extracted(const std::vector<long long>& chunk_ids);
    // The passages one extraction request saw, and those whose findings took their date from the item. With the
    // evidence schema a later revision rechecks only findings whose inputs changed; without a record it rechecks all.
    Status record_extraction(const std::vector<long long>& chunk_ids, const std::vector<long long>& item_dated);
    void clear_extract_queue();

    // ---- Entities ----
    // Finds or creates the entity: a Person by lower-cased email when there is one, anything else by type and normalised
    // name. A Person without an address is linked to an existing Person only when the name matches one unambiguously.
    long long resolve_entity(const std::string& type, const std::string& name, const std::string& email, long long now);
    std::optional<EntityRow> entity(long long id);
    std::vector<EntityRow> find_entities(const std::string& query, int limit);
    std::vector<EntitySearchRow> find_entities_page(const std::string& query, int limit,
                                                    const std::optional<EntityCursor>& after = std::nullopt);
    bool entity_has_directory_relation(long long entity_id);
    std::vector<ItemRow> entity_evidence_items(long long entity_id, long long after_item_id, int limit);
    std::vector<ItemRow> entity_alias_items(long long entity_id, const std::string& normalized_alias, long long after_item_id, int limit);
    std::vector<EntityRow> entities_in_chunks(const std::vector<long long>& chunk_ids);
    // Entities one of whose names (three letters or more) occurs in the text as whole words: the query's entities.
    std::vector<long long> entities_named_in(const std::string& text, int limit);
    void add_mention(long long chunk_id, long long entity_id, const std::string& alias);
    long long add_relation(long long subject, const std::string& predicate, long long object, double confidence,
                           const std::string& valid_from, const std::string& valid_to, long long chunk_id);
    std::vector<RelationRow> relations_of(long long entity_id, int limit);
    std::optional<RelationRow> relation(long long id);
    bool delete_relation(long long id);
    // Relations extracted from the item's chunks, and the entities its chunks mention.
    std::vector<RelationRow> relations_of_item(long long item_id);
    std::vector<EntityRow> entities_of_item(long long item_id);
    // Renames an entity; its old name stays as an alias. Fails with "conflict" when another entity has the new key.
    Status rename_entity(long long id, const std::string& name);
    // Fact tombstones (design D5): a fact the user deleted or corrected is not extracted again from the same item version.
    void add_tombstone(const std::string& source, const std::string& ext_id, const std::string& item_hash, const std::string& fact_key);
    bool tombstoned(const std::string& source, const std::string& ext_id, const std::string& item_hash, const std::string& fact_key);
    // Chunks that mention any of the entities, newest first.
    std::vector<long long> chunks_mentioning(const std::vector<long long>& entity_ids, int limit);
    // Entities one relation away.
    std::vector<long long> neighbours(const std::vector<long long>& entity_ids, int limit);
    void set_enrichment(long long id, const std::string& name, const std::string& department, const std::string& title,
                        const std::string& manager, long long at);
    std::vector<EntityRow> people_to_enrich(long long older_than, int limit);
    // Moves aliases, mentions, relations and facts of `from` to `into`, then deletes `from`.
    Status merge_entities(long long from, long long into);
    // Moves one alias with its mentions (and the relations from those chunks) to a new entity of the same type.
    Result<long long> split_entity(long long id, const std::string& alias, long long now);
    Status forget_entity(long long id);

    // ---- User-stated facts ----
    long long add_fact(long long entity_id, const std::string& text, const std::string& thread_id, long long now,
                       const std::string& item_source = {}, const std::string& item_ext_id = {});
    std::vector<FactRow> facts_of(long long entity_id);
    std::optional<FactRow> fact(long long id);
    bool update_fact(long long id, const std::string& text);
    bool delete_fact(long long id);
    // User-stated facts with the item as provenance.
    std::vector<FactRow> facts_of_item(const std::string& source, const std::string& ext_id);

    // ---- Sharing (add-knowledge-sharing design D3) ----
    std::vector<ShareRow> shares();
    std::optional<ShareRow> share(const std::string& source, const std::string& ext_id);
    Status put_share(const ShareRow& s);
    Status repair_ambiguous_shares();
    Status mark_shares_unsharing();
    Status mark_share_removed(const std::string& source, const std::string& ext_id);
    Status remove_share(const std::string& source, const std::string& ext_id);
    // Share rules: a folder whose supported files are each shared with the Groups.
    std::vector<std::pair<std::string, std::vector<std::string>>> share_rules();
    Status put_share_rule(const std::string& folder, const std::vector<std::string>& groups);
    Status remove_share_rule(const std::string& folder);
    // Facts whose text matches the query words, for knowledge.search.
    std::vector<FactRow> find_facts(const std::string& query, int limit);

    // ---- Budgets (per local day) ----
    long long used(const std::string& day, const std::string& kind);
    void add_used(const std::string& day, const std::string& kind, long long n);

    // ---- Meta ----
    std::string meta(const std::string& key);
    void set_meta(const std::string& key, const std::string& value);

    // Removes everything (forget the whole knowledge store); Sources, folders, documents, notes, exclusions and sharing
    // stay unless `all`.
    Status clear(bool all = false);

private:
    Status withdraw_findings(const std::vector<long long>& chunk_ids);
    Status stage_passages(const ItemInput& in);
    Status prepare_item_removal_steps(const std::string& source, const std::string& ext_id);
    Result<KnowledgeImpactPreview> preview_evidence_snapshot(std::unordered_set<long long> removed,
                                                            std::optional<long long> item_id);
    std::unique_ptr<Database> db_;
};

// "zprava o rozpoctu" -> "\"zprava\" OR \"o\" OR \"rozpoctu\"" (FTS5 query of the words, quoted).
std::string fts_query(const std::string& text);

}  // namespace wb::knowledge
