#include "base/strings.h"

#include <cctype>
#include <utility>

namespace wb {

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const auto pos = s.find(sep, start);
        out.emplace_back(s.substr(start, pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string fold_latin(std::string_view s) {
    static const std::pair<std::string_view, char> map[] = {
        {"á", 'a'}, {"Á", 'a'}, {"ä", 'a'}, {"Ä", 'a'}, {"č", 'c'}, {"Č", 'c'}, {"ď", 'd'}, {"Ď", 'd'}, {"é", 'e'}, {"É", 'e'},
        {"ě", 'e'}, {"Ě", 'e'}, {"í", 'i'}, {"Í", 'i'}, {"ň", 'n'}, {"Ň", 'n'}, {"ó", 'o'}, {"Ó", 'o'}, {"ö", 'o'}, {"Ö", 'o'},
        {"ř", 'r'}, {"Ř", 'r'}, {"š", 's'}, {"Š", 's'}, {"ť", 't'}, {"Ť", 't'}, {"ú", 'u'}, {"Ú", 'u'}, {"ů", 'u'}, {"Ů", 'u'},
        {"ü", 'u'}, {"Ü", 'u'}, {"ý", 'y'}, {"Ý", 'y'}, {"ž", 'z'}, {"Ž", 'z'}, {"ß", 's'}, {"ẞ", 's'},
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
            ++i;
            continue;
        }
        bool hit = false;
        for (const auto& [from, to] : map) {
            if (s.substr(i, from.size()) == from) {
                out.push_back(to);
                i += from.size();
                hit = true;
                break;
            }
        }
        if (!hit) {
            out.push_back(static_cast<char>(c));
            ++i;
        }
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string utf8_truncate(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    size_t n = max_bytes;
    // Step back over continuation bytes (10xxxxxx) so the cut lands on a code point boundary.
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return std::string(s.substr(0, n));
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

}  // namespace wb
