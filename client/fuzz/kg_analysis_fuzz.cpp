// Fuzz target for structured model output of knowledge analysis (improve-skowork-and-knowledge-control task 8.2;
// design D7, analysis response validator). Category and possible-inconsistency proposals come from a model and are
// untrusted: whatever the bytes, the validator either refuses the whole answer or returns proposals that only name
// supplied facts and categories, pair two different facts once, carry a bounded nonempty reason and a known kind, and
// cite a quote that occurs exactly once inside the passage supplied for that fact.
#include "fuzz_main.h"
#include "knowledge/analysis.h"

#include <set>
#include <utility>

using namespace wb::knowledge;

namespace {

// Facts 1-4 with their passages: multibyte text, a phrase that occurs twice, and a passage shared by two facts.
const AnalysisEvidence& evidence() {
    static const AnalysisEvidence e = {
        {1, {{10, "Projekt začne v červnu."}, {11, "Rozpočet je 5 mil. EUR."}}},
        {2, {{20, "The project starts in July."}}},
        {3, {{30, "Budget: 5000 kEUR. Budget: 5000 kEUR."}, {31, "Petr Novotný owns the budget."}}},
        {4, {{31, "Petr Novotný owns the budget."}, {40, ""}}}};
    return e;
}

const std::set<long long>& categories() {
    static const std::set<long long> c = {7, 8};
    return c;
}

void check_span(long long fact, const AnalysisSpan& span) {
    const auto f = evidence().find(fact);
    if (f == evidence().end()) __builtin_trap();
    const auto p = f->second.find(span.chunk_id);
    if (p == f->second.end()) __builtin_trap();
    const std::string& text = p->second;
    if (span.begin >= span.end || span.end > text.size() || span.end - span.begin > 2048) __builtin_trap();
    const std::string quote = text.substr(span.begin, span.end - span.begin);
    if (text.find(quote) != span.begin || text.find(quote, span.begin + 1) != std::string::npos) __builtin_trap();
}

void check_reason(const std::string& reason) {
    if (reason.empty() || reason.size() > 512 || reason.find_first_not_of(" \t\r\n") == std::string::npos) __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    // With and without categories: a consistency request supplies none, so any category proposal refuses it.
    for (const bool with_categories : {true, false}) {
        const std::set<long long> none;
        const auto& allowed = with_categories ? categories() : none;
        const auto x = parse_analysis_proposals(text, evidence(), allowed);
        if (!x) continue;
        if (x->categories.size() > 200 || x->conflicts.size() > 200) __builtin_trap();
        std::set<std::pair<long long, long long>> assigned, paired;
        for (const auto& c : x->categories) {
            if (!evidence().count(c.fact_id) || !allowed.count(c.category_id)) __builtin_trap();
            if (!assigned.emplace(c.fact_id, c.category_id).second) __builtin_trap();
            check_reason(c.reason);
            check_span(c.fact_id, c.evidence);
        }
        for (const auto& c : x->conflicts) {
            if (c.left_id == c.right_id || !evidence().count(c.left_id) || !evidence().count(c.right_id)) __builtin_trap();
            if (!paired.emplace(std::min(c.left_id, c.right_id), std::max(c.left_id, c.right_id)).second) __builtin_trap();
            if (c.kind != "conflict" && c.kind != "uncertain_entity") __builtin_trap();
            check_reason(c.reason);
            check_span(c.left_id, c.left);
            check_span(c.right_id, c.right);
        }
    }
    return 0;
}
