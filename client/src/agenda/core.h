// Agenda core (client/agenda, add-agenda design D1-D3): the detection request and its strict parser, the admission
// rules that keep nonsense off the Agenda, due-date checks and the matching used to merge candidates. Portable, no I/O.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "base/result.h"

namespace wb::agenda {

// Who the user is, for responsibility checks.
struct Identity {
    std::string name;                 // display name, e.g. "Jana Nováková"
    std::vector<std::string> emails;  // lower-case addresses
};

// One source item offered for detection. Metadata comes from the client, never from the item text.
struct SourceItem {
    std::string kind;          // mail | calendar | meeting | chat
    std::string id;            // stable within its kind (Graph id, meeting id, chat message id)
    std::string conversation;  // mail conversation, meeting id, chat id: merges by source identity (design D3)
    std::string title;         // subject, event or meeting title, chat name
    std::string text;          // body or message text as sent to the model (already cut to the size limit)
    std::string author;        // display name or address
    bool author_is_user = false;
    bool user_addressed = false;  // in To (not only Cc), invited, a direct Chat, or mentioned
    int recipients = 0;           // To + Cc (mail), attendees (calendar), members (team Chat)
    bool distribution_list = false;
    long long at_ms = 0;          // sent, received or start time (Unix ms)
    std::string link;             // route the UI opens for the source
};

struct Candidate {
    std::string source_id;
    std::string title, action, due, due_phrase, due_kind, responsibility, quote, draft_kind;
    double confidence = 0;
};

// A resolved due moment.
struct Due {
    long long at_ms = 0;
    bool has_time = false;
    bool inferred = false;
};

constexpr size_t kMaxItemsPerBatch = 10;
constexpr size_t kMaxItemText = 6000;
constexpr size_t kMaxCandidatesPerItem = 3;
constexpr int kBroadcastRecipients = 25;

// The fixed system instruction and the JSON data of one detection request. tz_offset_min is the local offset from UTC
// at now_ms; dates in the data are local, so the model can resolve "by Friday".
std::string detection_instructions();
json detection_data(const std::vector<SourceItem>& items, const Identity& me, long long now_ms, int tz_offset_min);

// Parses a detection response strictly: one JSON object {"items":[{"id", "candidates":[...]}]} with exactly one entry per
// item in input order. Unknown fields, missing or extra items, more than three candidates per item, wrong types or
// oversized strings reject the whole response (Bounded Agenda Detection).
Result<std::vector<Candidate>> parse_detection(const std::string& text, const std::vector<SourceItem>& items);

// Why a candidate was not admitted (Agenda Item Admission, Noise Excluded From Agenda); none = admitted.
enum class Drop { none, quote, responsibility, action, date, past, confidence, broadcast };
const char* to_string(Drop d);

// Resolves the candidate's due value (ISO date or local date-time) and checks a weekday, "today" or "tomorrow" phrase
// against it (Agenda Due Dates). Empty when the date is invalid or contradicts its phrase.
std::optional<Due> resolve_due(const Candidate& c, long long source_ms, int tz_offset_min);

Drop admit(const Candidate& c, const SourceItem& item, const Identity& me, long long now_ms, int tz_offset_min,
           double min_confidence, Due* due);

// True when the quote occurs in the text, compared with runs of white space folded to one space.
bool quote_in_text(const std::string& quote, const std::string& text);

// Word overlap of two titles (Jaccard over lower-case words of three or more letters), 0..1.
double title_similarity(const std::string& a, const std::string& b);

// The model request that decides whether a candidate is the same commitment as one of up to ten open items
// (design D3); the response is {"same": "<item id>"} or {"same": null}.
std::string merge_instructions();
Result<std::optional<std::string>> parse_merge(const std::string& text, const std::vector<std::string>& item_ids);

// Strict JSON: at most 64 KiB, depth 32, no duplicate keys.
std::optional<json> strict_json(const std::string& text);

}  // namespace wb::agenda
