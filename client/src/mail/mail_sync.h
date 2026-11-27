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

// kind: mail.received | mail.sent | mail.updated | mail.deleted | event.created | event.updated | event.cancelled. mail.sent
// (close-replied-work): a new non-draft message in Sent Items outside backfill, for client/agenda and client/event-triage only.
struct MailEvent {
    std::string kind;
    std::string id;
    json detail = json::object();  // the item as tools show it (message_json / event_json without bodies)
    long long start = 0, end = 0;  // calendar events: unix ms
};

struct SyncReport {
    int folders = 0, messages = 0, events = 0, published = 0, resyncs = 0;
};

// One item change of a page, as client/event-triage takes it in (improve-proactive-durable-work design D1). `fresh`:
// a new or changed item that is not the user's own mail, a draft, withheld or first-sync backfill.
struct PageChange {
    std::string scope;  // mail | calendar
    std::string id;
    bool removed = false;
    bool fresh = false;
    std::string folder_id, folder;  // mail: the folder's id and its well-known or display name
};
// Runs inside the page transaction; a failure rolls the whole page back and stops the cycle.
using PageSink = std::function<Status(const std::vector<PageChange>&)>;

class MailSync {
public:
    MailSync(MailStore& store, const Clock& clock);

    void set_config(const MailConfig& cfg);
    // One sync cycle: folders, each folder not excluded, then the calendar window. Fails with the first Graph error
    // that stops it (offline, not connected); items already processed stay.
    Result<SyncReport> sync_once(GraphClient& graph, const std::shared_ptr<CancelToken>& cancel = {});
    void on_event(std::function<void(const MailEvent&)> fn);
    // The triage intake of each page, and what runs after every complete cycle.
    void set_page_sink(PageSink sink);
    void on_cycle(std::function<void()> fn);
    [[nodiscard]] Rules rules();

private:
    Status sync_folder(const StoredFolder& f, SyncReport& r, const std::shared_ptr<CancelToken>& cancel);
    Status sync_calendar(SyncReport& r, const std::shared_ptr<CancelToken>& cancel);
    void publish(const MailEvent& e, SyncReport& r);
    // Commits a page: `write` stores the source changes into `events`/`changes`, then the sink and `cursor` run in the
    // same transaction. Listeners get the events only after the commit.
    Status commit_page(const std::function<Status(std::vector<MailEvent>&, std::vector<PageChange>&)>& write,
                       const std::function<Status()>& cursor, SyncReport& r);

    GraphClient* graph_ = nullptr;  // of the running cycle
    MailStore& store_;
    const Clock& clock_;
    std::mutex mx_;  // one cycle at a time
    std::mutex cfg_mx_;
    MailConfig cfg_;
    std::vector<std::function<void(const MailEvent&)>> listeners_;
    PageSink sink_;
    std::vector<std::function<void()>> cycle_listeners_;
};

// A page that lists an item twice with different content, or both live and removed, has no defensible current
// revision: empty when the page is consistent, else the conflicting id.
std::string conflicting_item(const std::vector<json>& items);

// Folders whose new messages are not "received" mail (the user's own drafts and sent mail).
bool own_mail_folder(const StoredFolder& f);

}  // namespace wb::mail
