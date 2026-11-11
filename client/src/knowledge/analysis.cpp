#include "knowledge/analysis.h"

#include "base/json.h"
#include "base/strings.h"
#include "knowledge/core.h"

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
        std::string kind = "conflict";
        if (const auto k = c.find("kind"); k != c.end()) {
            if (!k->is_string()) return std::nullopt;
            kind = k->get<std::string>();
            if (kind != "conflict" && kind != "uncertain_entity") return std::nullopt;
        }
        const auto a = evidence(*le, *left, facts), b = evidence(*re, *right, facts);
        if (!a || !b) return std::nullopt;
        out.conflicts.push_back({*left, *right, *why, kind, *a, *b});
    }
    // Optional new category names, each resting on exact excerpts of the facts it would hold.
    if (const auto names = parsed->find("new_categories"); names != parsed->end()) {
        if (!names->is_array() || names->size() > kMaxNamesPerAnswer) return std::nullopt;
        std::set<std::string> seen_names;
        for (const auto& n : *names) {
            if (!n.is_object()) return std::nullopt;
            const auto nm = n.find("name");
            const auto why = reason(n);
            const auto fs = n.find("facts");
            if (nm == n.end() || !nm->is_string() || !why || fs == n.end() || !fs->is_array() || fs->empty() ||
                fs->size() > facts.size()) return std::nullopt;
            std::string display = nm->get<std::string>();
            const auto b = display.find_first_not_of(" \t");
            display = b == std::string::npos ? std::string() : display.substr(b, display.find_last_not_of(" \t") - b + 1);
            if (display.empty() || display.size() > kMaxCategoryNameBytes ||
                std::any_of(display.begin(), display.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20; }))
                return std::nullopt;
            const std::string key = category_name_key(display);
            if (key.empty() || !seen_names.insert(key).second) return std::nullopt;
            CategoryNameProposal proposal{display, *why, {}};
            std::set<long long> seen_facts;
            for (const auto& f : *fs) {
                const auto fact = positive_id(f, "fact_id");
                const auto span = f.is_object() ? f.find("evidence") : f.end();
                if (!fact || !facts.count(*fact) || span == f.end() || !seen_facts.insert(*fact).second) return std::nullopt;
                const auto e = evidence(*span, *fact, facts);
                if (!e) return std::nullopt;
                proposal.facts.push_back({*fact, *e});
            }
            out.names.push_back(std::move(proposal));
        }
    }
    return out;
}

std::string category_name_key(std::string_view name) {
    std::string out;
    for (std::string word : split(normalize_name(name), ' ')) {
        if (word.empty()) continue;
        if (word.size() > 4 && word.compare(word.size() - 3, 3, "ies") == 0) word.replace(word.size() - 3, 3, "y");
        else if (word.size() > 3 && word.back() == 's' && word[word.size() - 2] != 's') word.pop_back();
        if (!out.empty()) out.push_back(' ');
        out += word;
    }
    return out;
}

bool category_keys_near(std::string_view a, std::string_view b) {
    if (a == b) return true;
    const size_t shorter = std::min(a.size(), b.size()), longer = std::max(a.size(), b.size());
    if (shorter < 5) return false;
    const size_t allowed = longer >= 10 ? 2 : 1;
    if (longer - shorter > allowed) return false;
    // Edit distance, stopped as soon as every path exceeds the allowance.
    std::vector<size_t> row(b.size() + 1), next(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        next[0] = i;
        size_t best = next[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            next[j] = std::min({row[j] + 1, next[j - 1] + 1, row[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            best = std::min(best, next[j]);
        }
        if (best > allowed) return false;
        row.swap(next);
    }
    return row[b.size()] <= allowed;
}

}  // namespace wb::knowledge
