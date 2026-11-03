// Graph JSON to the local item shapes (the parsers fuzzed by fuzz_graph_parse). They never throw: missing or mistyped
// properties become empty values, and an item without an id is rejected.
#pragma once

#include "base/json.h"
#include "mail/filter.h"
#include "mail/types.h"

#include <optional>
#include <string>
#include <vector>

namespace wb::mail {

// A Graph dateTimeTimeZone ({dateTime, timeZone}) or an ISO string, as unix ms. Times without an offset are UTC, which
// is what Graph returns unless asked for another zone.
std::optional<long long> graph_time(const json& v);
Person person_of(const json& recipient);  // {emailAddress: {name, address}}

// Messages and events as stored. Withheld items keep only the id, label and times; masked private events only the
// times and their busy state. `folder_names` are the well-known and display names of the message's folder.
std::optional<StoredMessage> parse_message(const json& item, const Rules& rules, const std::vector<std::string>& folder_names,
                                           Disposition* disposition = nullptr);
std::optional<StoredEvent> parse_event(const json& item, const Rules& rules, Disposition* disposition = nullptr);

// The tool-facing JSON of a stored item. Withheld items say only that they were withheld.
json message_json(const StoredMessage& m, bool with_body);
json event_json(const StoredEvent& e, bool with_body);

}  // namespace wb::mail
