#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wb {

std::vector<std::string> split(std::string_view s, char sep);
std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
// Lower-case ASCII with the Czech and German letters folded to their base letter, so "Uložit" matches "ulozit".
std::string fold_latin(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);

// Truncates UTF-8 text to at most max_bytes without splitting a code point.
std::string utf8_truncate(std::string_view s, size_t max_bytes);

// Constant-time comparison for secrets (state values, proxy credentials).
bool constant_time_equals(std::string_view a, std::string_view b);

}  // namespace wb
