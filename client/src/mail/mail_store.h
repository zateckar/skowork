// The local mail and calendar copy in app.db (client/mail-calendar Incremental Delta Sync, Sync Window, Offline Mail
// Behaviour; design D3): folders, messages with plain-text bodies and an FTS5 index, calendar events, delta links and
// the user's own exclusions. Withheld items keep only their id, label and times; private events only their times.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "mail/types.h"
#include "storage/database.h"

#include <optional>
#include <string>
#include <vector>

namespace wb::mail {

class MailStore {
public:
    explicit MailStore(Database& db) : db_(db) {}

    enum class Change { none, created, updated };
    // Deduplicated by (id, change key): an unchanged item is "none". A write failure is reported through `status` (set
    // only on failure) so a caller's page transaction can roll back (improve-proactive-durable-work D1).
    Change upsert_message(const StoredMessage& m, Status* status = nullptr);
    std::optional<StoredMessage> remove_message(const std::string& id, Status* status = nullptr);
    std::optional<StoredMessage> message(const std::string& id);
    std::vector<StoredMessage> search(const SearchQuery& q);
    std::vector<std::string> message_ids(const std::string& folder_id);
    std::vector<std::string> all_message_ids();
    // Every message received in [from, to), withheld ones included, oldest first (client/evaluation export).
    std::vector<StoredMessage> messages_between(long long from, long long to);
    // The user replied (close-replied-work D1): a non-draft message in Sent Items with this conversation id, sent after
    // `after_ms`. Never matched by subject; false for an empty conversation id.
    bool user_replied(const std::string& conversation_id, long long after_ms);

    Change upsert_event(const StoredEvent& e, Status* status = nullptr);
    std::optional<StoredEvent> remove_event(const std::string& id, Status* status = nullptr);
    std::optional<StoredEvent> event(const std::string& id);
    std::vector<StoredEvent> events_between(long long from, long long to);
    std::vector<std::string> event_ids();

    Status set_folders(const std::vector<StoredFolder>& folders);
    std::vector<StoredFolder> folders();
    std::optional<StoredFolder> folder(const std::string& id_or_name);

    // Delta links and the time of the last complete sync per key ("folder:<id>", "calendar").
    std::string delta_link(const std::string& key);
    Status set_delta_link(const std::string& key, const std::string& link, long long synced_at, const std::string& extra = {});
    std::string sync_extra(const std::string& key);
    Status drop_sync_state(const std::string& key);
    // Every folder and the calendar are read in full on the next cycle (exclusions or labels changed); known items are
    // not announced again.
    void restart_sync();
    long long last_sync();  // the latest successful sync of any key, unix ms; 0 when never

    // Items outside the sync window are not kept.
    int prune(long long mail_before, long long events_before, long long events_after);

    // The user's exclusions (folders and categories), on top of policy's.
    std::vector<std::string> user_exclusions(const std::string& kind);
    Status add_user_exclusion(const std::string& kind, const std::string& value);
    Status remove_user_exclusion(const std::string& kind, const std::string& value);

    // Disconnect and forget: every synced item and delta link (user exclusions stay unless `all`).
    void clear(bool all = false);

    struct Counts {
        long long messages = 0, events = 0, withheld = 0;
    };
    Counts counts();

    [[nodiscard]] Database& db() { return db_; }

private:
    Database& db_;
};

}  // namespace wb::mail
