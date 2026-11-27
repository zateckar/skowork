#include "shell/palette.h"

#include <algorithm>
#include <string_view>
#include <tuple>

namespace wb::shell {

namespace {

// The base letter of an accented Latin letter, or the character itself.
char16_t base_letter(char16_t c) {
    if (c >= u'A' && c <= u'Z') return static_cast<char16_t>(c - u'A' + u'a');
    if (c < 0xC0 || c > 0x17F) return c;
    // Latin-1 Supplement U+00C0..U+00FF and Latin Extended-A U+0100..U+017F, from their Unicode decompositions;
    // '*' marks a character that is not a letter (the multiplication and division signs).
    static constexpr std::u16string_view table =
        u"aaaaaaaceeeeiiiidnooooo*ouuuuytsaaaaaaaceeeeiiiidnooooo*ouuuuyty"
        u"aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
    const char16_t b = table[static_cast<size_t>(c - 0xC0)];
    return b == u'*' ? c : b;
}

bool word_start(const std::u16string& s, size_t i) {
    if (i == 0) return true;
    const char16_t p = s[i - 1];
    return p == u' ' || p == u'-' || p == u'_' || p == u'/' || p == u'.' || p == u':' || p == u'(' || p == u'"';
}

// 0: prefix, 1: a word starts with the query, 2: the query's characters in order; -1: no match.
int rank_of(const std::u16string& name, const std::u16string& q) {
    if (q.empty()) return 0;
    if (name.starts_with(q)) return 0;
    for (size_t at = name.find(q); at != std::u16string::npos; at = name.find(q, at + 1))
        if (word_start(name, at)) return 1;
    size_t j = 0;
    for (size_t i = 0; i < name.size() && j < q.size(); ++i)
        if (name[i] == q[j]) ++j;
    return j == q.size() ? 2 : -1;
}

}  // namespace

std::u16string fold_for_match(std::u16string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (char16_t c : s) out += base_letter(c);
    return out;
}

std::vector<size_t> rank_palette(const std::vector<PaletteEntry>& entries, std::u16string_view query, size_t limit) {
    std::u16string q = fold_for_match(query);
    while (!q.empty() && q.front() == u' ') q.erase(q.begin());
    while (!q.empty() && q.back() == u' ') q.pop_back();
    std::vector<std::tuple<int, int, size_t, size_t>> hits;  // rank, entry kind, name length, index
    for (size_t i = 0; i < entries.size(); ++i) {
        const int r = rank_of(fold_for_match(entries[i].name), q);
        if (r < 0) continue;
        if (q.empty()) hits.emplace_back(0, 0, 0, i);  // as listed
        else hits.emplace_back(r, entries[i].command ? 0 : 1, entries[i].name.size(), i);
    }
    std::stable_sort(hits.begin(), hits.end());
    std::vector<size_t> out;
    for (const auto& h : hits) {
        if (out.size() >= limit) break;
        out.push_back(std::get<3>(h));
    }
    return out;
}

}  // namespace wb::shell
