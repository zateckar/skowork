#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace wb {

using sys_clock = std::chrono::system_clock;

// Clock indirection so token freshness, heartbeats and timers can be tested without waiting.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual sys_clock::time_point now() const = 0;
    [[nodiscard]] long long unix_seconds() const {
        return std::chrono::duration_cast<std::chrono::seconds>(now().time_since_epoch()).count();
    }
    [[nodiscard]] long long unix_millis() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now().time_since_epoch()).count();
    }
};

class SystemClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override { return sys_clock::now(); }
};

const Clock& system_clock();

std::string iso8601_utc(sys_clock::time_point t);  // 2026-10-22T08:15:30.123Z
std::string iso8601_utc_now();
long long unix_millis_now();
// Unix milliseconds of an ISO-8601 date-time: "2026-10-23T09:12:00Z", fractions of any length, a "+01:00" offset, or no
// zone at all (read as UTC, as Graph's dateTime with timeZone "UTC"). A plain date is midnight UTC. Years 1700-2200 only.
std::optional<long long> parse_iso8601_ms(std::string_view text);
// UTC date-time without a zone suffix and with 7 fraction digits, as Graph's dateTimeTimeZone wants.
std::string iso8601_graph(long long unix_ms);

}  // namespace wb
