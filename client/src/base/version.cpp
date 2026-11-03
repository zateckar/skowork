#include "base/version.h"

#include "base/strings.h"

#include <charconv>

namespace wb {
namespace {

bool parse_int(std::string_view s, int& out) {
    if (s.empty()) return false;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size() && out >= 0;
}

bool numeric(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

std::optional<Version> parse_version(std::string_view s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    if (const auto plus = s.find('+'); plus != std::string_view::npos) s = s.substr(0, plus);
    Version v;
    if (const auto dash = s.find('-'); dash != std::string_view::npos) {
        v.pre = std::string(s.substr(dash + 1));
        s = s.substr(0, dash);
    }
    const auto parts = split(s, '.');
    if (parts.empty() || parts.size() > 3) return std::nullopt;
    if (!parse_int(parts[0], v.major)) return std::nullopt;
    if (parts.size() > 1 && !parse_int(parts[1], v.minor)) return std::nullopt;
    if (parts.size() > 2 && !parse_int(parts[2], v.patch)) return std::nullopt;
    return v;
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;  // a release ranks above its pre-releases
    if (b.pre.empty()) return -1;
    const auto pa = split(a.pre, '.'), pb = split(b.pre, '.');
    for (size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
        if (pa[i] == pb[i]) continue;
        const bool na = numeric(pa[i]), nb = numeric(pb[i]);
        if (na && nb) {
            int x = 0, y = 0;
            parse_int(pa[i], x);
            parse_int(pb[i], y);
            return x < y ? -1 : 1;
        }
        if (na != nb) return na ? -1 : 1;  // numeric identifiers rank lower
        return pa[i] < pb[i] ? -1 : 1;
    }
    if (pa.size() == pb.size()) return 0;
    return pa.size() < pb.size() ? -1 : 1;
}

bool below_minimum(std::string_view current, std::string_view minimum) {
    const auto m = parse_version(minimum);
    if (!m) return false;
    const auto c = parse_version(current);
    if (!c) return true;  // an unparsable own version is treated as outdated
    return compare_versions(*c, *m) < 0;
}

}  // namespace wb
