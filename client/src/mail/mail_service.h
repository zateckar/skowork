// client/mail-calendar in the host (add-mail-calendar): the configuration from policy, the Graph connection (auth and
// HTTP layer), the local copy and its background delta sync, the event feed, and the mail and calendar tools. Mail and
// calendar are unavailable while policy.mail.oauth is incomplete or policy is not acceptable for this build.
#pragma once

#include "mail/graph_auth.h"
#include "mail/graph_client.h"
#include "mail/mail_config.h"
#include "mail/mail_store.h"
#include "mail/mail_sync.h"
#include "runtime/tools.h"
#include "tools/files.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wb::mail {

struct MailEnv {
    AppStore* store = nullptr;
    const Clock* clock = nullptr;
    BrowserLauncher* launcher = nullptr;
    TokenBroker* broker = nullptr;
    PolicyLookup policy;
    std::function<bool(const std::string& tool)> tool_permitted;  // policy does not deny the tool outright
    bool dev_build = false;
    tools::Extractor extract;  // Document Text Extraction for attachments
    std::string login_hint;
    // Pause background work and the battery-saver or presentation deferral (client/desktop-host).
    std::function<bool()> paused;
    // A policy error event (Non-Graph Host In Release Build, Mock Auth In Release Build).
    std::function<void(const std::string& reason)> policy_error;
    // Silent acquisition needs the user: raise the "Connect mailbox" Question (client/work-inbox, system-origin Thread).
    std::function<void()> interaction_required;
    // Status for the settings section changed.
    std::function<void()> changed;
    bool background_sync = true;  // tests drive sync_now() themselves
};

// The Graph connection of the current configuration. Replaced when policy changes; tools hold the one they started with.
struct Connection {
    MailConfig cfg;
    std::unique_ptr<GraphAuth> auth;
    std::unique_ptr<GraphClient> graph;
};

class MailService {
public:
    explicit MailService(MailEnv env);
    ~MailService();
    MailService(const MailService&) = delete;
    MailService& operator=(const MailService&) = delete;

    // Reads policy, restores the connection silently and starts the background sync.
    void start();
    void stop();
    // Re-reads policy; a changed endpoint, tenant, app or scope set makes a new connection.
    void policy_changed();

    // The user chose Connect (settings section or the Connect mailbox Question).
    Status connect();
    // Disconnect mailbox: tokens deleted, the local copy cleared, features stop until the next Connect.
    void disconnect();
    // One sync cycle now, on the calling thread.
    Result<SyncReport> sync_now();
    // Wakes the background sync (after Connect, or "Sync now" in settings).
    void request_sync();

    [[nodiscard]] std::shared_ptr<Connection> connection();
    [[nodiscard]] MailConfig config();
    [[nodiscard]] bool available();
    [[nodiscard]] bool connected();
    [[nodiscard]] MailStore& store() { return store_; }
    [[nodiscard]] Rules rules();
    // The settings section: availability, connection state, account, last sync, counts, exclusions.
    [[nodiscard]] json status();
    Status add_exclusion(const std::string& kind, const std::string& value);
    Status remove_exclusion(const std::string& kind, const std::string& value);

    void on_event(std::function<void(const MailEvent&)> fn);
    // mail.* and calendar.* tools (client/mail-calendar Mail Tools And Risk Classes, Calendar Tools And Risk Classes).
    std::vector<runtime::ToolDef> tools();

    // Forget operations of client/local-storage: tokens and the local copy.
    Status forget_all();

    [[nodiscard]] const MailEnv& env() const { return env_; }
    [[nodiscard]] std::string account();  // the mailbox address (from /me), once connected
    // A colleague from the Graph user directory (client/knowledge-graph People Resolution; add-knowledge-graph design D5):
    // {displayName, mail, department, jobTitle, manager: {displayName, mail}}. Errors as GraphClient's, not_found included.
    Result<json> directory_user(const std::string& upn);

private:
    std::shared_ptr<Connection> make_connection(const MailConfig& cfg);
    void loop();
    void auth_changed(GraphAuthState s);

    MailEnv env_;
    MailStore store_;
    std::mutex mx_;
    std::shared_ptr<Connection> conn_;
    MailSync sync_;
    std::vector<std::function<void(const MailEvent&)>> listeners_;
    std::string account_;
    std::string last_error_;
    bool asked_interaction_ = false;

    std::thread thread_;
    std::mutex wake_mx_;
    std::condition_variable wake_;
    bool wake_now_ = false;
    std::atomic<bool> stop_{false};
    std::shared_ptr<CancelToken> cancel_ = std::make_shared<CancelToken>();
};

}  // namespace wb::mail
