// Incremental delta sync of mail folders and the calendar view (client/mail-calendar Incremental Delta Sync, Sync
// Window, Event Feed For Triggers And Knowledge; design D3, D5): one delta query per folder and one for the calendar
// window, a full resync when Graph reports an expired token, and normalised events for client/scheduler and
// client/knowledge-graph, deduplicated by (item id, change key). Withheld, excluded and private items are never published.
#pragma once

#include "base/json.h"
#include "base/time.h"
#include "mail/filter.h"
#include "mail/graph_client.h"
#include "mail/mail_store.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wb::mail {

// kind: mail.received | mail.updated | mail.deleted | event.created | event.updated | event.cancelled.
struct MailEvent {
    std::string kind;
    std::string id;
    json detail = json::object();  // the item as tools show it (message_json / event_json without bodies)
    long long start = 0, end = 0;  // calendar events: unix ms
};

struct SyncReport {
    int folders = 0, messages = 0, events = 0, published = 0, resyncs = 0;
};

class MailSync {
public:
    MailSync(MailStore& store, const Clock& clock);

    void set_config(const MailConfig& cfg);
    // One sync cycle: folders, each folder not excluded, then the calendar window. Fails with the first Graph error
    // that stops it (offline, not connected); items already processed stay.
    Result<SyncReport> sync_once(GraphClient& graph, const std::shared_ptr<CancelToken>& cancel = {});
    void on_event(std::function<void(const MailEvent&)> fn);
    [[nodiscard]] Rules rules();

private:
    Status sync_folder(const StoredFolder& f, SyncReport& r, const std::shared_ptr<CancelToken>& cancel);
    Status sync_calendar(SyncReport& r, const std::shared_ptr<CancelToken>& cancel);
    void publish(const MailEvent& e, SyncReport& r);

    GraphClient* graph_ = nullptr;  // of the running cycle
    MailStore& store_;
    const Clock& clock_;
    std::mutex mx_;  // one cycle at a time
    std::mutex cfg_mx_;
    MailConfig cfg_;
    std::vector<std::function<void(const MailEvent&)>> listeners_;
};

// Folders whose new messages are not "received" mail (the user's own drafts and sent mail).
bool own_mail_folder(const StoredFolder& f);

}  // namespace wb::mail
