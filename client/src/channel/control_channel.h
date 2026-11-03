// The managed connection to the control plane (client/control-channel): registration, heartbeat, the WebSocket
// push connection with ack/replay and auth-renew, client messages with an offline outbox, the version gate,
// revocation, and the single connected/disconnected state every other capability uses.
#pragma once

#include "base/json.h"
#include "base/time.h"
#include "channel/control_plane.h"
#include "identity/token_manager.h"
#include "platform/websocket.h"
#include "storage/app_store.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace wb {

// Push message types the fleet registry may send (control-plane/fleet-registry Push Message Types).
const std::set<std::string>& registered_push_types();

struct PushEnvelope {
    long long id = 0;
    std::string type;
    json target;
    long long issued_at_ms = 0;
    long long expires_at_ms = 0;
    json payload;
};

// Validates and normalises an envelope (timestamps given in seconds or milliseconds). Exposed for tests.
std::optional<PushEnvelope> parse_envelope(const std::string& text);

class ControlChannel {
public:
    struct Hooks {
        std::function<void(bool connected)> connection_changed;
        std::function<void(const std::string& min_version, bool blocked)> min_version;
        std::function<void()> revoked;        // stop Runs (client/control-channel Remote Session Revocation)
        std::function<void(bool locked)> locked_changed;
        std::function<void(const RegistrationResult&)> registered;
        std::function<std::string()> policy_version;  // for heartbeats
        std::function<std::string()> status;          // idle | running | paused
        std::function<std::string()> network_enforcement;  // proxy-only | os-enforced (client/tool-sandbox)
    };

    ControlChannel(ControlPlaneClient& cp, TokenManager& tokens, AppStore& store, const Clock& clock,
                   RegistrationInfo info, Hooks hooks);
    ~ControlChannel();

    void start();
    void stop();

    // Handler for a registered push type. Runs on the channel thread; keep it short. The message is acknowledged
    // after the handler returns.
    void on_push(const std::string& type, std::function<void(const PushEnvelope&)> handler);

    // Handler for the server's answers to client messages: kind "reply" (payload {message, ...}) or "error" (payload
    // {code, reason, message?}), with the ref the client message carried. Runs on the channel thread; keep it short.
    void on_reply(std::function<void(const std::string& kind, long long ref, const json& payload)> handler);

    // Queues a client message (remote-task-claim/accept/decline/status) in the outbox and sends it when connected.
    Status send(const std::string& type, long long ref, const json& payload);

    // Wakes the channel after a sign-in or sign-out.
    void auth_changed();

    [[nodiscard]] bool connected() const { return connected_.load(); }
    [[nodiscard]] bool push_up() const { return ws_up_.load(); }
    // When the channel was last found disconnected (Unix ms; 0 while connected or before the first check).
    [[nodiscard]] long long offline_since() const { return offline_since_.load(); }
    [[nodiscard]] bool locked() const { return locked_.load(); }
    [[nodiscard]] bool version_blocked() const { return version_blocked_.load(); }
    [[nodiscard]] std::string min_version() const;
    [[nodiscard]] Services services() const;
    [[nodiscard]] std::string installation_id() const { return info_.installation_id; }
    [[nodiscard]] long long last_ack() const { return last_ack_.load(); }
    [[nodiscard]] int auth_renews_sent() const { return auth_renews_.load(); }
    [[nodiscard]] int connects() const { return connects_.load(); }

    // Grace period before a dropped push connection counts as disconnected (client/control-channel: 60 s).
    static constexpr long long kDisconnectGraceMs = 60'000;

private:
    void push_loop();
    void tick_loop();
    Status ensure_registered();
    void handle_text(const std::string& text);
    void flush_outbox();
    void evaluate_state();
    void apply_min_version(const std::string& v);
    void set_locked(bool locked);
    std::string push_url() const;
    void send_auth_renew_if_needed();

    ControlPlaneClient& cp_;
    TokenManager& tokens_;
    AppStore& store_;
    const Clock& clock_;
    RegistrationInfo info_;
    Hooks hooks_;

    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::map<std::string, std::function<void(const PushEnvelope&)>> handlers_;
    std::function<void(const std::string&, long long, const json&)> reply_handler_;
    std::shared_ptr<WebSocket> ws_;
    Services services_;
    std::string min_version_;
    bool registered_ = false;
    int heartbeat_interval_s_ = 300;
    long long next_heartbeat_ms_ = 0;
    long long down_since_ms_ = 0;
    bool ever_up_ = false;
    std::string last_sent_token_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> ws_up_{false};
    std::atomic<bool> locked_{false};
    std::atomic<bool> version_blocked_{false};
    std::atomic<bool> outbox_dirty_{true};
    std::atomic<long long> last_ack_{0};
    std::atomic<long long> offline_since_{0};
    std::atomic<int> auth_renews_{0};
    std::atomic<int> connects_{0};
    std::thread push_thread_, tick_thread_;
};

}  // namespace wb
