// Text formatting of the window: sizes, durations, relative times. Ported from the web UI's format.ts; the
// locale-specific parts come from the string bundles, dates in full from the Windows layer.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "base/json.h"
#include "ui/strings.h"

namespace wb::shell {

// Binary sizes with Windows-style labels: 1536 -> "1.5 KB". `decimal` is the language's decimal separator.
std::string format_bytes(double bytes, char decimal = '.');
// Remaining time as a short phrase: "45 s", "12 min", "1 h 5 min"; rounds up to whole minutes above a minute.
std::string format_duration(double seconds);
// Elapsed time of a Run: "12 s", "3 min 5 s", "1 h 2 min".
std::string format_elapsed(long long ms);
// Relative time of an ISO timestamp for list entries: "now", "5 min ago", "yesterday", "3 days ago"; older ones go
// through `date` (milliseconds since the epoch -> a localised date). Empty for an unreadable timestamp.
std::u16string format_relative(const std::string& iso, long long now_ms, const ui::Strings& s,
                               const std::function<std::u16string(long long)>& date);
// A share as a percentage: one decimal below 10 % ("4.5%"), whole numbers otherwise ("42%").
std::string format_percent(double fraction, char decimal = '.');
// A search snippet split into plain and matched parts; the host wraps matches in [[ and ]].
struct SnippetPart {
    std::string text;
    bool hit = false;
    bool operator==(const SnippetPart&) const = default;
};
std::vector<SnippetPart> snippet_parts(std::string_view snippet);
// The About page's details as plain text for the clipboard (app.version's result).
std::string about_text(const json& version);
std::string short_commit(const std::string& commit, size_t length = 8);
// Best human-readable name for the signed-in user, whatever shape the host sends (a string or an object).
std::string display_user(const json& user);

struct UsageSegment {
    std::string name;
    long long bytes = 0;
    double fraction = 0;  // share of the quota
};
// Bar segments of the storage categories: largest first, the rest beyond max_segments - 1 merged into "Other".
std::vector<UsageSegment> usage_segments(const json& categories, double max, size_t max_segments = 6);

}  // namespace wb::shell
