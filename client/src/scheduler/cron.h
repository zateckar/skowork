// Cron expressions evaluated in a Trigger's time zone (client/scheduler Trigger Kinds; add-scheduled-and-remote-tasks
// design D2, D9). Portable: the time zone comes in through TimeZone, which the Windows build implements over icu.dll.
#pragma once

#include "base/result.h"

#include <bitset>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace wb::scheduler {

// A wall-clock time in a time zone. month 1-12, day 1-31, dow 0 (Sunday) - 6.
struct LocalTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    int dow = 4;
};

class TimeZone {
public:
    virtual ~TimeZone() = default;
    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual LocalTime local(long long utc_ms) const = 0;
    // The first instant at which the zone shows this wall-clock time: nullopt when it does not exist (a DST gap); of a
    // time shown twice (a DST overlap) only the first instance, so it fires once.
    [[nodiscard]] virtual std::optional<long long> utc(const LocalTime& t) const = 0;
};

// A fixed offset from UTC (tests and the "UTC" zone).
class FixedZone final : public TimeZone {
public:
    explicit FixedZone(int offset_minutes = 0, std::string id = "UTC") : offset_(offset_minutes), id_(std::move(id)) {}
    [[nodiscard]] std::string id() const override { return id_; }
    [[nodiscard]] LocalTime local(long long utc_ms) const override;
    [[nodiscard]] std::optional<long long> utc(const LocalTime& t) const override;

private:
    int offset_;
    std::string id_;
};

// minute hour day-of-month month day-of-week, each a list of values, ranges (a-b), steps (*/n, a-b/n) or *. Day of week
// 0-7 (0 and 7 are Sunday). When both day fields are restricted, a day matching either matches (as in cron).
struct Cron {
    std::bitset<60> minute;
    std::bitset<24> hour;
    std::bitset<32> dom;
    std::bitset<13> month;
    std::bitset<7> dow;
    bool dom_any = true, dow_any = true;
    std::string text;
};

Result<Cron> parse_cron(std::string_view text);

// The first firing strictly after `after_ms`, or nullopt when none comes within eight years.
std::optional<long long> next_fire(const Cron& c, const TimeZone& tz, long long after_ms);

// The shortest time between consecutive firings over the next `samples` firings after `from_ms` (for
// policy.scheduler.min_interval_minutes); nullopt when it fires less than twice.
std::optional<long long> shortest_gap_ms(const Cron& c, const TimeZone& tz, long long from_ms, int samples = 400);

// Civil calendar helpers (proleptic Gregorian).
long long days_from_civil(int y, int m, int d);
LocalTime civil_from_days(long long days);

}  // namespace wb::scheduler
