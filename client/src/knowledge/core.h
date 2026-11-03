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
};
struct Extraction {
    std::vector<ExtractedEntity> entities;
    std::vector<ExtractedRelation> relations;
};

// The JSON schema the small model is asked to answer with.
const json& extraction_schema();
// The model's answer (JSON, possibly in a ``` fence) checked against the schema: unknown types, out-of-range chunk
// indexes and relations between unknown entities are dropped, names and predicates are bounded. nullopt: not JSON.
std::optional<Extraction> parse_extraction(std::string_view text, int chunk_count);

}  // namespace wb::knowledge
