#include "knowledge/analysis.h"
#include "base/json.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::knowledge;

namespace {
const AnalysisEvidence facts = {
    {1, {{10, "Projekt Phoenix začne 30. června."}}},
    {2, {{20, "Project Phoenix starts on 15 July."}}},
    {3, {{30, "Projekt Phoenix beginnt am 15. Juli."}}},
};

json valid() {
    return {{"categories", {{{"fact_id", 1}, {"category_id", 7}, {"reason", "Project schedule"},
                             {"evidence", {{"chunk_id", 10}, {"quote", "30. června"}}}}}},
            {"conflicts", {{{"left_id", 1}, {"right_id", 2}, {"reason", "Different proposed dates"},
                            {"left_evidence", {{"chunk_id", 10}, {"quote", "30. června"}}},
                            {"right_evidence", {{"chunk_id", 20}, {"quote", "15 July"}}}}}}};
}
}  // namespace

TEST(KnowledgeAnalysis, ResolvesCzechEnglishAndGermanEvidenceWithoutInventingSpans) {
    auto input = valid();
    input["categories"].push_back({{"fact_id", 3}, {"category_id", 7}, {"reason", "Projekttermin"},
                                   {"evidence", {{"chunk_id", 30}, {"quote", "15. Juli"}}}});
    const auto result = parse_analysis_proposals(input.dump(), facts, {7});
    ASSERT_TRUE(result);
    ASSERT_EQ(result->categories.size(), 2u);
    ASSERT_EQ(result->conflicts.size(), 1u);
    const auto& czech = result->conflicts[0].left;
    EXPECT_EQ(facts.at(1).at(czech.chunk_id).substr(czech.begin, czech.end - czech.begin), "30. června");
    const auto& german = result->categories[1].evidence;
    EXPECT_EQ(facts.at(3).at(german.chunk_id).substr(german.begin, german.end - german.begin), "15. Juli");
}

TEST(KnowledgeAnalysis, RefusesForeignMissingAmbiguousAndDuplicateEvidence) {
    auto proposal = valid();
    proposal["categories"][0]["fact_id"] = 999;
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    proposal = valid();
    proposal["categories"][0]["category_id"] = 8;
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    proposal = valid();
    proposal["categories"][0]["evidence"]["chunk_id"] = 20;  // belongs to a different fact
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    proposal = valid();
    proposal["categories"][0]["evidence"]["quote"] = "invented";
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    proposal = valid();
    auto repeated = facts;
    repeated[1][10] += " 30. června";
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), repeated, {7}));
    proposal = valid();
    proposal["categories"].push_back(proposal["categories"][0]);
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    proposal = valid();
    auto reverse = proposal["conflicts"][0];
    std::swap(reverse["left_id"], reverse["right_id"]);
    std::swap(reverse["left_evidence"], reverse["right_evidence"]);
    proposal["conflicts"].push_back(reverse);
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
}

TEST(KnowledgeAnalysis, RejectsMalformedOversizedAndOutOfRangeValues) {
    auto proposal = valid();
    proposal["conflicts"][0]["left_id"] = 1.0;
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    proposal = valid();
    proposal["categories"][0]["reason"] = std::string(513, 'x');
    EXPECT_FALSE(parse_analysis_proposals(proposal.dump(), facts, {7}));
    EXPECT_FALSE(parse_analysis_proposals(std::string(256 * 1024 + 1, 'x'), facts, {7}));
    EXPECT_FALSE(parse_analysis_proposals("{\"categories\":[],\"conflicts\":[", facts, {7}));
    EXPECT_TRUE(parse_analysis_proposals(R"({"categories":[],"conflicts":[]})", facts, {7}));
}
