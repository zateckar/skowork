#include "scheduler/cron.h"

#include <charconv>
#include <vector>

namespace wb::scheduler {

long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

LocalTime civil_from_days(long long z) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    LocalTime t;
    t.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    t.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    t.year = static_cast<int>(static_cast<long long>(yoe) + era * 400 + (t.month <= 2));
    const long long w = (z - 719468 + 4) % 7;  // 1970-01-01 was a Thursday
    t.dow = static_cast<int>(w < 0 ? w + 7 : w);
    return t;
}

namespace {

constexpr long long kMinute = 60'000, kDay = 86'400'000;

long long floor_div(long long a, long long b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

}  // namespace

LocalTime FixedZone::local(long long utc_ms) const {
    const long long ms = utc_ms + offset_ * kMinute;
    const long long days = floor_div(ms, kDay);
    LocalTime t = civil_from_days(days);
    const long long in_day = ms - days * kDay;
    t.hour = static_cast<int>(in_day / 3'600'000);
    t.minute = static_cast<int>(in_day / kMinute % 60);
    t.second = static_cast<int>(in_day / 1000 % 60);
    return t;
}

std::optional<long long> FixedZone::utc(const LocalTime& t) const {
    return days_from_civil(t.year, t.month, t.day) * kDay + t.hour * 3'600'000LL + t.minute * kMinute + t.second * 1000LL -
           offset_ * kMinute;
}

namespace {

template <size_t N>
Status parse_field(std::string_view f, int lo, int hi, std::bitset<N>& out, bool& any, const char* name) {
    out.reset();
    any = f == "*" || f == "?";
    auto num = [&](std::string_view s, int& v) {
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        return r.ec == std::errc() && r.ptr == s.data() + s.size();
    };
    size_t start = 0;
    while (start <= f.size()) {
        size_t comma = f.find(',', start);
        if (comma == std::string_view::npos) comma = f.size();
        std::string_view part = f.substr(start, comma - start);
        if (part.empty()) return make_error("invalid_cron", std::string("empty item in the ") + name + " field");
        int step = 1;
        if (const size_t slash = part.find('/'); slash != std::string_view::npos) {
            if (!num(part.substr(slash + 1), step) || step < 1) return make_error("invalid_cron", std::string("bad step in the ") + name + " field");
            part = part.substr(0, slash);
        }
        int a = lo, b = hi;
        if (part != "*" && part != "?") {
            if (const size_t dash = part.find('-'); dash != std::string_view::npos) {
                if (!num(part.substr(0, dash), a) || !num(part.substr(dash + 1), b))
                    return make_error("invalid_cron", std::string("bad range in the ") + name + " field");
            } else {
                if (!num(part, a)) return make_error("invalid_cron", std::string("bad value in the ") + name + " field");
                b = step > 1 ? hi : a;  // "5/15" means from 5 every 15
            }
        }
        if (a < lo || b > hi || a > b)
            return make_error("invalid_cron", std::string("the ") + name + " field takes " + std::to_string(lo) + "-" + std::to_string(hi));
        for (int v = a; v <= b; v += step) out.set(static_cast<size_t>(v));
        start = comma + 1;
    }
    return {};
}

}  // namespace

Result<Cron> parse_cron(std::string_view text) {
    std::vector<std::string_view> f;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const size_t s = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (i > s) f.push_back(text.substr(s, i - s));
    }
    if (f.size() != 5) return make_error("invalid_cron", "a cron expression has five fields: minute hour day month weekday");
    Cron c;
    c.text = std::string(text);
    bool unused = false;
    if (auto s = parse_field(f[0], 0, 59, c.minute, unused, "minute"); !s) return s.error();
    if (auto s = parse_field(f[1], 0, 23, c.hour, unused, "hour"); !s) return s.error();
    if (auto s = parse_field(f[2], 1, 31, c.dom, c.dom_any, "day"); !s) return s.error();
    if (auto s = parse_field(f[3], 1, 12, c.month, unused, "month"); !s) return s.error();
    std::bitset<8> dow8;
    if (auto s = parse_field(f[4], 0, 7, dow8, c.dow_any, "weekday"); !s) return s.error();
    for (size_t d = 0; d < 7; ++d) c.dow[d] = dow8[d];
    if (dow8[7]) c.dow[0] = true;
    return c;
}

namespace {

bool day_matches(const Cron& c, const LocalTime& d) {
    if (!c.month[static_cast<size_t>(d.month)]) return false;
    const bool dom = c.dom[static_cast<size_t>(d.day)], dow = c.dow[static_cast<size_t>(d.dow)];
    if (c.dom_any && c.dow_any) return true;
    if (c.dom_any) return dow;
    if (c.dow_any) return dom;
    return dom || dow;
}

}  // namespace

std::optional<long long> next_fire(const Cron& c, const TimeZone& tz, long long after_ms) {
    const LocalTime start = tz.local(after_ms);
    // From the day before (a local time early on the next day can still be after `after` across a DST change).
    const long long first = days_from_civil(start.year, start.month, start.day) - 1;
    // Wall-clock times more than a day's worst offset change (3 h) before the start cannot come after it.
    const long long start_wall = (first + 1) * kDay + start.hour * 3'600'000LL + start.minute * kMinute - 3 * 3'600'000LL;
    for (long long day = first; day < first + 366 * 8 + 2; ++day) {
        LocalTime d = civil_from_days(day);
        if (!day_matches(c, d)) continue;
        for (int h = 0; h < 24; ++h) {
            if (!c.hour[static_cast<size_t>(h)]) continue;
            for (int m = 0; m < 60; ++m) {
                if (!c.minute[static_cast<size_t>(m)]) continue;
                if (day * kDay + h * 3'600'000LL + m * kMinute < start_wall) continue;
                d.hour = h;
                d.minute = m;
                d.second = 0;
                const auto u = tz.utc(d);
                if (u && *u > after_ms) return u;
            }
        }
    }
    return std::nullopt;
}

std::optional<long long> shortest_gap_ms(const Cron& c, const TimeZone& tz, long long from_ms, int samples) {
    std::optional<long long> prev, best;
    long long at = from_ms;
    for (int i = 0; i < samples; ++i) {
        const auto n = next_fire(c, tz, at);
        if (!n) break;
        if (prev && (!best || *n - *prev < *best)) best = *n - *prev;
        prev = n;
        at = *n;
    }
    return best;
}

}  // namespace wb::scheduler
