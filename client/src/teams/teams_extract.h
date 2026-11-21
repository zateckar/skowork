// Conversations, messages and profiles from the decoded Teams IndexedDB cache (client/teams;
// add-local-outlook-and-teams design D4 "What is read", D5 plain text). Field names are matched leniently; records
// missing a required field are skipped and counted, so a Teams update degrades the result instead of failing.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "teams/indexeddb.h"

namespace wb::teams {

struct TeamsConversation {
    std::string id;
    std::string type;  // chat | group | meeting | channel | other
    std::string topic;
    long long last_activity_ms = 0;
};

struct TeamsMessage {
    std::string id, conversation_id, parent_id, sender_mri, sender_name, html, text;
    long long at_ms = 0, edited_ms = 0;
    bool deleted = false, system = false;
    std::vector<std::string> mention_mris;
    std::string version;  // the message's version field, else "h:" + a hash of content, edit time and delete flag
};

struct TeamsProfile {
    std::string mri, name, email;
};

struct Extraction {
    std::vector<TeamsConversation> conversations;
    std::vector<TeamsMessage> messages;
    std::vector<TeamsProfile> profiles;
    int skipped = 0;        // records or messages missing a required field
    int blobs = 0;          // values kept in blob files (not read)
    int decode_errors = 0;  // values that could not be deserialised
    std::string user_mri;   // "8:orgid:<user-guid>" from a database name ending in ...:<tenant-guid>:<user-guid>...
};

Extraction extract(const IdbCatalog& catalog);

// The conversation type from its thread properties, else inferred from the conversation id.
std::string conversation_type(const json& conversation, std::string_view id);

// Milliseconds since the epoch from a number, a decimal string or an ISO 8601 string; 0 when not a time.
long long time_ms(const json& v);

// Plain text from Teams message HTML: tags removed, common entities decoded, block elements as newlines,
// <at>Name</at> as "@Name", whitespace runs collapsed, trimmed.
std::string html_to_text(std::string_view html);

}  // namespace wb::teams
