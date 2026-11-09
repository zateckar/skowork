#include "knowledge/reconcile.h"

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <iostream>
#include <random>

using namespace wb::knowledge;

TEST(KnowledgeImpact, IndependentAndConjunctiveProofsPropagateOnlyWhenNeeded) {
    const std::vector<ImpactFact> facts = {
        {1, {{true, {11}, {}}}},
        {2, {{true, {12}, {}}}},
        {3, {{true, {}, {1, 2}}, {true, {13}, {}}}},
        {4, {{true, {}, {3}}}},
        {5, {{false, {14}, {}}}},
    };
    const std::unordered_set<long long> evidence = {11, 12, 13, 14};
    auto partial = reconcile_impact(facts, evidence, {11});
    ASSERT_TRUE(partial);
    ASSERT_EQ(partial->size(), 5u);
    EXPECT_EQ((*partial)[0].before, 1);
    EXPECT_EQ((*partial)[0].after, 0);
    EXPECT_EQ((*partial)[2].before, 2);
    EXPECT_EQ((*partial)[2].after, 1);
    EXPECT_EQ((*partial)[3].after, 1);
    EXPECT_EQ((*partial)[4].before, 0);
    auto all = reconcile_impact(facts, evidence, {11, 13});
    ASSERT_TRUE(all);
    EXPECT_EQ((*all)[2].after, 0);
    EXPECT_EQ((*all)[3].after, 0);
    EXPECT_EQ((*all)[1].after, 1);
}

TEST(KnowledgeImpact, RejectsIncompleteDuplicateAndCyclicSnapshots) {
    const std::unordered_set<long long> evidence = {1};
    EXPECT_FALSE(reconcile_impact({{1, {{true, {2}, {}}}}}, evidence, {}));
    EXPECT_FALSE(reconcile_impact({{1, {{true, {1, 1}, {}}}}}, evidence, {}));
    EXPECT_FALSE(reconcile_impact({{1, {{true, {}, {2}}}}}, evidence, {}));
    EXPECT_FALSE(reconcile_impact({{1, {{true, {}, {2}}}}, {2, {{true, {}, {1}}}}}, evidence, {}));
    EXPECT_FALSE(reconcile_impact({{1, {{true, {}, {}}}}}, evidence, {}));
    EXPECT_FALSE(reconcile_impact({{1, {{true, {1}, {}}}}, {1, {{true, {1}, {}}}}}, evidence, {}));
    EXPECT_FALSE(reconcile_impact({{1, {{true, {1}, {}}}}}, evidence, {2}));
}

TEST(KnowledgeImpact, HandlesOneHundredThousandFactDependencyChainWithoutRecursion) {
    constexpr int count = 100'000;
    std::vector<ImpactFact> facts;
    facts.reserve(count);
    facts.push_back({1, {{true, {1}, {}}}});
    for (int i = 2; i <= count; ++i) facts.push_back({i, {{true, {}, {i - 1}}}});
    const auto started = std::chrono::steady_clock::now();
    const auto diff = reconcile_impact(facts, {1}, {1});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    ASSERT_TRUE(diff);
    ASSERT_EQ(diff->size(), count);
    EXPECT_EQ(diff->front().before, 1);
    EXPECT_EQ(diff->front().after, 0);
    EXPECT_EQ(diff->back().before, 1);
    EXPECT_EQ(diff->back().after, 0);
    std::cout << "[ knowledge impact ] 100000-fact dependency chain: " << elapsed.count() << " ms\n";
}

TEST(KnowledgeImpact, MatchesIndependentEvaluationAcrossShuffledSmallGraphs) {
    std::mt19937 random(42);
    for (int trial = 0; trial < 1000; ++trial) {
        const int count = 1 + random() % 12;
        std::vector<ImpactFact> facts;
        std::vector<int> before(count), after(count);
        const std::unordered_set<long long> evidence = {1, 2, 3, 4};
        std::unordered_set<long long> removed;
        for (long long e = 1; e <= 4; ++e) if (random() % 2) removed.insert(e);
        for (int i = 0; i < count; ++i) {
            ImpactFact fact{static_cast<long long>(i + 1), {}};
            const int proofs = 1 + random() % 3;
            for (int j = 0; j < proofs; ++j) {
                ImpactSupport support{random() % 4 != 0, {1 + static_cast<long long>(random() % 4)}, {}};
                if (i && random() % 2) support.required_facts.push_back(1 + random() % i);
                bool was = support.active, remains = support.active && !removed.contains(support.evidence[0]);
                for (const auto requirement : support.required_facts) {
                    was = was && before[static_cast<size_t>(requirement - 1)] > 0;
                    remains = remains && after[static_cast<size_t>(requirement - 1)] > 0;
                }
                before[i] += was ? 1 : 0;
                after[i] += remains ? 1 : 0;
                fact.supports.push_back(std::move(support));
            }
            facts.push_back(std::move(fact));
        }
        std::shuffle(facts.begin(), facts.end(), random);
        const auto actual = reconcile_impact(facts, evidence, removed);
        ASSERT_TRUE(actual);
        for (const auto& fact : *actual) {
            EXPECT_EQ(fact.before, before[static_cast<size_t>(fact.id - 1)]);
            EXPECT_EQ(fact.after, after[static_cast<size_t>(fact.id - 1)]);
        }
    }
}
