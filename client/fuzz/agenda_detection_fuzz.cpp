#include "agenda/core.h"
#include "fuzz_main.h"

// add-agenda task 2.1: the detection parser takes untrusted model output; it either rejects it or returns candidates
// that respect the schema bounds, and admission never crashes on them.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    wb::agenda::SourceItem item;
    item.kind = "mail";
    item.id = "m1";
    item.text = "Hi Jana, please send me the Q4 slides by Friday.";
    item.user_addressed = true;
    item.at_ms = 1'795'000'000'000LL;
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto result = wb::agenda::parse_detection(text, {item});
    if (!result) return 0;
    if (result->size() > wb::agenda::kMaxCandidatesPerItem) __builtin_trap();
    for (const auto& c : *result) {
        if (c.source_id != "m1" || c.confidence < 0 || c.confidence > 1 || c.due.size() > 32) __builtin_trap();
        (void)wb::agenda::admit(c, item, {"Jana Nováková", {"jana@example.cz"}}, item.at_ms, 60, 0.7, nullptr);
    }
    (void)wb::agenda::parse_merge(text, {"a1"});
    return 0;
}
