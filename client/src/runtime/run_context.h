// Run Context (client/agent-runtime; add-agent-runtime design D11): what the model cannot discover with tools — the
// local date and time, the time zone, the signed-in user and the user's folders.
#pragma once

#include <string>

namespace wb::runtime {

struct RunContextInfo {
    int year = 0, month = 0, day = 0, weekday = 0;  // weekday: 0 = Sunday
    int hour = 0, minute = 0;
    int utc_offset_minutes = 0;  // local time minus UTC
    std::string time_zone;       // Windows time zone key, for example "Central Europe Standard Time"
    std::string user;            // the signed-in user's principal
    std::string home, documents, downloads, desktop;  // forward slashes; empty when unknown
};

// The text appended to the system prompt.
std::string format_run_context(const RunContextInfo& info);

// Collects the context now from Windows (local time, time zone, known folders). `user` is the signed-in user.
RunContextInfo collect_run_context(const std::string& user);

}  // namespace wb::runtime
