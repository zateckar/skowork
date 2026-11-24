// The Local Outlook backend's view of Outlook (client/mail-calendar Local Outlook Backend; add-local-outlook-and-teams
// design D2, D3): plain item shapes and the operations OutlookGraph needs. ComOutlookSession implements it over the
// Outlook object model on its own apartment thread; tests use a fake. Portable.
//
// Errors use the GraphClient codes: offline (Outlook is not running or does not answer), throttled (Outlook stayed
// busy), not_found, access_denied (the user declined an Outlook security prompt), bad_request, cancelled.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <optional>
#include <string>
#include <vector>

namespace wb::mail::outlook {

struct OlFolder {
    std::string id, name;
    std::string well_known;  // Graph's well-known names: inbox, sentitems, drafts, outbox, deleteditems, junkemail, archive
};

// One row of a folder table: enough to notice new, changed and removed messages without opening them.
struct OlRow {
    std::string id;
    long long modified = 0;  // LastModificationTime, unix ms UTC
    long long received = 0;  // ReceivedTime, unix ms UTC
};

struct OlAddress {
    std::string name, address;  // address: SMTP when it can be resolved
};

struct OlRecipient {
    OlAddress who;
    std::string type = "required";  // messages: to | cc | bcc; meetings: required | optional | resource
    std::string response;           // meetings: none | accepted | tentativelyAccepted | declined | notResponded
};

struct OlMessage {
    std::string id, folder_id, conversation_id;
    long long received = 0, modified = 0;  // unix ms UTC
    std::string subject, body;             // body: plain text
    OlAddress from;
    std::vector<OlRecipient> recipients;   // type to | cc | bcc
    bool has_attachments = false, is_read = false, is_draft = false;
    std::vector<std::string> categories;
    std::string msip_labels;  // the msip_labels named property, empty when the item has none
};

struct OlAttachment {
    std::string id, name, content_type;
    long long size = 0;
};

struct OlEvent {
    std::string id;         // EntryID, or occurrence_id() of an occurrence of a recurring series
    std::string series_id;  // the series' EntryID for an occurrence, else empty
    std::string global_id;  // GlobalAppointmentID (iCalUId)
    long long start = 0, end = 0, modified = 0;  // unix ms UTC
    bool all_day = false, is_organizer = false, cancelled = false, is_meeting = false;
    std::string subject, body, location;
    OlAddress organizer;
    std::vector<OlRecipient> attendees;
    std::string response;     // as OlRecipient::response, plus organizer
    std::string show_as;      // free | tentative | busy | oof | workingElsewhere
    std::string sensitivity;  // normal | personal | private | confidential
    std::vector<std::string> categories;
    std::string msip_labels;
    std::string online_url;
};

struct MessageQuery {
    std::string folder_id;        // empty: every mail folder of the default store
    std::string text;             // subject or body contains
    std::string conversation_id;  // exact
    long long since = 0, until = 0;  // received, unix ms, 0: open
    bool attachments_only = false;
    size_t limit = 50;  // newest first
};

// Fields of a draft or new message; an absent field is left as it is.
struct DraftPatch {
    std::optional<std::string> subject, body;
    std::optional<std::vector<std::string>> to, cc;
};

struct EventPatch {
    std::optional<std::string> subject, body, location;
    std::optional<long long> start, end;  // unix ms UTC
    std::optional<std::vector<std::string>> required, optional;
};

class OutlookSession {
public:
    virtual ~OutlookSession() = default;

    // running | not_running | busy, for the settings section.
    virtual std::string state() { return "running"; }

    // The account of the default store: name and SMTP address.
    virtual Result<OlAddress> me() = 0;
    virtual Result<std::vector<OlFolder>> folders() = 0;
    // Messages of a folder received at or after `received_since`, from the folder table (no item is opened).
    virtual Result<std::vector<OlRow>> rows(const std::string& folder_id, long long received_since) = 0;
    // A cheap fingerprint of a folder (item count and newest modification) that changes whenever a message in it is
    // added, changed or removed; empty when unknown, meaning "list the folder".
    virtual Result<std::string> folder_stamp(const std::string& folder_id) {
        (void)folder_id;
        return std::string();
    }
    virtual Result<OlMessage> message(const std::string& id) = 0;
    virtual Result<std::vector<OlMessage>> find(const MessageQuery& q) = 0;
    virtual Result<std::vector<OlAttachment>> attachments(const std::string& message_id) = 0;
    virtual Result<std::string> attachment_bytes(const std::string& message_id, const std::string& attachment_id) = 0;

    // kind: reply | reply_all | forward. The new item is saved in Drafts with `comment` above the quoted text.
    virtual Result<OlMessage> create_reply(const std::string& id, const std::string& kind, const std::string& comment) = 0;
    virtual Result<OlMessage> create_draft(const DraftPatch& p) = 0;
    virtual Result<OlMessage> update_draft(const std::string& id, const DraftPatch& p) = 0;
    virtual Status send(const std::string& draft_id) = 0;
    virtual Status send_new(const DraftPatch& p) = 0;
    virtual Result<OlMessage> move(const std::string& id, const std::string& folder_id) = 0;
    // Moves the item to Deleted Items.
    virtual Status remove(const std::string& id) = 0;

    // Events overlapping [from, to), recurring series expanded into occurrences, by start; at most `limit`.
    virtual Result<std::vector<OlEvent>> events(long long from, long long to, size_t limit) = 0;
    // A cheap fingerprint of the calendar folder (item count and newest modification) that changes whenever any item,
    // series or exception in it is added, changed or removed; empty when unknown, meaning "read the calendar".
    virtual Result<std::string> calendar_stamp() { return std::string(); }
    virtual Result<OlEvent> event(const std::string& id) = 0;
    // Creates (empty id) or changes an event; with attendees it becomes a meeting and invitations or updates are sent.
    virtual Result<OlEvent> save_event(const std::string& id, const EventPatch& p) = 0;
    // response: accept | tentative | decline; the response is sent to the organiser with the comment.
    virtual Status respond(const std::string& id, const std::string& response, const std::string& comment) = 0;
    // Free/busy digits of Graph's availabilityView (0 free, 1 tentative, 2 busy, 3 oof, 4 working elsewhere) for
    // `slots` slots of `interval_minutes` from `from`; not_found when the address cannot be resolved.
    virtual Result<std::string> free_busy(const std::string& address, long long from, int interval_minutes, int slots) = 0;

    // Shows the item in an Outlook window (Open in Outlook).
    virtual Status display(const std::string& id) = 0;
};

// Occurrence ids (design D2): "<series EntryID>.<local start yyyymmddThhmm>".
std::string occurrence_id(const std::string& series_id, const std::string& local_stamp);
// The series EntryID and the local start stamp of an occurrence id; nullopt for a plain EntryID.
std::optional<std::pair<std::string, std::string>> split_occurrence_id(const std::string& id);

}  // namespace wb::mail::outlook
