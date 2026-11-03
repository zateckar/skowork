#include "runtime/run_context.h"

#include "platform/win.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace wb::runtime {
namespace {

constexpr const char* kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
        out = narrow(path);
        std::replace(out.begin(), out.end(), '\\', '/');
    }
    ::CoTaskMemFree(path);
    return out;
}

}  // namespace

std::string format_run_context(const RunContextInfo& c) {
    char date[160];
    const int off = std::abs(c.utc_offset_minutes);
    std::snprintf(date, sizeof date, "%s %d %s %d, %02d:%02d (%04d-%02d-%02d), UTC%c%02d:%02d", kWeekdays[(c.weekday % 7 + 7) % 7],
                  c.day, kMonths[std::clamp(c.month, 1, 12) - 1], c.year, c.hour, c.minute, c.year, c.month, c.day,
                  c.utc_offset_minutes < 0 ? '-' : '+', off / 60, off % 60);
    std::string text = "Context of this Run:\n- Now: ";
    text += date;
    if (!c.time_zone.empty()) text += " (" + c.time_zone + ")";
    text += "\n";
    if (!c.user.empty()) text += "- User: " + c.user + "\n";
    if (!c.home.empty()) text += "- Home folder: " + c.home + "\n";
    if (!c.documents.empty()) text += "- Documents: " + c.documents + "\n";
    if (!c.downloads.empty()) text += "- Downloads: " + c.downloads + "\n";
    if (!c.desktop.empty()) text += "- Desktop: " + c.desktop + "\n";
    return text;
}

RunContextInfo collect_run_context(const std::string& user) {
    RunContextInfo c;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    c.year = st.wYear;
    c.month = st.wMonth;
    c.day = st.wDay;
    c.weekday = st.wDayOfWeek;
    c.hour = st.wHour;
    c.minute = st.wMinute;
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    const DWORD kind = ::GetDynamicTimeZoneInformation(&tz);
    long bias = tz.Bias;
    if (kind == TIME_ZONE_ID_DAYLIGHT) bias += tz.DaylightBias;
    else if (kind == TIME_ZONE_ID_STANDARD) bias += tz.StandardBias;
    c.utc_offset_minutes = static_cast<int>(-bias);
    c.time_zone = narrow(tz.TimeZoneKeyName);
    c.user = user;
    c.home = known_folder(FOLDERID_Profile);
    c.documents = known_folder(FOLDERID_Documents);
    c.downloads = known_folder(FOLDERID_Downloads);
    c.desktop = known_folder(FOLDERID_Desktop);
    return c;
}

}  // namespace wb::runtime
