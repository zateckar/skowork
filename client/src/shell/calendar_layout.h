// The Calendar page's time grid (client/calendar-view Calendar Time Grid; improve-threads-calendar-agenda D4): where
// the events of one day go side by side, and which hours the grid shows. Portable, so it is unit-tested everywhere.
#pragma once

#include <utility>
#include <vector>

namespace wb::shell {

// A timed event of one day, in minutes from that day's midnight (end > start; an event across midnight is cut to the
// day by the caller).
struct DaySpan {
    int start = 0, end = 0;
};

// An event's place in its day: lane `lane` of `lanes` equal lanes. Events that overlap, directly or through others,
// share one cluster and one lane count; each takes the first lane free at its start. Back-to-back events do not
// overlap. The result is in the order of `events`.
struct LanePlacement {
    int lane = 0, lanes = 1;
    bool operator==(const LanePlacement&) const = default;
};
std::vector<LanePlacement> lay_out_lanes(const std::vector<DaySpan>& events);

// The whole hours the grid shows: the working hours 08:00-18:00, extended to the hour before the earliest start and
// after the latest end of the shown events, within 0-24.
std::pair<int, int> grid_hours(const std::vector<DaySpan>& events, int first = 8, int last = 18);

}  // namespace wb::shell
