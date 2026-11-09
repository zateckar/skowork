// Bounded, iterative support-set reconciliation for source-change previews.
#pragma once

#include <optional>
#include <unordered_set>
#include <vector>

namespace wb::knowledge {

struct ImpactSupport {
    bool active = true;
    std::vector<long long> evidence;
    std::vector<long long> required_facts;
};

struct ImpactFact {
    long long id = 0;
    std::vector<ImpactSupport> supports;
};

struct FactImpact {
    long long id = 0;
    int before = 0, after = 0;  // independently sufficient support sets
};

// Every required fact and evidence id must be present in the supplied snapshot. Returns nullopt for cycles,
// duplicates, dangling references or resource-limit violations; a partial graph must never be treated as a full diff.
std::optional<std::vector<FactImpact>> reconcile_impact(const std::vector<ImpactFact>& facts,
                                                       const std::unordered_set<long long>& evidence,
                                                       const std::unordered_set<long long>& removed_evidence);

}  // namespace wb::knowledge
