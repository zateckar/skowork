// client/daily-report core (add-local-outlook-and-teams design D7): the user's settings, when a report is due, and the
// digest the report Run gets as untrusted content, with stable ids for Citations and fixed caps. Portable, no I/O.
#pragma once

#include "base/json.h"

#include <array>
#include <string>
#include <vector>

namespace wb::report {

inline constexpr int kLastStartHour = 18;   // a late start writes that day's report only before 18:00
inline constexpr size_t kMaxItemsPerList = 60;
inline constexpr size_t kMaxDigestChars = 24'000;
inline constexpr size_t kMaxPreview = 400;

struct Settings {
    bool enabled = false;
    int hour = 8, minute = 0;
    std::array<bool, 7> days{false, true, true, true, true, true, false};  // by tm_wday: Sunday = 0
};

Settings parse_settings(const json& j);
json settings_json(const Settings& s);
// Applies a partial patch ({"enabled", "time": "HH:MM", "days": [0..6]}); invalid values fail with "invalid".
bool apply_patch(Settings& s, const json& patch, std::string* error);

struct LocalTime {
    int year = 1970, month = 1, day = 1, wday = 4, hour = 0, minute = 0;
};

std::string day_key(const LocalTime& t);  // YYYY-MM-DD
// Due when today is a working day, the chosen time has passed, it is before 18:00 and no report was written today.
bool report_due(const Settings& s, const LocalTime& now, const std::string& last_day);

struct DigestEvent {
    std::string id, subject, start, end, location, organizer;
    bool busy_only = false, online = false, all_day = false;
};
struct DigestMail {
    std::string id, subject, from, received, preview;
    bool is_read = false, has_attachments = false, to_user = false;
};
struct DigestChat {
    std::string conversation_id, message_id, conversation, from, sent, text;
    bool addressed = false;
};
struct DigestAgenda {
    std::string id, title, due;
    bool overdue = false, is_new = false, prepared = false;
};
struct Digest {
    std::string date, since;  // the report's local date, and the previous report's time (ISO) or empty
    std::vector<DigestEvent> events;
    std::vector<DigestMail> mail;
    std::vector<DigestChat> chats;
    std::vector<DigestAgenda> agenda;
    std::vector<std::string> suggestions;  // open triage suggestion titles
};

// The digest as JSON text, at most kMaxDigestChars: lists keep their first kMaxItemsPerList entries (callers pass newest
// first), previews are cut first, then the oldest entries are dropped until it fits.
std::string render_digest(const Digest& d);

// The fixed instruction of the report Run (Daily Report Content, Daily Report Has No Effects).
std::string instructions();
std::string run_message(const std::string& date);

}  // namespace wb::report
