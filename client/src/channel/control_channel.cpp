#include "channel/control_channel.h"

#include "base/backoff.h"
#include "base/strings.h"
#include "base/url.h"
#include "base/version.h"
#include "platform/log.h"

namespace wb {
namespace {

constexpr char kLog[] = "channel";
constexpr char kLastAck[] = "channel.last_ack";
constexpr char kServices[] = "channel.services";
constexpr char kMinVersion[] = "channel.min_version";

long long to_ms(long long v) { return v > 0 && v < 100'000'000'000LL ? v * 1000 : v; }

json services_json(const Services& s) {
    return {{"gateway", s.gateway},     {"policy", s.policy}, {"skills", s.skills}, {"telemetry", s.telemetry},
            {"tasks", s.tasks},         {"mcp", s.mcp},       {"push", s.push}};
}

}  // namespace

const std::set<std::string>& registered_push_types() {
    static const std::set<std::string> types = {"revocation",    "minimum-version", "policy-changed",
                                                "skills-changed", "skill-revoked",   "remote-task",
                                                "remote-task-claimed", "remote-task-cancelled",
                                                "chat-message", "chat-changed", "presence-changed"};
    return types;
}

std::optional<PushEnvelope> parse_envelope(const std::string& text) {
    auto j = parse_json(text);
    if (!j || !j->is_object()) return std::nullopt;
    PushEnvelope e;
    e.id = jint(*j, "id", 0);
    e.type = jstr(*j, "type");
    if (e.id <= 0 || e.type.empty()) return std::nullopt;
    if (const auto it = j->find("target"); it != j->end() && it->is_object()) e.target = *it;
    e.issued_at_ms = to_ms(jint(*j, "issued_at", 0));
    e.expires_at_ms = to_ms(jint(*j, "expires_at", 0));
    if (const auto it = j->find("payload"); it != j->end()) e.payload = *it;
    return e;
}

ControlChannel::ControlChannel(ControlPlaneClient& cp, TokenManager& tokens, AppStore& store, const Clock& clock,
                               RegistrationInfo info, Hooks hooks)
    : cp_(cp), tokens_(tokens), store_(store), clock_(clock), info_(std::move(info)), hooks_(std::move(hooks)) {
    if (const auto v = store_.setting(kLastAck)) last_ack_ = std::atoll(v->c_str());
    if (const auto s = store_.setting(kServices)) {
        if (auto j = parse_json(*s)) {
            services_.gateway = jstr(*j, "gateway");
            services_.policy = jstr(*j, "policy");
            services_.skills = jstr(*j, "skills");
            services_.telemetry = jstr(*j, "telemetry");
            services_.tasks = jstr(*j, "tasks");
            services_.mcp = jstr(*j, "mcp");
            services_.push = jstr(*j, "push");
        }
    }
    if (const auto v = store_.setting(kMinVersion)) {
        min_version_ = *v;
        version_blocked_ = below_minimum(info_.version, *v);
    }
    cp_.set_installation_id(info_.installation_id);
}

ControlChannel::~ControlChannel() { stop(); }

void ControlChannel::start() {
    down_since_ms_ = clock_.unix_millis();
    push_thread_ = std::thread([this] { push_loop(); });
    tick_thread_ = std::thread([this] { tick_loop(); });
}

void ControlChannel::stop() {
    if (stopping_.exchange(true)) return;
    std::shared_ptr<WebSocket> ws;
    {
        const std::lock_guard lock(mx_);
        ws = ws_;
    }
    if (ws) ws->close(1001, "client stopping");
    cv_.notify_all();
    if (push_thread_.joinable()) push_thread_.join();
    if (tick_thread_.joinable()) tick_thread_.join();
}

void ControlChannel::on_push(const std::string& type, std::function<void(const PushEnvelope&)> handler) {
    const std::lock_guard lock(mx_);
    handlers_[type] = std::move(handler);
}

void ControlChannel::on_reply(std::function<void(const std::string&, long long, const json&)> handler) {
    const std::lock_guard lock(mx_);
    reply_handler_ = std::move(handler);
}

void ControlChannel::auth_changed() {
    {
        const std::lock_guard lock(mx_);
        if (tokens_.state() != AuthState::signed_in) registered_ = false;
    }
    cv_.notify_all();
    evaluate_state();
}

std::string ControlChannel::min_version() const {
    const std::lock_guard lock(mx_);
    return min_version_;
}

Services ControlChannel::services() const {
    const std::lock_guard lock(mx_);
    return services_;
}

Status ControlChannel::send(const std::string& type, long long ref, const json& payload) {
    if (Status s = store_.outbox_push(type, ref, payload.dump()); !s) return s;
    outbox_dirty_ = true;
    cv_.notify_all();
    return {};
}

void ControlChannel::set_locked(bool locked) {
    if (locked_.exchange(locked) == locked) return;
    log::warn(kLog, locked ? "installation locked: access revoked" : "installation unlocked");
    if (hooks_.locked_changed) hooks_.locked_changed(locked);
    evaluate_state();
}

void ControlChannel::apply_min_version(const std::string& v) {
    bool blocked = false;
    {
        const std::lock_guard lock(mx_);
        min_version_ = v;
        blocked = below_minimum(info_.version, v);
    }
    store_.set_setting(kMinVersion, v);
    version_blocked_ = blocked;
    if (hooks_.min_version) hooks_.min_version(v, blocked);
}

std::string ControlChannel::push_url() const {
    std::string base;
    {
        const std::lock_guard lock(mx_);
        base = services_.push;
    }
    if (base.empty()) {
        base = join_url(cp_.base_url(), "api/v1/fleet/push");
        if (starts_with(base, "https://")) base = "wss://" + base.substr(8);
        else if (starts_with(base, "http://")) base = "ws://" + base.substr(7);
    }
    return base + (base.find('?') == std::string::npos ? "?" : "&") + "installation_id=" +
           url_encode(info_.installation_id) + "&last_ack=" + std::to_string(last_ack_.load());
}

Status ControlChannel::ensure_registered() {
    {
        const std::lock_guard lock(mx_);
        if (registered_) return {};
    }
    RegistrationInfo info = info_;
    if (const auto upn = tokens_.user_principal(); !upn.empty()) info.user_principal = upn;
    auto r = cp_.register_installation(info);
    if (!r) {
        if (iequals(r.error().code, "http_403") || iequals(r.error().code, "revoked") || iequals(r.error().code, "denied"))
            set_locked(true);
        if (iequals(r.error().code, "revoked") || iequals(r.error().code, "denied")) tokens_.end_offline_session_revoked();
        return r.error();
    }
    if (r->revoked) {
        set_locked(true);
        tokens_.end_offline_session_revoked();
        return make_error("revoked", "installation revoked");
    }
    set_locked(false);
    {
        const std::lock_guard lock(mx_);
        registered_ = true;
        services_ = r->services;
        heartbeat_interval_s_ = r->heartbeat_interval_s;
        next_heartbeat_ms_ = clock_.unix_millis() + static_cast<long long>(heartbeat_interval_s_) * 1000;
    }
    store_.set_setting(kServices, services_json(r->services).dump());
    apply_min_version(r->min_version);
    log::info(kLog, "registered installation " + info_.installation_id);
    if (hooks_.registered) hooks_.registered(*r);
    return {};
}

void ControlChannel::push_loop() {
    Backoff backoff(std::chrono::milliseconds(1000), std::chrono::minutes(5));
    while (!stopping_) {
        // Wait until a session exists and the installation is not locked.
        {
            std::unique_lock lock(mx_);
            cv_.wait_for(lock, std::chrono::seconds(5), [&] {
                return stopping_.load() || (tokens_.state() == AuthState::signed_in && !locked_.load());
            });
        }
        if (stopping_) break;
        if (tokens_.state() != AuthState::signed_in || locked_) continue;

        auto wait = [&](std::chrono::milliseconds d) {
            std::unique_lock lock(mx_);
            cv_.wait_for(lock, d, [&] { return stopping_.load(); });
        };

        if (Status s = ensure_registered(); !s) {
            const auto d = backoff.next();
            log::warn(kLog, "registration failed (" + s.error().code + "): " + s.error().message + "; retry in " +
                                std::to_string(d.count()) + " ms");
            wait(d);
            continue;
        }
        auto token = tokens_.access_token();
        if (!token) {
            wait(backoff.next());
            continue;
        }
        auto ws = WebSocket::connect(push_url(), {{"Authorization", "Bearer " + *token},
                                                   {"X-Skowork-Installation", info_.installation_id}},
                                     std::chrono::seconds(15));
        if (!ws) {
            const auto& code = ws.error().code;
            if (code == "http_401") tokens_.refresh_after_unauthorized(*token);
            if (code == "http_403") {
                // Revoked or denied: re-register to learn which, then stay locked.
                {
                    const std::lock_guard lock(mx_);
                    registered_ = false;
                }
                if (Status s = ensure_registered(); !s && !iequals(s.error().code, "revoked")) set_locked(true);
            }
            const auto d = backoff.next();
            log::info(kLog, "push connect failed (" + code + "); retry in " + std::to_string(d.count()) + " ms");
            wait(d);
            continue;
        }
        std::shared_ptr<WebSocket> sock(std::move(*ws).release());
        {
            const std::lock_guard lock(mx_);
            ws_ = sock;
            last_sent_token_ = *token;
            ever_up_ = true;
        }
        ws_up_ = true;
        ++connects_;
        backoff.reset();
        outbox_dirty_ = true;
        log::info(kLog, "push connected (last_ack " + std::to_string(last_ack_.load()) + ")");
        evaluate_state();
        cv_.notify_all();

        unsigned short close_status = 0;
        for (;;) {
            auto msg = sock->receive();
            if (!msg) {
                if (!stopping_) log::info(kLog, "push connection lost: " + msg.error().message);
                break;
            }
            if (msg->kind == WsMessage::Kind::closed) {
                close_status = msg->close_status;
                log::info(kLog, "push connection closed by server (" + std::to_string(close_status) + ")");
                break;
            }
            handle_text(msg->text);
        }
        sock->abort();
        {
            const std::lock_guard lock(mx_);
            ws_.reset();
            down_since_ms_ = clock_.unix_millis();
        }
        ws_up_ = false;
        evaluate_state();
        if (stopping_) break;
        if (close_status == 4403) {
            const std::lock_guard lock(mx_);
            registered_ = false;
        }
        if (close_status == 4401) {
            tokens_.drop_access_token();  // reconnect at once with a fresh token
            continue;
        }
        wait(backoff.next());
    }
}

void ControlChannel::handle_text(const std::string& text) {
    auto j = parse_json(text);
    if (j && (jstr(*j, "type") == "error" || jstr(*j, "type") == "reply")) {
        const std::string kind = jstr(*j, "type");
        const json payload = jobj(*j, "payload") ? *jobj(*j, "payload") : json::object();
        if (kind == "error") log::warn(kLog, "server refused a client message: " + jstr(payload, "reason"));
        const json* r = jobj(*j, "ref");
        const long long ref = r && r->is_number_integer() ? r->get<long long>() : 0;
        std::function<void(const std::string&, long long, const json&)> handler;
        {
            const std::lock_guard lock(mx_);
            handler = reply_handler_;
        }
        if (handler && ref > 0) handler(kind, ref, payload);
        return;
    }
    auto env = parse_envelope(text);
    if (!env) {
        log::warn(kLog, "ignored a push message that failed validation");
        return;
    }
    auto ack = [&] {
        std::shared_ptr<WebSocket> ws;
        {
            const std::lock_guard lock(mx_);
            ws = ws_;
        }
        if (ws) ws->send_text(json{{"type", "ack"}, {"ref", env->id}}.dump());
        if (env->id > last_ack_) {
            last_ack_ = env->id;
            store_.set_setting(kLastAck, std::to_string(env->id));
        }
    };
    // Addressing (client/control-channel Push Message Integrity).
    const std::string target_inst = jstr(env->target, "installation_id");
    if (!target_inst.empty() && target_inst != info_.installation_id) {
        log::warn(kLog, "ignored push " + std::to_string(env->id) + " addressed to another installation");
        store_.security_event("push_misaddressed", "push for another installation ignored");
        ack();
        return;
    }
    if (env->id <= last_ack_) {  // replayed duplicate
        ack();
        return;
    }
    if (env->expires_at_ms > 0 && env->expires_at_ms < clock_.unix_millis()) {
        log::info(kLog, "push " + std::to_string(env->id) + " expired before delivery");
        ack();
        return;
    }
    if (!registered_push_types().count(env->type)) {
        log::warn(kLog, "ignored push of unregistered type '" + utf8_truncate(env->type, 64) + "'");
        ack();
        return;
    }
    std::function<void(const PushEnvelope&)> handler;
    {
        const std::lock_guard lock(mx_);
        const auto it = handlers_.find(env->type);
        if (it != handlers_.end()) handler = it->second;
    }
    // Built-in handling of this capability's own types.
    if (env->type == "minimum-version") {
        apply_min_version(jstr(env->payload, "min_version", jstr(env->payload, "version")));
    } else if (env->type == "revocation") {
        log::warn(kLog, "revocation received");
        if (hooks_.revoked) hooks_.revoked();
        tokens_.drop_access_token();
        {
            const std::lock_guard lock(mx_);
            registered_ = false;
        }
        ack();
        // Close the connection; the push loop re-registers and stays locked if the control plane keeps refusing.
        std::shared_ptr<WebSocket> ws;
        {
            const std::lock_guard lock(mx_);
            ws = ws_;
        }
        if (ws) ws->close(1000, "revoked");
        return;
    }
    if (handler) {
        handler(*env);
    } else if (env->type != "minimum-version") {
        log::info(kLog, "no handler yet for push type " + env->type);
    }
    ack();
}

void ControlChannel::flush_outbox() {
    std::shared_ptr<WebSocket> ws;
    {
        const std::lock_guard lock(mx_);
        ws = ws_;
    }
    if (!ws) return;
    outbox_dirty_ = false;
    for (const auto& item : store_.outbox_pending()) {
        json msg = {{"type", item.type}, {"ref", item.ref}};
        if (auto p = parse_json(item.payload)) msg["payload"] = std::move(*p);
        if (Status s = ws->send_text(msg.dump()); !s) {
            outbox_dirty_ = true;
            return;
        }
        store_.outbox_remove(item.id);
    }
}

void ControlChannel::send_auth_renew_if_needed() {
    std::shared_ptr<WebSocket> ws;
    std::string last;
    {
        const std::lock_guard lock(mx_);
        ws = ws_;
        last = last_sent_token_;
    }
    if (!ws) return;
    auto token = tokens_.access_token();  // refreshes when less than 60 s remain
    if (!token || *token == last) return;
    if (ws->send_text(json{{"type", "auth-renew"}, {"payload", {{"access_token", *token}}}}.dump())) {
        const std::lock_guard lock(mx_);
        last_sent_token_ = *token;
        ++auth_renews_;
        log::debug(kLog, "auth-renew sent");
    }
}

void ControlChannel::evaluate_state() {
    const long long now = clock_.unix_millis();
    bool push_ok = false;
    {
        const std::lock_guard lock(mx_);
        push_ok = ws_up_ || (ever_up_ && now - down_since_ms_ <= kDisconnectGraceMs);
    }
    const bool auth_ok = tokens_.state() == AuthState::signed_in &&
                         (tokens_.access_token_expiry() > clock_.unix_seconds() || ws_up_);
    const bool now_connected = auth_ok && push_ok && !locked_;
    if (now_connected)
        offline_since_ = 0;
    else if (offline_since_ == 0)
        offline_since_ = now;
    if (connected_.exchange(now_connected) != now_connected) {
        log::info(kLog, now_connected ? "connected" : "disconnected");
        if (hooks_.connection_changed) hooks_.connection_changed(now_connected);
    }
}

void ControlChannel::tick_loop() {
    while (!stopping_) {
        {
            std::unique_lock lock(mx_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_.load(); });
        }
        if (stopping_) break;
        evaluate_state();
        if (!ws_up_) continue;
        send_auth_renew_if_needed();
        if (outbox_dirty_) flush_outbox();
        // Heartbeat (client/control-channel Heartbeat).
        bool due = false;
        {
            const std::lock_guard lock(mx_);
            due = registered_ && clock_.unix_millis() >= next_heartbeat_ms_;
            if (due) next_heartbeat_ms_ = clock_.unix_millis() + static_cast<long long>(heartbeat_interval_s_) * 1000;
        }
        if (due) {
            const std::string pv = hooks_.policy_version ? hooks_.policy_version() : "";
            const std::string st = hooks_.status ? hooks_.status() : "idle";
            const std::string ne = hooks_.network_enforcement ? hooks_.network_enforcement() : "";
            auto hb = cp_.heartbeat(info_.installation_id, pv, st, ne);
            if (hb) {
                {
                    const std::lock_guard lock(mx_);
                    heartbeat_interval_s_ = hb->heartbeat_interval_s;
                }
                if (!hb->min_version.empty() && hb->min_version != min_version()) apply_min_version(hb->min_version);
            } else {
                log::warn(kLog, "heartbeat failed: " + hb.error().message);
                if (iequals(hb.error().code, "http_403") || iequals(hb.error().code, "denied")) set_locked(true);
            }
        }
    }
}

}  // namespace wb
