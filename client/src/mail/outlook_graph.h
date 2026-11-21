// The Local Outlook backend as a Graph transport (client/mail-calendar Local Outlook Backend; add-local-outlook-and-teams
// design D2): answers the subset of Microsoft Graph v1.0 that client/mail-calendar calls, with Graph-shaped JSON built
// from an OutlookSession, so the tools, sync, label rules and the event feed run unchanged on Outlook.
//
// Delta queries are emulated: the first call lists the window and returns an "outlook-delta|..." link; a later call with
// that link lists what changed since (and "@removed" entries) by comparing with a snapshot of (id, changeKey). Snapshots
// and the generation are kept through `persist`; a link of another generation answers 410 syncStateNotFound, and
// MailSync then reads in full without announcing items it already has. Portable.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "base/time.h"
#include "mail/outlook_session.h"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::mail::outlook {

// Durable state of the emulation (snapshots per key, the generation): load returns nullopt when nothing is stored.
struct Persist {
    std::function<std::optional<std::string>(const std::string& key)> load;
    std::function<void(const std::string& key, const std::string& value)> save;
};

inline constexpr size_t kMaxCalendarItems = 5000;
inline constexpr long long kTransactionTtlMs = 24LL * 3'600'000;

class OutlookGraph {
public:
    OutlookGraph(OutlookSession& session, const Clock& clock, Persist persist = {});

    // One Graph request: method, the path relative to the Graph base (with its query) or a link this class returned,
    // the JSON body, and the page size the caller prefers (0: 50).
    Result<json> handle(const std::string& method, const std::string& path, const std::optional<json>& body, int page_size = 0);

    // Forgets every snapshot and starts a new generation: every stored delta link answers 410 next time.
    void reset();

    [[nodiscard]] OutlookSession& session() { return s_; }

private:
    struct Sequence {
        std::string key;                    // folder:<id> | calendar
        std::vector<std::string> to_fetch;  // message ids still to open
        std::vector<json> ready;            // items already built (calendar), and @removed entries
        std::map<std::string, std::string> snapshot;  // the snapshot to keep once the last page is delivered
        std::string delta_link;
        std::string folder_id;
    };

    Result<json> dispatch(const std::string& method, const std::string& path, const std::optional<json>& body, int page_size);
    Result<json> route(const std::string& method, const std::string& path, const std::multimap<std::string, std::string>& q,
                       const std::optional<json>& body, int page_size);
    Result<json> folders();
    Result<json> folder_delta(const std::string& folder_id, long long since, bool initial, int page_size);
    Result<json> calendar_delta(long long from, long long to, bool initial, int page_size);
    Result<json> next_page(const std::string& seq_id, int page_size);
    Result<json> list_messages(const std::string& folder_id, const std::multimap<std::string, std::string>& q);
    Result<json> get_schedule(const json& body);
    Result<json> create_event(const json& body);
    Result<std::string> folder_ref(const std::string& id_or_name);

    std::map<std::string, std::string> load_snapshot(const std::string& key);
    void save_snapshot(const std::string& key, const std::map<std::string, std::string>& snap);
    std::string delta_link(const std::string& key, const std::string& a, const std::string& b) const;

    OutlookSession& s_;
    const Clock& clock_;
    Persist persist_;
    std::mutex mx_;
    std::string generation_;
    long long next_seq_ = 1;
    std::map<std::string, Sequence> sequences_;
    std::map<std::string, std::map<std::string, std::string>> snapshots_;  // loaded or written this process
    std::map<std::string, std::pair<std::string, long long>> transactions_;  // transactionId -> event id, created at
};

// Graph JSON of the Outlook shapes (exposed for tests).
json message_to_graph(const OlMessage& m);
json event_to_graph(const OlEvent& e);
std::string change_key_of(const OlEvent& e);

}  // namespace wb::mail::outlook
