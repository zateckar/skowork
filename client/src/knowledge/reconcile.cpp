#include "knowledge/reconcile.h"

#include <queue>
#include <unordered_map>

namespace wb::knowledge {

std::optional<std::vector<FactImpact>> reconcile_impact(const std::vector<ImpactFact>& facts,
                                                       const std::unordered_set<long long>& evidence,
                                                       const std::unordered_set<long long>& removed_evidence) {
    constexpr size_t kMaxFacts = 100'000, kMaxSupports = 400'000, kMaxInputs = 2'000'000;
    if (facts.size() > kMaxFacts || evidence.size() > kMaxInputs || removed_evidence.size() > evidence.size())
        return std::nullopt;
    for (const auto id : evidence) if (id <= 0) return std::nullopt;
    for (const auto id : removed_evidence) if (!evidence.contains(id)) return std::nullopt;
    std::unordered_map<long long, size_t> index;
    index.reserve(facts.size());
    for (size_t i = 0; i < facts.size(); ++i)
        if (facts[i].id <= 0 || !index.emplace(facts[i].id, i).second) return std::nullopt;
    std::vector<size_t> pending(facts.size(), 0);
    std::vector<std::vector<size_t>> dependents(facts.size());
    size_t support_count = 0, input_count = 0;
    for (size_t i = 0; i < facts.size(); ++i) {
        if (facts[i].supports.size() > kMaxSupports - support_count) return std::nullopt;
        support_count += facts[i].supports.size();
        for (const auto& support : facts[i].supports) {
            if (support.evidence.empty() && support.required_facts.empty()) return std::nullopt;
            if (support.evidence.size() > kMaxInputs - input_count) return std::nullopt;
            input_count += support.evidence.size();
            if (support.required_facts.size() > kMaxInputs - input_count) return std::nullopt;
            input_count += support.required_facts.size();
            std::unordered_set<long long> seen_evidence, seen_facts;
            for (const auto id : support.evidence)
                if (!evidence.contains(id) || !seen_evidence.insert(id).second) return std::nullopt;
            for (const auto id : support.required_facts) {
                const auto it = index.find(id);
                if (it == index.end() || !seen_facts.insert(id).second) return std::nullopt;
                ++pending[i];
                dependents[it->second].push_back(i);
            }
        }
    }
    std::queue<size_t> ready;
    for (size_t i = 0; i < facts.size(); ++i) if (!pending[i]) ready.push(i);
    std::vector<FactImpact> result;
    result.reserve(facts.size());
    std::vector<int> before(facts.size()), after(facts.size());
    size_t visited = 0;
    while (!ready.empty()) {
        const auto i = ready.front();
        ready.pop();
        ++visited;
        for (const auto& support : facts[i].supports) {
            bool was = support.active, remains = support.active;
            for (const auto id : support.evidence) if (removed_evidence.contains(id)) remains = false;
            for (const auto id : support.required_facts) {
                const auto required = index.at(id);
                was = was && before[required] > 0;
                remains = remains && after[required] > 0;
            }
            before[i] += was ? 1 : 0;
            after[i] += remains ? 1 : 0;
        }
        for (const auto next : dependents[i]) if (--pending[next] == 0) ready.push(next);
    }
    if (visited != facts.size()) return std::nullopt;
    for (size_t i = 0; i < facts.size(); ++i) result.push_back({facts[i].id, before[i], after[i]});
    return result;
}

}  // namespace wb::knowledge
