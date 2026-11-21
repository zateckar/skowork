// The portable part of client/knowledge-graph (add-knowledge-graph): file kinds and text decoding for the plain-text and
// source-code readers, the chunkers (design D3), exclusion globs, binary and int8 vector quantisation (design D1, D2),
// reciprocal rank fusion (D6) and the parser of extraction output (D4). The fuzz job builds it: everything here takes
// untrusted file, mail, transcript and model content.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::knowledge {

// About 256 tokens of four characters, with a 32-token overlap (design D3).
inline constexpr size_t kChunkChars = 1024;
inline constexpr size_t kOverlapChars = 128;
// Transcripts split by speaker turn in windows of about 2 minutes.
inline constexpr long long kTranscriptWindowMs = 120'000;
// The most text one item contributes (a larger document is indexed up to here).
inline constexpr size_t kMaxItemText = 4u << 20;

struct Chunk {
    std::string heading;  // the section heading, symbol or speakers
    std::string text;
    long long at = 0;     // unix ms of the passage when the source has one (messages, transcript turns)
};

// Rough token count, about four characters per token (the embedding budget is counted with it).
long long estimate_tokens(std::string_view text);

// ---- Files ----------------------------------------------------------------------------------------------------------

// The supported kind of a file by its extension: docx, xlsx, pptx, pdf (Document Text Extraction of client/core-tools),
// md, txt or code; empty for anything else (File Ingestion Scope).
std::string file_kind(std::string_view path);
// Office and pdf documents, whose text comes from the sandboxed extract worker.
bool is_document_kind(std::string_view kind);
// A NUL byte in the first 8 KB (and no UTF-16 byte-order mark): not text.
bool looks_binary(std::string_view bytes);
// The text of a plain-text or source file as UTF-8: byte-order marks (UTF-8, UTF-16 LE/BE) honoured, invalid UTF-8
// sequences replaced with U+FFFD.
std::string decode_text(std::string_view bytes);

// ---- Chunkers (design D3) -------------------------------------------------------------------------------------------

// Documents: split by headings (Markdown #) and paragraphs into about kChunkChars with kOverlapChars of overlap.
std::vector<Chunk> chunk_document(std::string_view text);
// Source code: by top-level symbol (a line at column 0 after a blank line or a closing line), falling back to lines.
std::vector<Chunk> chunk_code(std::string_view text);
// Mail: one message, its quoted history stripped first.
std::string strip_quoted(std::string_view body);
std::vector<Chunk> chunk_mail(std::string_view body);

struct Turn {
    std::string speaker, text;
    long long at = 0;  // unix ms
};
// Transcripts: by speaker turn, in windows of about kTranscriptWindowMs.
std::vector<Chunk> chunk_transcript(const std::vector<Turn>& turns);
// Thread messages: "Speaker: text", packed message by message.
std::vector<Chunk> chunk_messages(const std::vector<Turn>& messages);
// By kind: md, txt and the document kinds as documents, code as code, mail as mail.
std::vector<Chunk> chunk_text(std::string_view kind, std::string_view text);

json chunks_json(const std::vector<Chunk>& chunks);
std::vector<Chunk> chunks_from_json(const json& j);
// FNV-1a 64 of the content, in hex: change detection only.
std::string content_hash(std::string_view bytes);

// The index worker's requests (design D7), also callable in process by tests:
//   index.read  {path, kind, max_bytes}         a plain-text or source file -> {chunks, hash, bytes}; errors too_large,
//                                               binary, unsupported, read_failed
//   index.chunk {kind, text} | {kind, turns}    text of a document, mail, transcript (turns) or Thread (turns) -> {chunks}
Result<json> index_request(const std::string& method, const json& params);

// ---- Exclusions -----------------------------------------------------------------------------------------------------

// Case-insensitive glob over a path with / and \ treated alike: ** matches across folders, * within one name, ? one
// character. A pattern with no separator is matched against the file name only (*.pst).
bool glob_match(std::string_view pattern, std::string_view path);

// A name for alias matching: lower case, Czech and German letters folded, punctuation dropped, spaces collapsed.
std::string normalize_name(std::string_view name);

// ---- Vectors (design D2) --------------------------------------------------------------------------------------------

// One bit per dimension (set when the component is positive): the coarse search reads these.
std::vector<std::uint8_t> quantize_binary(const std::vector<float>& v);
// The vector scaled to signed bytes (-127..127), stored as raw bytes, for rescoring.
std::vector<std::uint8_t> quantize_int8(const std::vector<float>& v);
int hamming(const std::uint8_t* a, const std::uint8_t* b, size_t n);
double int8_cosine(const std::uint8_t* a, const std::uint8_t* b, size_t n);

// Reciprocal rank fusion of ranked id lists (k = 60): id -> fused score.
std::map<long long, double> reciprocal_rank_fusion(const std::vector<std::vector<long long>>& lists, double k = 60.0);

// ---- Extraction (design D4) -----------------------------------------------------------------------------------------

// Salience of a fact nobody has rated (design D7), the days a temporary fact stays current (design D4) and the longest
// statement (design D3, in bytes so that it is also at most 200 characters).
inline constexpr double kDefaultSalience = 0.5;
inline constexpr int kTemporaryDays = 30;
inline constexpr size_t kMaxStatementBytes = 200;

// Person, Project, Organization, Document, Meeting, Topic, System.
const std::vector<std::string>& entity_types();

struct ExtractedEntity {
    std::string name, type, email;
    std::vector<int> chunks;  // indexes into the batch
};
struct ExtractedRelation {
    std::string subject, predicate, object;  // entity names of the same output
    double confidence = 0.5;
    std::string valid_from, valid_to;  // YYYY-MM-DD or empty
    int chunk = -1;
    // add-knowledge-salience design D2/D3: importance for the user's future work (0..1), durability (lasting, temporary,
    // trivial) and a self-contained statement of at most kMaxStatementBytes bytes.
    double salience = kDefaultSalience;
    std::string durability = "lasting";
    std::string statement;
};
struct Extraction {
    std::vector<ExtractedEntity> entities;
    std::vector<ExtractedRelation> relations;
};

// The JSON schema the small model is asked to answer with.
const json& extraction_schema();
// The model's answer (JSON, possibly in a ``` fence) checked against the schema: unknown types, out-of-range chunk
// indexes and relations between unknown entities are dropped, names and predicates are bounded. nullopt: not JSON.
// Salience is clamped to 0..1, an unknown durability becomes lasting, and a statement is cut at a word boundary.
std::optional<Extraction> parse_extraction(std::string_view text, int chunk_count);

// ---- Source context (add-knowledge-salience design D1) ----------------------------------------------------------------

// What the client knows about the Source of a passage. It comes from the item's metadata, never from the passage text.
struct SourceContext {
    std::string source;     // a Source kind: threads, files, mail, calendar, meetings, notes
    std::string title;      // the item title (mail subject, meeting title, file name)
    std::string author;     // the author's display name, or address when there is none
    bool author_is_user = false;
    int recipients = 0;     // mail: recipients; calendar and meetings: attendees; 0 when unknown
    std::string breadth;    // direct, group, broadcast; empty when unknown
    std::string folder;     // files: the folder name
    bool organization_sender = false;  // the author is in the signed-in user's organization domain (retrieval boost D5)
};
// One rating of the background pass for an existing fact (design D7): its index in the request, salience, durability.
struct RelationScore {
    int index = -1;
    double salience = kDefaultSalience;
    std::string durability = "lasting";
};
// The answer to a scoring request: {"ratings": [{"i", "salience", "durability"}]}. Entries outside 0..count-1 are
// ignored; nullopt when the answer is not JSON.
std::optional<std::vector<RelationScore>> parse_relation_scores(std::string_view text, int count);
// Addressing breadth (design D1): one recipient or a direct Chat is direct, 2 to 10 group, more broadcast.
std::string addressing_breadth(const std::string& source, int recipients);
json source_context_json(const SourceContext& c);
SourceContext source_context_from_json(const json& j);
// The passage as the extraction request shows it: `<passage ...>` with client-derived attributes and the text. Text that
// would open or close a passage tag is neutralised, so a passage cannot forge a header.
std::string render_passage(int index, const std::string& date, const SourceContext& context, std::string_view text);
// A durability word, lower-cased; anything but temporary and trivial is lasting.
std::string normalize_durability(std::string_view durability);

// ---- Retrieval weights (add-knowledge-salience design D5) ---------------------------------------------------------------

// w = (0.6 + 0.4 * salience) times the breadth factor (a broadcast from the organization 1.15, a Chat 0.9, else 1.0).
double salience_weight(double salience, const SourceContext& context, bool organization_sender);
// Applies a weight to a score that may be negative: a higher weight always moves a result up.
double weighted_score(double score, double weight);

// The importance level the Knowledge view shows (design D6): high, normal or low, never a number.
std::string importance_level(double salience);

// ---- Common-sense triage (add-common-sense-knowledge-triage design D2) ------------------------------------------------

// The version of the triage rules; a stored lower version triggers the one-time cleanup (design D3).
// 2: the cleanup also queues every Thread for indexing, so Threads indexed with the run-time line are indexed again.
// 3: Local App names match spelling variants. 4: rubric 2 is rated.
inline constexpr int kTriageRules = 4;
// The version of the rating rubric (design D1): facts are rated again only when it changes, not with every rules
// change. Rules 1 and 2 came with rubric 1. Rubric 2: "Me" and the passage author are the user, not a placeholder.
inline constexpr int kTriageRubric = 2;
// The output cap of extraction, rating and resolution requests: a reasoning model spends it on reasoning first.
inline constexpr int kKnowledgeOutputTokens = 8192;
// Why an entity name is never kept: "assistant" (Skowork itself; in a Thread also a well-known AI assistant or model,
// design D6), "placeholder" (Speaker 1, Me, the user, ...), "path" (a file path or file name named in a Thread) or
// "local_app" (a Thread naming one of `local_apps`, ids or titles). Empty when the name passes. `source` is the Source
// kind of the passage.
std::string common_sense_reject_name(std::string_view name, std::string_view source,
                                     const std::vector<std::string>& local_apps = {});
// Why a candidate relation is never stored: a name reason above, "meta" (a predicate that only says something was
// discussed, mentioned or relates to something) or "self" (subject and object are the same). Empty when it passes.
std::string common_sense_reject(std::string_view subject, std::string_view predicate, std::string_view object,
                                std::string_view source, const std::vector<std::string>& local_apps = {});
// "Me", "I", "the user" and their cs and de equivalents: the author of a meeting capture, Thread or note (design D5).
bool self_reference_name(std::string_view name);

// ---- Duplicate entity resolution (add-common-sense-knowledge-triage design D7) -----------------------------------------

// Whether two entity names are similar enough to ask the model whether they name the same thing: a small edit distance
// of the folded names, one containing the other as a word, or a shared word of 4 or more characters (honorifics
// dropped).
bool similar_entity_names(std::string_view a, std::string_view b);
// The answer to a resolution request: {"groups": [[i, j, ...], ...]}. Indexes outside 0..count-1 and repeats are
// dropped, and groups with fewer than two names are ignored; nullopt when the answer is not JSON.
std::optional<std::vector<std::vector<int>>> parse_entity_groups(std::string_view text, int count);

}  // namespace wb::knowledge
