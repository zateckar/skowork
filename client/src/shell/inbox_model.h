// View-model helpers of the Thread list (client/work-inbox Smart Views, Pinned Threads And Date Grouping), ported
// from the web UI's inbox-model.ts.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "shell/model.h"

namespace wb::shell {

// "inbox", a smart view (unread, action, running, all, archive), "folder:<id>" or "chats" (the Chats navigation entry):
// the strings inbox.list accepts.
using ViewId = std::string;

// Whether an entry belongs in a view; mirrors the host's view queries so live updates can be applied locally.
bool belongs_to(const ThreadEntry& e, const ViewId& view);
// Pinned first, then by last activity, newest first, then by id: the host's keyset order. <0 when a sorts first.
int compare_entries(const ThreadEntry& a, const ThreadEntry& b);
// Inserts, replaces or removes `e` so that `list` stays sorted and only holds entries of `view`. With more pages to
// load (`more`), an entry that sorts after the loaded ones is left to arrive with its page.
void apply_entry(std::vector<ThreadEntry>& list, const ThreadEntry& e, const ViewId& view, bool more = false);

enum class Group { pinned, today, yesterday, this_week, last_week, this_month, older };
inline constexpr Group kGroupOrder[] = {Group::pinned,    Group::today,      Group::yesterday, Group::this_week,
                                        Group::last_week, Group::this_month, Group::older};
const char* group_key(Group g);  // the string key suffix: "pinned", "today", "this_week", ...
// Older periods start collapsed.
std::set<Group> collapsed_by_default();

// The date group of an entry in local time; weeks start on Monday. now_ms: milliseconds since the epoch.
Group date_group(bool pinned, const std::string& last_activity, long long now_ms);

struct ListRow {
    enum Kind { header, entry } kind = header;
    Group group = Group::older;
    size_t count = 0;        // header
    bool collapsed = false;  // header
    const ThreadEntry* item = nullptr;  // entry; points into the list given to list_rows
};
// Flattens sorted entries into header and entry rows; entries of collapsed groups are left out.
std::vector<ListRow> list_rows(const std::vector<ThreadEntry>& entries, long long now_ms, const std::set<Group>& collapsed);

inline constexpr float kHeaderRowHeight = 32, kEntryRowHeight = 68;

// Midnight local time of the day `days` from the day of `ms` (negative: earlier), through the C calendar so
// daylight-saving days stay right.
long long local_day_start(long long ms, int days = 0);
// Milliseconds of a local date and time (month 1-12).
long long local_millis(int year, int month, int day, int hour = 0, int minute = 0, int second = 0);

}  // namespace wb::shell
