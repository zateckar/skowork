// The mail and calendar detection sources of client/scheduler (design D9; add-mail-calendar design D7): their
// conditions, the matching of client/mail-calendar events, and the calendar offsets ("15 minutes before the start"),
// whose timers the scheduler owns. A Run started by these Triggers gets only item ids and times: mail content is read
// with the mail tools, as untrusted content.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "scheduler/scheduler.h"

#include <functional>
#include <string>
#include <vector>

namespace wb::scheduler {

// mail: {folder?, from?, subject_contains?, has_attachments?}. Every given field must match a received message.
Status validate_mail_conditions(const json& conditions);
// `message`: the event's item as the mail tools show it (subject, from {name, address}, has_attachments) plus folder.
bool mail_matches(const json& conditions, const json& message);

// calendar: {at: "start"|"end" (default start), offset_minutes (-1440..1440; negative is before), subject_contains?,
// organizer?, external_attendees?}.
Status validate_calendar_conditions(const json& conditions);
// `own_domain`: the user's mail domain; an attendee outside it is external.
bool calendar_matches(const json& conditions, const json& event, const std::string& own_domain);

struct CalendarItem {
    std::string id;
    long long start = 0, end = 0;  // unix ms
    json detail;                   // as the calendar tools show it
};

// Fires calendar Triggers when an event's start or end plus the Trigger's offset passes. The key includes the event's
// time, so a rescheduled event fires at its new time and a cancelled one (gone from `events`) does not fire.
class CalendarOffsets {
public:
    CalendarOffsets(Scheduler& s, std::function<std::vector<CalendarItem>(long long from, long long to)> events,
                    std::function<std::string()> own_domain);
    // Fires everything due since the previous call (the first call looks back two minutes).
    void tick(long long now);

private:
    Scheduler& sched_;
    std::function<std::vector<CalendarItem>(long long, long long)> events_;
    std::function<std::string()> own_domain_;
    long long last_ = 0;
};

// The trigger detail of a mail or calendar firing: ids and times only.
json mail_trigger_detail(const std::string& kind, const std::string& message_id, const std::string& folder);

}  // namespace wb::scheduler
