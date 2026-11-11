// client/knowledge-graph in the host (add-knowledge-graph): Source enablement and exclusions, the connectors (Threads,
// files, mail, calendar, meetings), the background indexer with its embedding, extraction and enrichment steps and their
// daily budgets, hybrid retrieval, the knowledge.* tools, status and control for the settings section, and the forget
// cascade. Everything stays in kg.db; text leaves the laptop only in model requests through client/model-access, and
// (add-knowledge-sharing) the text of the documents, folders and notes the user shares, which goes to
// control-plane/team-knowledge. The Knowledge view's documents, notes, curation, sharing and team search live here too.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "knowledge/kg_store.h"
#include "model/model_client.h"
#include "runtime/tools.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace wb::knowledge {

using PolicyLookup = std::function<std::optional<json>(const std::string& key)>;

// The knowledge policy (client/knowledge-graph; control-plane keys knowledge.*), read on every use.
struct KnowledgeConfig {
    std::map<std::string, bool> source_policy;  // kind -> policy allows (knowledge.sources.<kind>, default on)
    bool extraction = true;                     // knowledge.extraction
    bool directory_enrichment = true;           // knowledge.directory_enrichment
    long long max_file_mb = 25;                 // knowledge.max_file_mb
    long long embedding_daily_tokens = 10'000'000;
    long long extraction_daily_calls = 2000;
    int max_threads = 2;
    std::vector<std::string> exclusions;  // knowledge.exclusions (globs)
    bool save_as_note = true;             // knowledge.save_as_note (add-save-as-note design D5)

    static KnowledgeConfig from(const PolicyLookup& policy);
};

struct KnowledgeEnv {
    const Clock* clock = nullptr;
    PolicyLookup policy;
    // The index worker (design D7): index.read and index.chunk. Tests answer in process with index_request().
    std::function<Result<json>(const std::string& method, const json& params)> index;
    // Document Text Extraction of client/core-tools (docx, xlsx, pptx, pdf) from the file's bytes.
    std::function<Result<std::string>(const std::string& kind, const std::string& bytes)> extract_document;
    // Model roles through client/model-access: embed, rerank and the small chat role.
    std::function<model::ModelResult<model::EmbedResult>(const std::vector<std::string>&, const model::RequestContext&)> embed;
    std::function<model::ModelResult<std::vector<double>>(const std::string&, const std::vector<std::string>&, const model::RequestContext&)> rerank;
    std::function<model::ModelResult<model::ChatResponse>(const model::ChatRequest&)> chat;
    // Microsoft Graph user directory through the mail-calendar connection: {displayName, department, jobTitle,
    // manager: {displayName, mail}}. Errors: not_found, offline, not_connected.
    std::function<Result<json>(const std::string& upn)> directory;
    // policy.filesystem read rules allow the path.
    std::function<bool(const std::string& path)> read_permitted;
    // The mail-calendar local copy: an item as the connector indexes it, or nullopt when it is gone or excluded
    // ({title, body, at, ref, withheld, people: [{name, address}]}), and the ids it holds. kind: mail | calendar.
    std::function<std::optional<json>(const std::string& kind, const std::string& id)> mail_item;
    std::function<std::vector<std::string>(const std::string& kind)> mail_ids;
    // Save as note (add-save-as-note): a mail message with its header ({subject, from: {name, address}, to, cc, at,
    // body, withheld}), and a Chat from the messaging cache ({chat: {name, kind, members}, messages: [...]}, oldest
    // first). nullopt when it is gone.
    std::function<std::optional<json>(const std::string& id)> mail_message;
    std::function<std::optional<json>(const std::string& chat)> messaging_chat;
    // Thread data of client/agent-runtime: {title, messages: [{role, text, at}]}, and the Threads to backfill.
    std::function<std::optional<json>(const std::string& thread_id)> thread;
    std::function<std::vector<std::string>()> thread_ids;
    // The signed-in user (name, email): knowledge.remember stores facts about them, and their domain marks internal people.
    std::function<std::pair<std::string, std::string>()> user;
    // Background work is deferred (pause, battery saver, presentation; client/desktop-host).
    std::function<bool()> deferred;
    std::function<void()> changed;  // status changed (settings section)
    // control-plane/team-knowledge through the control plane client (add-knowledge-sharing): (method, path, body) -> the
    // JSON answer. Error codes: offline when the control plane is not connected, else the server's code
    // (GROUP_NOT_PERMITTED, TOO_LARGE, QUOTA_EXCEEDED) or http_<status>. Unset: no sharing and no team search.
    std::function<Result<json>(const std::string& method, const std::string& path, const json& body)> team;
    // Refuse to re-upload an item while an earlier withdrawal for this account is pending.
    std::function<Result<bool>(const std::string& remote_id)> pending_item_withdrawal;
    // Sends the item removals Delete all left pending for the signed-in account (client/local-storage Pending Shared
    // Data Removal), using `remove`, which succeeds only when the server confirms the removal or the item's absence.
    std::function<void(const std::function<Status(const std::string& remote_id)>& remove)> retry_withdrawals;
    bool background = true;         // tests drive step() themselves
    long long sweep_interval_ms = 60'000;  // files rescan and reconciliation
};

// The category and history filter the knowledge tools and the Knowledge view share (client/knowledge-graph Retrieval
// Respects Knowledge State).
struct KnowledgeFilter {
    std::optional<long long> category;  // a category id; 0 is Uncategorized
    bool history = false;               // also return facts that are no longer current, labeled
};

struct SearchOptions {
    int limit = 8;
    std::vector<std::string> sources;  // empty: all
    std::string run_id, thread_id;
    std::shared_ptr<CancelToken> cancel;
    bool team = false;  // also search control-plane/team-knowledge (knowledge.search and the Knowledge view)
    // Only passages that support current facts of this category (0: a fact without a category, or no fact). Team
    // knowledge has no local categories and is not searched then.
    std::optional<long long> category;
};

struct Passage {
    long long chunk_id = 0, item_id = 0;
    std::string source, title, ref, heading, text;
    long long date = 0;  // unix ms
    double score = 0;
    // A team passage (source "team"): the owner, the Groups it is shared with, and its id in team knowledge.
    std::string owner, team_item;
    std::vector<std::string> groups;
};

struct SearchResult {
    std::vector<Passage> passages;
    std::vector<FactRow> facts;
    bool semantic = false;  // the query embedding was available
    bool reranked = false;
    bool team_searched = false;
    std::string team_note;  // why team knowledge was not searched
    bool file_source_problem = false;  // selected file knowledge may not reflect the current files
};

// A Save as note draft within the note size limit (add-save-as-note design D2): cut at a character boundary, with a
// closing line saying it was shortened.
std::string fit_note_text(std::string text, size_t title_bytes, size_t limit);

class KnowledgeService {
public:
    KnowledgeService(KnowledgeEnv env, std::unique_ptr<KgStore> store);
    ~KnowledgeService();
    KnowledgeService(const KnowledgeService&) = delete;
    KnowledgeService& operator=(const KnowledgeService&) = delete;

    void start();
    void stop();
    void policy_changed();

    // ---- Connectors ----
    // A Run finished in the Thread (the threads Source).
    void thread_finished(const std::string& thread_id);
    Status thread_forgotten(const std::string& thread_id);
    // A mail-calendar event: mail.received, mail.updated, mail.deleted, event.created, event.updated, event.cancelled.
    void mail_event(const std::string& kind, const std::string& id);
    // The meetings connector (client/meeting-capture): a finished transcript, indexed in the meetings Source.
    void meeting_transcript(const std::string& meeting_id, const std::string& title, long long start_ms,
                            const std::vector<Turn>& turns, const std::string& ref);
    void meeting_removed(const std::string& meeting_id);

    // One round of background work: queued items, the periodic sweep, then one embedding, extraction and enrichment
    // batch. Returns whether anything was done. The background thread calls it; tests call it directly.
    bool step();
    // The host's background gate changed (deferral lifted, pause ended): look for work now instead of at the next poll.
    void wake();
    // step() until nothing is left to do (tests), at most `max_rounds`.
    void run_until_idle(int max_rounds = 1000);
    void sweep_now();

    // ---- Retrieval ----
    SearchResult search(const std::string& query, const SearchOptions& opt);
    // Apply the same current Source and exclusion checks to graph and reader results.
    bool item_visible(const ItemRow& item);
    bool chunk_visible(long long chunk_id);
    bool relation_visible(const RelationRow& relation);
    bool fact_visible(const FactRow& fact);
    bool entity_visible(long long entity_id);
    std::vector<EntityRow> visible_entities(const std::string& query, int limit);
    std::vector<std::string> visible_entity_aliases(const EntityRow& entity);
    // ---- The knowledge state filter (Retrieval Respects Knowledge State) ----
    // {category, history} of a tool call or Knowledge view request; an unknown category is not_found.
    Result<KnowledgeFilter> knowledge_filter(const json& params);
    // The relation as current knowledge: its state (current | disputed), accepted categories, the excerpts of permitted
    // current revisions it cites and the other side of each unresolved review. Null when the relation is not permitted,
    // not current knowledge (rejected, superseded, unsupported) or outside the filter's category.
    json current_relation(const RelationRow& relation, const KnowledgeFilter& filter);
    // Facts about the entity that are no longer current knowledge, labeled with their state and cited with the
    // retained excerpts and revision numbers of permitted items (an explicit history request).
    json fact_history(long long entity_id, const KnowledgeFilter& filter, int limit);
    // The categories a filter can name: [{id, name}], then Uncategorized.
    json categories();

    // ---- Tools ----
    std::vector<runtime::ToolDef> tools();

    // ---- Status and control (Knowledge sources settings section) ----
    json status();
    Status set_source_enabled(const std::string& kind, bool enabled);
    Status set_source_paused(const std::string& kind, bool paused);
    Status reindex(const std::string& kind);
    Status forget_source(const std::string& kind);
    Status forget_item(long long item_id);
    Status forget_entity(long long entity_id);
    Status merge_entities(long long from, long long into);
    Result<long long> split_entity(long long id, const std::string& alias);
    Status add_folder(const std::string& path);
    Status remove_folder(const std::string& path);
    Status add_exclusion(const std::string& kind, const std::string& value);
    Status remove_exclusion(const std::string& kind, const std::string& value);
    json entities(const std::string& query, int limit);
    // Indexed items by title or reference, for forgetting one: [{id, source, title, ref}].
    json items(const std::string& query, int limit);
    json entity_profile(long long id, const KnowledgeFilter& filter = {});
    // Forget everything (client/local-storage: forget all).
    Status forget_all();
    // Stops new item uploads and withdraws existing shares before a whole-profile wipe.
    Status prepare_delete_all();
    Result<std::vector<std::string>> freeze_for_delete_all();
    // Whole-profile Delete all: freezes uploads, removes what the server confirms now, and returns the remote ids
    // whose removal is still pending (offline, or not proven absent) for the caller to stage in the outbox.
    Result<std::vector<std::string>> withdraw_for_delete_all();
    void cancel_delete_all();

    // ---- Knowledge view (add-knowledge-sharing) ----
    // Single documents of the files Source.
    Status add_document(const std::string& path);
    Status remove_document(const std::string& path);
    // Notes: saving queues the note for indexing; returns its id. A new note takes `origin` ({kind: mail | chat, ref,
    // title}, from a draft); an edited note keeps its own.
    Result<long long> save_note(long long id, const std::string& title, const std::string& text, const json& origin = nullptr);
    // Save as note (add-save-as-note design D1, D2): {title, text, origin} for the editor; nothing is stored. Refused
    // with not_permitted while knowledge.save_as_note is off. period: day | week | all.
    Result<json> draft_from_mail(long long item_id);
    Result<json> draft_from_chat(const std::string& chat, const std::string& period);
    Status delete_note(long long id);
    json notes();
    json note(long long id);
    // Items of a Source with title, date, size, indexing and sharing state: [{id, source, title, ref, date, size,
    // pending, shared}]. The notes Source lists every note, indexed or not (note_id).
    json browse(const std::string& source, const std::string& query, int limit);
    // The number of items of every Source for the Source tabs ({source: n}); notes count every note.
    json source_counts();
    // An item with its indexed text, entities, facts and sharing state.
    // Facts are filtered by the same state filter and the filter's category.
    json item_detail(long long id, const KnowledgeFilter& filter = {});
    // Curation (Knowledge Curation, design D5): kind relation (an extracted fact) or user (a user-stated fact).
    Status edit_fact(const std::string& kind, long long id, const std::string& text);
    Status delete_fact(const std::string& kind, long long id);
    Status rename_entity(long long id, const std::string& name);

    // ---- Sharing (Sharing Knowledge Items, design D1, D3) ----
    // {enabled, policy_groups, permitted_groups, connected, items: [...], rules: [...]}
    json sharing();
    Status share_item(long long item_id, const std::vector<std::string>& groups);
    Status unshare_item(long long item_id);
    Status share_folder(const std::string& folder, const std::vector<std::string>& groups);
    Status unshare_folder(const std::string& folder);
    // Keeps shared items current: lists them on the server when `list` (start and every policy refresh, design D2),
    // re-publishes changed items, unshares deleted or excluded ones and publishes the files of share rules.
    void sync_sharing(bool list);

    [[nodiscard]] KgStore& store() { return *store_; }
    [[nodiscard]] KnowledgeConfig config() const { return KnowledgeConfig::from(env_.policy); }
    // Whether the Source may ingest now: policy, the user's switch and pause.
    [[nodiscard]] bool source_active(const std::string& kind);
    [[nodiscard]] std::string today() const;
    // The Person entity of the signed-in user (knowledge.remember about the user).
    long long user_entity();

private:
    struct Task {
        std::string op;  // upsert | remove
        std::string source, id;
        int priority = 2;
        json data;  // meetings: the transcript
    };
    void enqueue(Task t);
    bool excluded_path(const std::string& path, const KnowledgeConfig& cfg);
    bool excluded_item(const std::string& source, const std::string& id);
    Status apply_exclusions();
    bool process_task(const Task& t);
    void index_thread(const std::string& id, int priority);
    void index_mail(const std::string& kind, const std::string& id, int priority);
    void index_file(const std::string& path, int priority);
    void file_error(const std::string& path, const std::string& error, long long updated = 0);
    void index_meeting(const Task& t);
    std::optional<long long> put_eligible(const ItemInput& in, const std::function<void(long long)>& written_hook = {});
    void index_note(const std::string& id, int priority);
    json origin_json(const NoteRow& n);
    void scan_files();
    void search_team(const std::string& query, const SearchOptions& opt, SearchResult& out);
    std::vector<std::string> sharing_policy();
    std::vector<std::string> permitted_groups();
    // The text published for a shareable item: a note's text, or a file's (extracted) text.
    Result<std::string> share_text(const std::string& source, const std::string& ext_id);
    Status publish(ShareRow row, const ItemRow& item);
    bool unpublish(const ShareRow& row);  // false: not reachable, retried later
    void reconcile();
    bool embed_step();
    bool extract_step();
    bool enrich_step();
    // Returns the passages one of whose findings took its date from the item.
    std::vector<long long> store_extraction(const std::vector<ChunkRow>& batch, const Extraction& x);
    void notify();
    [[nodiscard]] bool deferred() const;
    void loop();
    long long now_ms() const { return env_.clock->unix_millis(); }

    KnowledgeEnv env_;
    std::unique_ptr<KgStore> store_;
    std::mutex mx_;
    std::deque<Task> tasks_;
    std::string file_recheck_scanned_;
    int file_scans_running_ = 0;
    long long last_sweep_ = 0;
    bool swept_once_ = false;
    bool list_shares_ = true;  // list the shared items at the next sweep (start, policy refresh)
    std::mutex share_mx_;      // one sharing pass at a time
    std::atomic<bool> deleting_all_{false};
    std::mutex file_status_mx_;
    std::string file_scan_error_;  // retained until the next complete file scan
    std::map<std::string, std::string> file_errors_;  // retained until this file succeeds or is no longer selected
    long long embed_retry_at_ = 0, extract_retry_at_ = 0, enrich_retry_at_ = 0;
    int extract_failures_ = 0;
    std::atomic<bool> extraction_delayed_{false};
    struct DeferredExtraction { long long item_id = 0, retry_at = 0; int failures = 0; std::string text; };
    std::mutex extraction_deferred_mx_;
    std::map<long long, DeferredExtraction> extraction_deferred_;
    bool isolate_extraction_ = false;
    int isolated_extract_failures_ = 0;
    std::string last_embed_error_, last_extract_error_;

    std::thread thread_;
    std::condition_variable wake_;
    bool wake_now_ = false;
    std::atomic<bool> stop_{false};
};

}  // namespace wb::knowledge
