#include "knowledge/analysis.h"

#include "base/json.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace wb::knowledge {
namespace {

constexpr size_t kMaxResponse = 256 * 1024;
constexpr size_t kMaxProposals = 200;
constexpr size_t kMaxReason = 512;
constexpr size_t kMaxQuote = 2048;

std::optional<long long> positive_id(const json& j, const char* name) {
    if (!j.is_object()) return std::nullopt;
    const auto it = j.find(name);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    const auto n = json_int(*it);
    return n && *n > 0 ? n : std::nullopt;
}

std::optional<std::string> reason(const json& j) {
    const auto it = j.find("reason");
    if (it == j.end() || !it->is_string()) return std::nullopt;
    const auto s = it->get<std::string>();
    if (s.empty() || s.size() > kMaxReason || s.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
    return s;
}

std::optional<AnalysisSpan> evidence(const json& value, long long fact_id, const AnalysisEvidence& facts) {
    const auto id = positive_id(value, "chunk_id");
    if (!id) return std::nullopt;
    const auto q = value.find("quote");
    if (q == value.end() || !q->is_string()) return std::nullopt;
    const std::string quote = q->get<std::string>();
    if (quote.empty() || quote.size() > kMaxQuote || quote.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
    const auto fact = facts.find(fact_id);
    if (fact == facts.end()) return std::nullopt;
    const auto chunk = fact->second.find(*id);
    if (chunk == fact->second.end()) return std::nullopt;
    const auto start = chunk->second.find(quote);
    if (start == std::string::npos || chunk->second.find(quote, start + 1) != std::string::npos) return std::nullopt;
    return AnalysisSpan{*id, start, start + quote.size()};
}

}  // namespace

std::optional<AnalysisProposals> parse_analysis_proposals(std::string_view text, const AnalysisEvidence& facts,
                                                         const std::set<long long>& category_ids) {
    if (text.size() > kMaxResponse) return std::nullopt;
    const auto parsed = parse_json(text);
    if (!parsed || !parsed->is_object()) return std::nullopt;
    const auto cats = parsed->find("categories"), conflicts = parsed->find("conflicts");
    if (cats == parsed->end() || !cats->is_array() || conflicts == parsed->end() || !conflicts->is_array() ||
        cats->size() > kMaxProposals || conflicts->size() > kMaxProposals) return std::nullopt;
    AnalysisProposals out;
    std::set<std::pair<long long, long long>> seen_categories, seen_conflicts;
    for (const auto& c : *cats) {
        if (!c.is_object()) return std::nullopt;
        const auto fact = positive_id(c, "fact_id"), category = positive_id(c, "category_id");
        const auto why = reason(c);
        const auto span = c.find("evidence");
        if (!fact || !category || !facts.count(*fact) || !category_ids.count(*category) || !why || span == c.end() ||
            !seen_categories.emplace(*fact, *category).second) return std::nullopt;
        const auto e = evidence(*span, *fact, facts);
        if (!e) return std::nullopt;
        out.categories.push_back({*fact, *category, *why, *e});
    }
    for (const auto& c : *conflicts) {
        if (!c.is_object()) return std::nullopt;
        const auto left = positive_id(c, "left_id"), right = positive_id(c, "right_id");
        const auto why = reason(c);
        const auto le = c.find("left_evidence"), re = c.find("right_evidence");
        if (!left || !right || *left == *right || !facts.count(*left) || !facts.count(*right) || !why || le == c.end() ||
            re == c.end() || !seen_conflicts.emplace(std::min(*left, *right), std::max(*left, *right)).second) return std::nullopt;
        const auto a = evidence(*le, *left, facts), b = evidence(*re, *right, facts);
        if (!a || !b) return std::nullopt;
        out.conflicts.push_back({*left, *right, *why, *a, *b});
    }
    return out;
}

}  // namespace wb::knowledge
