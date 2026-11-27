#include "shell/calendar_layout.h"

#include <algorithm>
#include <numeric>

namespace wb::shell {

std::vector<LanePlacement> lay_out_lanes(const std::vector<DaySpan>& events) {
    std::vector<LanePlacement> out(events.size());
    std::vector<size_t> order(events.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (events[a].start != events[b].start) return events[a].start < events[b].start;
        return events[a].end > events[b].end;  // the longer one first, so it gets the left lane
    });
    std::vector<size_t> cluster;  // indices of the current cluster
    std::vector<int> lane_end;    // per lane: the end of its last event
    int cluster_end = -1;
    const auto close = [&] {
        const int lanes = static_cast<int>(lane_end.size());
        for (size_t i : cluster) out[i].lanes = std::max(1, lanes);
        cluster.clear();
        lane_end.clear();
    };
    for (size_t i : order) {
        const DaySpan& e = events[i];
        if (!cluster.empty() && e.start >= cluster_end) close();
        int lane = -1;
        for (size_t l = 0; l < lane_end.size(); ++l)
            if (lane_end[l] <= e.start) {
                lane = static_cast<int>(l);
                break;
            }
        if (lane < 0) {
            lane = static_cast<int>(lane_end.size());
            lane_end.push_back(0);
        }
        lane_end[static_cast<size_t>(lane)] = std::max(e.end, e.start + 1);
        out[i].lane = lane;
        cluster_end = cluster.empty() ? std::max(e.end, e.start + 1) : std::max(cluster_end, std::max(e.end, e.start + 1));
        cluster.push_back(i);
    }
    close();
    return out;
}

std::pair<int, int> grid_hours(const std::vector<DaySpan>& events, int first, int last) {
    for (const auto& e : events) {
        first = std::min(first, e.start / 60);
        last = std::max(last, (e.end + 59) / 60);
    }
    return {std::clamp(first, 0, 24), std::clamp(last, 0, 24)};
}

}  // namespace wb::shell
