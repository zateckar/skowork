#include "shell/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell/model.h"

namespace wb::shell {

std::string format_bytes(double bytes, char decimal) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    if (!std::isfinite(bytes) || bytes < 0) return "\xE2\x80\x94";
    double value = bytes;
    size_t unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    // Whole bytes need no decimals; 100+ needs none either; otherwise one decimal.
    const int digits = unit == 0 || value >= 100 ? 0 : 1;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f %s", digits, value, units[unit]);
    std::string out = buf;
    if (decimal != '.')
        if (const auto dot = out.find('.'); dot != std::string::npos) out[dot] = decimal;
    return out;
}

std::string format_duration(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) return "0 s";
    const auto s = static_cast<long long>(std::ceil(seconds));
    if (s < 60) return std::to_string(s) + " s";
    const long long total_minutes = (s + 59) / 60;
    const long long h = total_minutes / 60, m = total_minutes % 60;
    if (h == 0) return std::to_string(m) + " min";
    return m == 0 ? std::to_string(h) + " h" : std::to_string(h) + " h " + std::to_string(m) + " min";
}

std::string format_elapsed(long long ms) {
    const long long s = std::max(0LL, (ms + 500) / 1000);
    if (s < 60) return std::to_string(s) + " s";
    const long long m = s / 60;
    if (m < 60) return s % 60 ? std::to_string(m) + " min " + std::to_string(s % 60) + " s" : std::to_string(m) + " min";
    return m % 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m / 60) + " h";
}

std::u16string format_relative(const std::string& iso, long long now_ms, const ui::Strings& s,
                               const std::function<std::u16string(long long)>& date) {
    const auto at = iso_millis(iso);
    if (!at) return {};
    const long long secs = std::llabs(*at - now_ms) / 1000;
    if (secs < 60) return s.get("time.now");
    if (secs < 3600) return s.count("time.minutes_ago", (secs + 30) / 60);
    if (secs < 86400) return s.count("time.hours_ago", (secs + 1800) / 3600);
    if (secs < 7 * 86400) {
        const long long days = (secs + 43200) / 86400;
        return days <= 1 ? s.get("time.yesterday") : s.count("time.days_ago", days);
    }
    return date ? date(*at) : std::u16string{};
}

std::string format_percent(double fraction, char decimal) {
    if (!std::isfinite(fraction)) return "\xE2\x80\x94";
    const double pct = std::max(0.0, fraction) * 100;
    char buf[32];
    if (pct > 0 && pct < 10) std::snprintf(buf, sizeof buf, "%.1f%%", pct);
    else std::snprintf(buf, sizeof buf, "%.0f%%", pct);
    std::string s = buf;
    if (const auto dot = s.find('.'); dot != std::string::npos) s[dot] = decimal;
    return s;
}

std::vector<SnippetPart> snippet_parts(std::string_view snippet) {
    std::vector<SnippetPart> out;
    size_t at = 0;
    while (at < snippet.size()) {
        const size_t open = snippet.find("[[", at);
        const size_t close = open == std::string_view::npos ? open : snippet.find("]]", open + 2);
        if (close == std::string_view::npos) break;
        if (open > at) out.push_back({std::string(snippet.substr(at, open - at)), false});
        out.push_back({std::string(snippet.substr(open + 2, close - open - 2)), true});
        at = close + 2;
    }
    if (at < snippet.size()) out.push_back({std::string(snippet.substr(at)), false});
    return out;
}

std::string about_text(const json& v) {
    auto field = [&](const char* k) {
        const json* x = v.contains(k) ? &v[k] : nullptr;
        if (!x || x->is_null()) return std::string();
        return x->is_string() ? x->get<std::string>() : x->dump();
    };
    return "Workbench " + field("version") + "\nChannel: " + field("channel") + "\nCommit: " + field("commit") +
           "\nProcess ID: " + field("pid");
}

std::string short_commit(const std::string& commit, size_t length) {
    if (commit.empty()) return "\xE2\x80\x94";
    return commit.size() > length ? commit.substr(0, length) : commit;
}

std::string display_user(const json& user) {
    if (user.is_string()) {
        std::string s = user.get<std::string>();
        const auto b = s.find_first_not_of(" \t");
        return b == std::string::npos ? std::string{} : s.substr(b, s.find_last_not_of(" \t") - b + 1);
    }
    if (!user.is_object()) return {};
    std::string name = jstr(user, "display_name", jstr(user, "name"));
    std::string account = jstr(user, "email", jstr(user, "upn", jstr(user, "preferred_username")));
    if (!name.empty() && !account.empty() && name != account) return name + " (" + account + ")";
    if (!name.empty()) return name;
    if (!account.empty()) return account;
    return jstr(user, "sub");
}

std::vector<UsageSegment> usage_segments(const json& categories, double max, size_t max_segments) {
    std::vector<UsageSegment> entries;
    if (categories.is_object())
        for (const auto& [name, v] : categories.items())
            if (v.is_number() && v.get<double>() > 0) entries.push_back({name, v.get<long long>(), 0});
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
    if (max_segments > 0 && entries.size() > max_segments) {
        long long other = 0;
        for (size_t i = max_segments - 1; i < entries.size(); ++i) other += entries[i].bytes;
        entries.resize(max_segments - 1);
        entries.push_back({"Other", other, 0});
    }
    double denom = max;
    if (denom <= 0) {
        denom = 0;
        for (const auto& e : entries) denom += static_cast<double>(e.bytes);
        if (denom <= 0) denom = 1;
    }
    for (auto& e : entries) e.fraction = std::min(1.0, static_cast<double>(e.bytes) / denom);
    return entries;
}

}  // namespace wb::shell
