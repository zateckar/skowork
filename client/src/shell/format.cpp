#include "shell/format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

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

std::string format_call_time(long long ms, char decimal) {
    ms = std::max(0LL, ms);
    if (ms >= 9950) return format_elapsed(ms);
    const long long tenths = (ms + 50) / 100;
    return std::to_string(tenths / 10) + decimal + std::to_string(tenths % 10) + " s";
}

std::vector<int> whole_shares(const std::vector<long long>& parts, long long capacity) {
    if (capacity <= 0) return {};
    std::vector<long long> v;
    long long used = 0;
    for (long long p : parts) {
        v.push_back(std::max(0LL, p));
        used += v.back();
    }
    const long long total = std::max(capacity, used);
    v.push_back(total - used);  // free
    std::vector<int> out(v.size());
    std::vector<std::pair<long long, size_t>> rest;
    int sum = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        out[i] = static_cast<int>(v[i] * 100 / total);
        sum += out[i];
        rest.push_back({v[i] * 100 % total, i});
    }
    std::stable_sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t k = 0; sum < 100 && k < rest.size(); ++k, ++sum) ++out[rest[k].second];
    return out;
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
    return "Skowork " + field("version") + "\nChannel: " + field("channel") + "\nCommit: " + field("commit") +
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
            if (const auto n = json_int(v); n && *n > 0) entries.push_back({name, *n, 0});
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

ScheduleWords describe_schedule(std::string_view cron) {
    std::vector<std::string> f;
    std::string cur;
    for (const char c : cron) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) f.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) f.push_back(cur);
    ScheduleWords out;
    if (f.size() != 5) return out;
    const auto number = [](const std::string& v, int lo, int hi) {
        if (v.empty() || v.size() > 2 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; })) return -1;
        const int n = std::stoi(v);
        return n >= lo && n <= hi ? n : -1;
    };
    const int minute = number(f[0], 0, 59);
    if (minute < 0 || f[2] != "*" || f[3] != "*") return out;
    if (f[1] == "*") {
        if (f[4] == "*") out = {"hourly", -1, minute, -1};
        return out;
    }
    const int hour = number(f[1], 0, 23);
    if (hour < 0) return out;
    std::string dow = f[4];
    std::transform(dow.begin(), dow.end(), dow.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (dow == "*") return {"daily", hour, minute, -1};
    if (dow == "1-5" || dow == "MON-FRI") return {"workdays", hour, minute, -1};
    static const char* const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    for (int d = 0; d < 7; ++d)
        if (dow == kDays[d]) return {"weekly", hour, minute, d};
    if (const int d = number(dow, 0, 7); d >= 0) return {"weekly", hour, minute, d % 7};
    return out;
}

}  // namespace wb::shell
