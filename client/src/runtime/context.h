// The model's view of a Run: untrusted-data delimiters (client/agent-runtime Untrusted Content Provenance; design D6),
// request size estimates and the compaction boundary (Context Compaction; design D4), and previews of large tool
// results (Large Tool Results; design D4a). Pure functions, no I/O.
#pragma once

#include "model/types.h"
#include "runtime/run_store.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::runtime {

// The paragraph every Run's system prompt ends with: what an untrusted-data block is and that it is data only.
extern const char* const kUntrustedDataInstructions;

// A fresh per-Run boundary token: 16 URL-safe random characters.
std::string new_nonce();

// Wraps untrusted text in the delimited block
//   <<<untrusted-data id=NONCE source=KIND:ID>>>
//   ...content, with every "<<<" defused...
//   <<<end-untrusted-data id=NONCE>>>
std::string wrap_untrusted(std::string_view content, std::string_view nonce, std::string_view source_kind,
                           std::string_view source_id);

// Heuristic only: count non-ASCII bytes individually and allow one token per three ASCII bytes. It deliberately
// overweights UTF-8 compared with bytes/4, but is not a provider tokenizer or a guaranteed upper bound.
size_t estimate_context_tokens(std::string_view text);
// Request estimate including message framing, tool calls, tool definitions and a fixed image allowance.
size_t estimate_request_tokens(const std::vector<model::ChatMessage>& messages, const std::vector<model::ToolSpec>& tools);

struct WorkingContextBudget {
    long long input = 0;
    long long trigger = 0;
    long long target = 0;
};

// An absent result means that the known window and output reservation leave no safe input allowance.
std::optional<WorkingContextBudget> working_context_budget(long long window, long long output_reserve);

// The index of the first message to keep when compacting `msgs` so that the `keep` most recent messages stay:
// never a tool result, never after an assistant message with unanswered tool calls, never inside the carried messages.
// None when there is nothing to compact.
std::optional<size_t> compaction_boundary(const std::vector<ContextMessage>& msgs, size_t keep);

// Render the complete summary and message range, or refuse when it does not fit or contains images that this text-only
// summarizer cannot account for. Never silently truncate content that will be removed from the model's view.
std::optional<std::string> render_transcript(const std::string& previous_summary, const std::vector<ContextMessage>& msgs,
                                             size_t end, size_t max_chars);
std::optional<std::string> render_transcript_range(const std::string& previous_summary, const std::vector<ContextMessage>& msgs,
                                                   size_t begin, size_t end, size_t max_chars);

// The longest prefix of `text` of at most `max_bytes` bytes that does not end inside a UTF-8 sequence.
std::string_view utf8_prefix(std::string_view text, size_t max_bytes);
// `bytes` without leading UTF-8 continuation bytes and without a trailing incomplete sequence: the offsets skipped at the
// start and dropped at the end are returned through the out parameters.
std::string_view utf8_trim(std::string_view bytes, size_t* skipped_front, size_t* dropped_back);

}  // namespace wb::runtime
