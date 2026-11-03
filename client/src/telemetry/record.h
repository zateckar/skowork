// Portable part of client/telemetry (add-telemetry-and-admin design D1): telemetry records built only from the fields of
// the generated catalogue allowlist, filtered by telemetry level, with secret redaction and URL-to-domain reduction.
// No Win32 headers, so the fuzz and sanitizer jobs build it.
#pragma once

#include "base/json.h"
#include "telemetry/catalogue.gen.h"
#include "telemetry/pattern.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wb::telemetry {

const char* to_string(Level l);
// "off", "metadata" or "content"; anything else is nullopt (the caller falls back to metadata).
std::optional<Level> parse_level(std::string_view s);

using Value = std::variant<long long, double, bool, std::string>;

// One attribute of a record. Only catalogue attributes can be named: there is no constructor from a string name.
struct Field {
    Attr attr;
    Value value;
    Field(Attr a, long long v) : attr(a), value(v) {}
    Field(Attr a, int v) : attr(a), value(static_cast<long long>(v)) {}
    Field(Attr a, double v) : attr(a), value(v) {}
    Field(Attr a, bool v) : attr(a), value(v) {}
    Field(Attr a, std::string v) : attr(a), value(std::move(v)) {}
    Field(Attr a, std::string_view v) : attr(a), value(std::string(v)) {}
    Field(Attr a, const char* v) : attr(a), value(std::string(v)) {}
};

struct Record {
    Event event{};
    long long time_ms = 0;  // Unix time
    std::vector<Field> attrs;
};

// Built-in secret patterns (tokens, codes, passwords: the diagnostic-log redaction) plus the regular expressions of
// policy.telemetry.redaction_patterns (design D8). A pattern outside the supported syntax is skipped and listed in invalid().
// When one text exhausts kRedactionBudget, apply() returns the redaction marker for all of it.
class Redactor {
public:
    Redactor() = default;
    explicit Redactor(const std::vector<std::string>& patterns);
    [[nodiscard]] std::string apply(std::string_view text) const;
    [[nodiscard]] const std::vector<std::string>& invalid() const { return invalid_; }

private:
    std::vector<Pattern> patterns_;
    std::vector<std::string> invalid_;
};

inline constexpr std::string_view kRedacted = "[redacted]";
inline constexpr long long kRedactionBudget = 4'000'000;  // Pike VM steps per text, all patterns together
inline constexpr size_t kMaxMetadataString = 256;
inline constexpr size_t kMaxContentString = 8000;

// The host name of a URL ("https://Wiki.Example.com/a?b" -> "wiki.example.com") or of a bare host[:port][/path];
// empty when there is none.
std::string domain_of(std::string_view url_or_host);

struct FilterStats {
    int not_in_event = 0;  // attributes the event does not list (a programming error; dropped)
    int above_level = 0;   // attributes above the current level (dropped)
    int wrong_type = 0;    // a value whose type does not match the catalogue (dropped)
};

// The record as it may be stored and sent at `level`: nullopt at level off. Attributes outside the event's catalogue
// entry or above the level are dropped; domain attributes are reduced to the host name; strings are redacted and cut.
std::optional<Record> make_record(Event e, long long time_ms, std::span<const Field> fields, Level level, const Redactor& redactor,
                                  FilterStats* stats = nullptr);
inline std::optional<Record> make_record(Event e, long long time_ms, std::initializer_list<Field> fields, Level level,
                                         const Redactor& redactor, FilterStats* stats = nullptr) {
    return make_record(e, time_ms, std::span<const Field>(fields.begin(), fields.size()), level, redactor, stats);
}

// {"event": "tool.call", "time": 1700000000000, "attrs": {"tool.id": "files.read", ...}}: the stored and viewed form.
json to_json(const Record& r);
std::optional<Record> record_from_json(const json& j);

std::optional<Attr> attr_by_name(std::string_view name);
std::optional<Event> event_by_name(std::string_view name);

}  // namespace wb::telemetry
