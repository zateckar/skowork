// The command palette's matching and ranking (client/work-inbox Command Palette; add-assistant-feedback-patterns
// design D3): portable, so it is unit-tested everywhere. The view is in views_palette.cpp.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wb::shell {

struct PaletteEntry {
    std::u16string name;
    bool command = false;  // a command ranks before an Inbox entry of the same rank
};

// Lower case without accents (Latin-1 Supplement and Latin Extended-A letters to ASCII), for matching.
std::u16string fold_for_match(std::u16string_view s);

// The entries that match `query`, best first, at most `limit`: a name starting with the query, then a name with a word
// starting with it, then a name containing its characters in order; at equal rank commands first, then the shorter
// name, then the original order. An empty query lists the entries in their order.
std::vector<size_t> rank_palette(const std::vector<PaletteEntry>& entries, std::u16string_view query, size_t limit = 50);

}  // namespace wb::shell
