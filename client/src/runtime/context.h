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

// About four characters per token, over the message texts, tool calls and tool definitions.
size_t estimate_request_tokens(const std::vector<model::ChatMessage>& messages, const std::vector<model::ToolSpec>& tools);

// The index of the first message to keep when compacting `msgs` so that the `keep` most recent messages stay:
// never a tool result, never after an assistant message with unanswered tool calls, never inside the carried messages.
// None when there is nothing to compact.
std::optional<size_t> compaction_boundary(const std::vector<ContextMessage>& msgs, size_t keep);

// The summariser's input: the previous summary and msgs[0, end), each message cut to `per_message` characters, the whole
// cut to `max_chars` (the newest part is kept).
std::string render_transcript(const std::string& previous_summary, const std::vector<ContextMessage>& msgs, size_t end,
                              size_t max_chars, size_t per_message);

// The longest prefix of `text` of at most `max_bytes` bytes that does not end inside a UTF-8 sequence.
std::string_view utf8_prefix(std::string_view text, size_t max_bytes);
// `bytes` without leading UTF-8 continuation bytes and without a trailing incomplete sequence: the offsets skipped at the
// start and dropped at the end are returned through the out parameters.
std::string_view utf8_trim(std::string_view bytes, size_t* skipped_front, size_t* dropped_back);

}  // namespace wb::runtime
