// Pins the exact text of every model instruction that evaluation replays can override (add-evaluation-export design
// D3, tasks 3.1). Moving instructions into the registry must not change a single byte of what live features send.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "agenda/core.h"
#include "knowledge/core.h"
#include "knowledge/knowledge_service.h"
#include "triage/classifier.h"

namespace {

std::string fixture(const std::string& name) {
    std::ifstream in(std::string(SKOWORK_SOURCE_DIR) + "/tests/fixtures/instructions/" + name, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // A Windows checkout may turn the fixture's line ends into CRLF; the instructions never contain a carriage return.
    std::string out;
    for (char c : text)
        if (c != '\r') out += c;
    return out;
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

}  // namespace

TEST(InstructionsGolden, TriageTextIsUnchanged) {
    EXPECT_EQ(wb::triage::classification_instructions(), fixture("triage.instructions.txt"));
}

TEST(InstructionsGolden, AgendaTextIsUnchanged) {
    EXPECT_EQ(wb::agenda::detection_instructions(), fixture("agenda.instructions.txt"));
    EXPECT_EQ(wb::agenda::merge_instructions(), fixture("agenda.merge_instructions.txt"));
}

TEST(InstructionsGolden, KnowledgeTextIsUnchanged) {
    std::string types;
    for (const auto& t : wb::knowledge::entity_types()) types += (types.empty() ? "" : ", ") + t;
    const std::string extraction = replace(replace(fixture("knowledge.extraction.txt"), "{types}", types), "{schema}",
                                           wb::knowledge::extraction_schema().dump());
    EXPECT_EQ(wb::knowledge::extraction_system_text(), extraction);
    EXPECT_EQ(wb::knowledge::score_instructions(), fixture("knowledge.score.txt"));
    EXPECT_EQ(wb::knowledge::resolve_instructions(), fixture("knowledge.resolve.txt"));
}
