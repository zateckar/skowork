// The local item shapes of client/mail-calendar: what the store keeps and the tools show. Portable (the fuzz job
// builds the parsers that produce them).
#pragma once

#include "base/json.h"

#include <optional>
#include <string>
#include <vector>

namespace wb::mail {
struct Person {
    std::string name, address;
};

struct StoredMessage {
    std::string id, folder_id, conversation_id, change_key;
    long long received_at = 0;  // unix ms
    std::string subject;
    Person from;
    std::vector<Person> to, cc;
    std::string body, preview;
    bool has_attachments = false, is_read = false, is_draft = false;
    std::vector<std::string> categories;
    std::string label;
    bool withheld = false;
    std::string web_link;
};

struct StoredEvent {
    std::string id, ical_uid, series_master_id, change_key;
    long long start_at = 0, end_at = 0;  // unix ms
    bool all_day = false;
    std::string subject, body, location;
    Person organizer;
    json attendees = json::array();  // [{name, address, type, response}]
    bool is_organizer = false, cancelled = false;
    std::string response, show_as, sensitivity;
    bool masked = false;  // a private item: only its times are kept
    std::vector<std::string> categories;
    std::string label;
    bool withheld = false;
    std::string web_link;
    std::string online_url;  // the join link of an online meeting (Teams, or a link in onlineMeeting)
};

struct StoredFolder {
    std::string id, name, well_known;
};

struct SearchQuery {
    std::string text, folder_id, from;
    long long since = 0, until = 0;  // unix ms, 0: open
    std::optional<bool> has_attachments;
    int limit = 25;
};

}  // namespace wb::mail
