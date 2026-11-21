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
// Appended after kUntrustedDataInstructions in every Run and subagent Run (Tool Call Size Instructions).
extern const char* const kToolCallSizeInstructions;

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
// The allowance for one image: images are sent downscaled to at most 1568 px a side (model::ImageLimits), and the
// costliest common accounting, one token per 28 x 28 px patch, gives 1568 * 1568 / 784 = 3,136 tokens plus framing.
inline constexpr size_t kImageTokenAllowance = 3'221;
// Request estimate including message framing, tool calls, tool definitions and kImageTokenAllowance per image.
size_t estimate_request_tokens(const std::vector<model::ChatMessage>& messages, const std::vector<model::ToolSpec>& tools);

// client/agent-runtime Tool Result Previews (keep-result-preview-ends design D1, D2): a preview of at most `budget`
// bytes keeps content[0, head_end) and content[tail_begin, end). Each part gets about half; a cut moves to a line break
// within kPreviewLineSnap bytes, towards the omitted middle, and never splits a UTF-8 character.
struct PreviewCut {
    size_t head_end = 0;
    size_t tail_begin = 0;
};
inline constexpr size_t kPreviewLineSnap = 256;
PreviewCut preview_cut(std::string_view content, size_t budget);

// client/agent-runtime Large Tool Results (cap-tool-results-by-context design D1, D2): a share of the context window.
inline constexpr long long kResultShareDivisor = 4;    // one result: a quarter of the window
inline constexpr long long kResponseShareDivisor = 2;  // the results of one response: half of the window

// The summarizer's completion limit when the small role allows it: room for its reasoning and a summary of at most
// 400 words (design D4, measured in task 1.3).
inline constexpr int kSummaryOutputTokens = 8'192;
// The estimated room kept in each summarizer request for the previous summary, unless that summary is larger.
inline constexpr long long kSummaryRoomTokens = 2'048;

// Reconciles estimates with provider prompt usage (Complete Request Accounting; design D4): the factor is the largest
// ratio of provider prompt tokens to the client's estimate among the last kSamples requests of at least kMinEstimate
// estimated tokens, at least 1 and at most kMaxFactor. Not thread-safe; the runtime guards it.
class UsageCalibration {
public:
    static constexpr size_t kSamples = 8;
    static constexpr size_t kMinEstimate = 1'024;
    static constexpr double kMaxFactor = 2.0;

    // Records one successful request: its estimate as sent and the provider's prompt tokens (ignored when unknown).
    void observe(size_t estimate, long long prompt_tokens);
    [[nodiscard]] double factor() const;
    // The estimate scaled by factor(), rounded up; never below `estimate`.
    [[nodiscard]] size_t apply(size_t estimate) const;

private:
    std::vector<double> ratios_;  // oldest first, at most kSamples
};

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

// One record of the summarizer's input: a whole message (parts 1), or part `part` (0-based) of `parts` of a message too
// large for one pass, covering content bytes [offset, offset + length).
struct TranscriptUnit {
    size_t message = 0;
    size_t offset = 0, length = 0;
    int part = 0, parts = 1;
};
// The units of msgs[0, end) in order. A message whose JSON-encoded content is estimated (estimate_context_tokens) above
// part_tokens is split into parts within it, never inside a UTF-8 sequence and at a line or word break in the last
// quarter of a part when there is one; its tool calls go with its last part. Together the parts hold every byte of the
// content exactly once.
std::vector<TranscriptUnit> transcript_units(const std::vector<ContextMessage>& msgs, size_t end, size_t part_tokens);
// Renders units [begin, end) like render_transcript_range; a part is a "message_part" record with its 1-based part and
// the number of parts. Refuses what does not fit, images and text that is not valid UTF-8.
std::optional<std::string> render_units(const std::string& previous_summary, const std::vector<ContextMessage>& msgs,
                                        const std::vector<TranscriptUnit>& units, size_t begin, size_t end, size_t max_chars);

// The longest prefix of `text` of at most `max_bytes` bytes that does not end inside a UTF-8 sequence.
std::string_view utf8_prefix(std::string_view text, size_t max_bytes);
// `bytes` without leading UTF-8 continuation bytes and without a trailing incomplete sequence: the offsets skipped at the
// start and dropped at the end are returned through the out parameters.
std::string_view utf8_trim(std::string_view bytes, size_t* skipped_front, size_t* dropped_back);

}  // namespace wb::runtime
