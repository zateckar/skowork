#include "base/time.h"

#include <cstdio>
#include <ctime>

namespace wb {

const Clock& system_clock() {
    static const SystemClock clock;
    return clock;
}

std::string iso8601_utc(sys_clock::time_point t) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

std::string iso8601_utc_now() { return iso8601_utc(sys_clock::now()); }

long long unix_millis_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys_clock::now().time_since_epoch()).count();
}

namespace {

long long days_from_civil(long long y, long long m, long long d) {
    y -= m <= 2 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool digits(std::string_view s, size_t at, size_t n, long long& out) {
    if (at + n > s.size()) return false;
    out = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        out = out * 10 + (s[i] - '0');
    }
    return true;
}

}  // namespace

std::optional<long long> parse_iso8601_ms(std::string_view s) {
    long long y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, ms = 0;
    if (!digits(s, 0, 4, y) || s.size() < 10 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' || !digits(s, 8, 2, d))
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    // Only years sys_clock (nanoseconds since 1970) can hold; Graph writes 0001-01-01 for "no date".
    if (y < 1700 || y > 2200) return std::nullopt;
    size_t i = 10;
    long long offset_min = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        if (!digits(s, i + 1, 2, h) || i + 3 >= s.size() || s[i + 3] != ':' || !digits(s, i + 4, 2, mi)) return std::nullopt;
        i += 6;
        if (i < s.size() && s[i] == ':') {
            if (!digits(s, i + 1, 2, sec)) return std::nullopt;
            i += 3;
            if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                ++i;
                long long scale = 100;
                const size_t start = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    ms += (s[i] - '0') * scale;
                    scale /= 10;
                    ++i;
                }
                if (i == start) return std::nullopt;
            }
        }
        if (h > 23 || mi > 59 || sec > 60) return std::nullopt;
        if (i < s.size()) {
            if (s[i] == 'Z' || s[i] == 'z') {
                ++i;
            } else if (s[i] == '+' || s[i] == '-') {
                long long oh = 0, om = 0;
                if (!digits(s, i + 1, 2, oh)) return std::nullopt;
                size_t j = i + 3;
                if (j < s.size() && s[j] == ':') ++j;
                if (!digits(s, j, 2, om)) return std::nullopt;
                offset_min = (s[i] == '-' ? -1 : 1) * (oh * 60 + om);
                i = j + 2;
            }
        }
    }
    if (i != s.size()) return std::nullopt;
    const long long days = days_from_civil(y, mo, d);
    return ((days * 86400 + h * 3600 + mi * 60 + sec) - offset_min * 60) * 1000 + ms;
}

std::string iso8601_graph(long long unix_ms) {
    const auto t = sys_clock::time_point(std::chrono::milliseconds(unix_ms));
    std::string s = iso8601_utc(t);  // ...SS.mmmZ
    s.pop_back();
    return s + "0000";
}

}  // namespace wb
