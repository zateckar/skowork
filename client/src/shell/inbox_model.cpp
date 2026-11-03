#include "shell/inbox_model.h"

#include <ctime>
#include <map>

namespace wb::shell {

namespace {

std::tm local_tm(long long ms) {
    const auto t = static_cast<std::time_t>(ms / 1000 - (ms % 1000 < 0 ? 1 : 0));
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

long long make_local(std::tm tm) {
    tm.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&tm)) * 1000;
}

}  // namespace

bool belongs_to(const ThreadEntry& e, const ViewId& view) {
    // Chat entries: every view but Action and Running (add-chats-in-inbox D2).
    if (e.is_chat() && (view == "action" || view == "running")) return false;
    if (view == "archive") return e.archived;
    if (view == "all") return !e.archived;
    if (view == "inbox") return !e.archived && e.folder_id.empty();
    if (view == "unread") return e.unread && !e.archived;
    if (view == "action") return e.has_action;
    if (view == "running") return e.running;
    constexpr std::string_view prefix = "folder:";
    if (view.compare(0, prefix.size(), prefix) == 0) return !e.archived && e.folder_id == view.substr(prefix.size());
    return false;
}

int compare_entries(const ThreadEntry& a, const ThreadEntry& b) {
    if (a.pinned != b.pinned) return a.pinned ? -1 : 1;
    if (a.last_activity != b.last_activity) return a.last_activity < b.last_activity ? 1 : -1;
    return a.id < b.id ? 1 : a.id > b.id ? -1 : 0;
}

void apply_entry(std::vector<ThreadEntry>& list, const ThreadEntry& e, const ViewId& view, bool more) {
    std::erase_if(list, [&](const ThreadEntry& x) { return x.id == e.id; });
    if (!belongs_to(e, view)) return;
    // Beyond the loaded pages the entry arrives with the next page.
    if (more && !list.empty() && compare_entries(e, list.back()) > 0) return;
    auto at = list.begin();
    while (at != list.end() && compare_entries(e, *at) >= 0) ++at;
    list.insert(at, e);
}

const char* group_key(Group g) {
    switch (g) {
        case Group::pinned: return "pinned";
        case Group::today: return "today";
        case Group::yesterday: return "yesterday";
        case Group::this_week: return "this_week";
        case Group::last_week: return "last_week";
        case Group::this_month: return "this_month";
        case Group::older: return "older";
    }
    return "older";
}

std::set<Group> collapsed_by_default() { return {Group::last_week, Group::this_month, Group::older}; }

long long local_day_start(long long ms, int days) {
    std::tm tm = local_tm(ms);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday += days;
    return make_local(tm);
}

long long local_millis(int year, int month, int day, int hour, int minute, int second) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return make_local(tm);
}

Group date_group(bool pinned, const std::string& last_activity, long long now_ms) {
    if (pinned) return Group::pinned;
    const auto at = iso_millis(last_activity);
    if (!at) return Group::older;
    if (*at >= local_day_start(now_ms)) return Group::today;
    if (*at >= local_day_start(now_ms, -1)) return Group::yesterday;
    const std::tm now = local_tm(now_ms);
    const int weekday = (now.tm_wday + 6) % 7;  // Monday = 0
    if (*at >= local_day_start(now_ms, -weekday)) return Group::this_week;
    if (*at >= local_day_start(now_ms, -weekday - 7)) return Group::last_week;
    if (*at >= local_day_start(now_ms, 1 - now.tm_mday)) return Group::this_month;
    return Group::older;
}

std::vector<ListRow> list_rows(const std::vector<ThreadEntry>& entries, long long now_ms, const std::set<Group>& collapsed) {
    std::map<Group, std::vector<const ThreadEntry*>> by_group;
    for (const ThreadEntry& e : entries) by_group[date_group(e.pinned, e.last_activity, now_ms)].push_back(&e);
    std::vector<ListRow> rows;
    for (Group g : kGroupOrder) {
        const auto it = by_group.find(g);
        if (it == by_group.end()) continue;
        const bool is_collapsed = collapsed.contains(g);
        rows.push_back({ListRow::header, g, it->second.size(), is_collapsed, nullptr});
        if (!is_collapsed)
            for (const ThreadEntry* e : it->second) rows.push_back({ListRow::entry, g, 0, false, e});
    }
    return rows;
}

}  // namespace wb::shell
