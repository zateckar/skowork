// Validation of untrusted category and possible-conflict proposals against a bounded evidence snapshot.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wb::knowledge {

// fact id -> (chunk id -> text from the particular revision supplied for analysis).
using AnalysisEvidence = std::map<long long, std::map<long long, std::string>>;

struct AnalysisSpan {
    long long chunk_id = 0;
    size_t begin = 0, end = 0;  // UTF-8 byte offsets in the supplied revision's chunk text
};

struct CategoryProposal {
    long long fact_id = 0, category_id = 0;
    std::string reason;
    AnalysisSpan evidence;
};

struct ConflictProposal {
    long long left_id = 0, right_id = 0;
    std::string reason;
    AnalysisSpan left, right;
};

struct AnalysisProposals {
    std::vector<CategoryProposal> categories;
    std::vector<ConflictProposal> conflicts;
};

// Refuses the entire response if any proposal references foreign IDs, has ambiguous or missing evidence, is a
// duplicate, or exceeds limits. A validated proposal is still an untrusted suggestion requiring review.
std::optional<AnalysisProposals> parse_analysis_proposals(std::string_view text, const AnalysisEvidence& facts,
                                                         const std::set<long long>& category_ids);

}  // namespace wb::knowledge
