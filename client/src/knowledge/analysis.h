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
    // "conflict": incompatible claims; "uncertain_entity": the two facts may or may not be about the same person or thing.
    std::string kind = "conflict";
    AnalysisSpan left, right;
};

// A new category name for the user to review (task 8.6); it places no fact until it is accepted.
struct CategoryNameFact {
    long long fact_id = 0;
    AnalysisSpan evidence;
};

struct CategoryNameProposal {
    std::string name, reason;
    std::vector<CategoryNameFact> facts;
};

struct AnalysisProposals {
    std::vector<CategoryProposal> categories;
    std::vector<ConflictProposal> conflicts;
    std::vector<CategoryNameProposal> names;  // optional "new_categories" of a categorization answer
};

// At most this many new names in one answer, each 1..kMaxCategoryNameBytes bytes long.
inline constexpr size_t kMaxNamesPerAnswer = 3;
inline constexpr size_t kMaxCategoryNameBytes = 60;

// Refuses the entire response if any proposal references foreign IDs, has ambiguous or missing evidence, is a
// duplicate, or exceeds limits. A validated proposal is still an untrusted suggestion requiring review.
std::optional<AnalysisProposals> parse_analysis_proposals(std::string_view text, const AnalysisEvidence& facts,
                                                         const std::set<long long>& category_ids);

// The key two category names are compared by: the normalized name with simple plural endings removed from each word.
std::string category_name_key(std::string_view name);
// Whether two keys name the same category: equal, or, when both have at least 5 bytes, within one edit (two when the
// longer has at least 10 bytes).
bool category_keys_near(std::string_view a, std::string_view b);

}  // namespace wb::knowledge
