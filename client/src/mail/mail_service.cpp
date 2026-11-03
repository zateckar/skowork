#include "mail/mail_service.h"

#include "base/url.h"

#include "platform/log.h"

namespace wb::mail {
namespace {

constexpr char kLog[] = "mail";
constexpr char kAccount[] = "mail.account";

bool same_connection(const MailConfig& a, const MailConfig& b) {
    return a.available == b.available && a.graph_base_url == b.graph_base_url && a.auth == b.auth && a.authority == b.authority &&
           a.tenant_id == b.tenant_id && a.client_id == b.client_id && a.scopes == b.scopes;
}

}  // namespace

MailService::MailService(MailEnv env) : env_(std::move(env)), store_(env_.store->db()), sync_(store_, *env_.clock) {
    sync_.on_event([this](const MailEvent& e) {
        std::vector<std::function<void(const MailEvent&)>> listeners;
        {
            const std::lock_guard lock(mx_);
            listeners = listeners_;
        }
        for (const auto& fn : listeners) fn(e);
    });
}

MailService::~MailService() { stop(); }

std::shared_ptr<Connection> MailService::make_connection(const MailConfig& cfg) {
    auto c = std::make_shared<Connection>();
    c->cfg = cfg;
    c->auth = std::make_unique<GraphAuth>(cfg, *env_.store, *env_.clock, *env_.launcher, env_.broker);
    c->auth->set_login_hint(env_.login_hint);
    c->graph = std::make_unique<GraphClient>(cfg.graph_base_url, *c->auth, env_.dev_build);
    c->auth->on_state_changed([this](GraphAuthState s) { auth_changed(s); });
    return c;
}

void MailService::auth_changed(GraphAuthState s) {
    bool ask = false;
    {
        const std::lock_guard lock(mx_);
        if (s == GraphAuthState::interaction_required && !asked_interaction_) ask = asked_interaction_ = true;
        if (s == GraphAuthState::connected) asked_interaction_ = false;
    }
    if (ask && env_.interaction_required) env_.interaction_required();
    if (s == GraphAuthState::connected) request_sync();
    if (env_.changed) env_.changed();
}

void MailService::start() {
    const MailConfig cfg = read_config(env_.policy, env_.tool_permitted, env_.dev_build);
    if (!cfg.available && cfg.unavailable_code == "policy_error" && env_.policy_error) env_.policy_error(cfg.unavailable_reason);
    auto conn = make_connection(cfg);
    {
        const std::lock_guard lock(mx_);
        conn_ = conn;
        account_ = env_.store->setting(kAccount).value_or("");
    }
    sync_.set_config(cfg);
    conn->auth->start();
    if (env_.background_sync && !thread_.joinable()) thread_ = std::thread([this] { loop(); });
}

void MailService::stop() {
    stop_ = true;
    cancel_->cancel();
    {
        const std::lock_guard lock(wake_mx_);
        wake_now_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MailService::policy_changed() {
    const MailConfig cfg = read_config(env_.policy, env_.tool_permitted, env_.dev_build);
    std::shared_ptr<Connection> old;
    {
        const std::lock_guard lock(mx_);
        old = conn_;
    }
    sync_.set_config(cfg);
    if (old && same_connection(old->cfg, cfg)) {
        // Exclusions, labels or the window changed: the connection stays; the next cycle applies them.
        old->cfg = cfg;
        store_.restart_sync();
        request_sync();
        if (env_.changed) env_.changed();
        return;
    }
    if (!cfg.available && cfg.unavailable_code == "policy_error" && env_.policy_error) env_.policy_error(cfg.unavailable_reason);
    auto conn = make_connection(cfg);
    {
        const std::lock_guard lock(mx_);
        conn_ = conn;
    }
    conn->auth->start();
    if (env_.changed) env_.changed();
}

std::shared_ptr<Connection> MailService::connection() {
    const std::lock_guard lock(mx_);
    return conn_;
}

MailConfig MailService::config() {
    auto c = connection();
    return c ? c->cfg : MailConfig{};
}

bool MailService::available() {
    auto c = connection();
    return c && c->cfg.available;
}

bool MailService::connected() {
    auto c = connection();
    return c && c->cfg.available && c->auth->state() == GraphAuthState::connected;
}

Rules MailService::rules() { return sync_.rules(); }

std::string MailService::account() {
    const std::lock_guard lock(mx_);
    return account_;
}

Result<json> MailService::directory_user(const std::string& upn) {
    auto c = connection();
    if (!c || !c->cfg.available) return make_error("unavailable", c ? c->cfg.unavailable_reason : "mail is not started");
    if (!c->cfg.directory_enabled) return make_error("unavailable", "directory lookups are not enabled by policy");
    if (c->auth->state() != GraphAuthState::connected) return make_error("not_connected", "the mailbox is not connected");
    GraphCall u;
    u.path = "/users/" + url_encode(upn) + "?$select=displayName,mail,userPrincipalName,department,jobTitle";
    u.cancel = cancel_;
    auto j = c->graph->call(u);
    if (!j) return j.error();
    if (!j->is_object()) return make_error("malformed", "not a user");
    json out = {{"displayName", jstr(*j, "displayName")}, {"mail", jstr(*j, "mail", jstr(*j, "userPrincipalName"))},
                {"department", jstr(*j, "department")}, {"jobTitle", jstr(*j, "jobTitle")}};
    GraphCall m;
    m.path = "/users/" + url_encode(upn) + "/manager?$select=displayName,mail,userPrincipalName";
    m.cancel = cancel_;
    if (auto mj = c->graph->call(m); mj && mj->is_object())
        out["manager"] = {{"displayName", jstr(*mj, "displayName")}, {"mail", jstr(*mj, "mail", jstr(*mj, "userPrincipalName"))}};
    else if (!mj && mj.error().code != "not_found")
        return mj.error();
    return out;
}

Status MailService::connect() {
    auto c = connection();
    if (!c) return make_error("unavailable", "mail is not started");
    {
        const std::lock_guard lock(mx_);
        asked_interaction_ = false;
    }
    return c->auth->connect();
}

void MailService::disconnect() {
    if (auto c = connection()) c->auth->disconnect();
    store_.clear();
    {
        const std::lock_guard lock(mx_);
        account_.clear();
    }
    (void)env_.store->delete_setting(kAccount);
    if (env_.changed) env_.changed();
}

Status MailService::forget_all() {
    if (auto c = connection()) c->auth->disconnect();
    store_.clear(true);
    (void)env_.store->delete_setting(kAccount);
    return GraphAuth::forget_secrets(*env_.store);
}

void MailService::request_sync() {
    {
        const std::lock_guard lock(wake_mx_);
        wake_now_ = true;
    }
    wake_.notify_all();
}

Result<SyncReport> MailService::sync_now() {
    auto c = connection();
    if (!c || !c->cfg.available) return make_error("unavailable", c ? c->cfg.unavailable_reason : "mail is not started");
    if (c->auth->state() != GraphAuthState::connected) return make_error("not_connected", "the mailbox is not connected");
    if (account().empty()) {
        GraphCall me;
        me.path = "/me?$select=mail,userPrincipalName,displayName";
        me.cancel = cancel_;
        if (auto j = c->graph->call(me); j && j->is_object()) {
            std::string a = jstr(*j, "mail");
            if (a.empty()) a = jstr(*j, "userPrincipalName");
            (void)env_.store->set_setting(kAccount, a);
            const std::lock_guard lock(mx_);
            account_ = a;
        }
    }
    auto r = sync_.sync_once(*c->graph, cancel_);
    {
        const std::lock_guard lock(mx_);
        last_error_ = r ? std::string{} : r.error().code + ": " + r.error().message;
    }
    if (!r) log::warn(kLog, "sync: " + r.error().message);
    if (env_.changed) env_.changed();
    return r;
}

void MailService::loop() {
    long long next_due = 0;
    while (!stop_) {
        const long long now = env_.clock->unix_millis();
        bool woken = false;
        {
            std::unique_lock lock(wake_mx_);
            const long long wait = next_due > now ? next_due - now : 0;
            if (wait > 0) wake_.wait_for(lock, std::chrono::milliseconds(std::min<long long>(wait, 60'000)), [this] { return wake_now_; });
            woken = wake_now_;
            wake_now_ = false;
        }
        if (stop_) break;
        if (!woken && env_.clock->unix_millis() < next_due) continue;
        const MailConfig cfg = config();
        const long long interval = static_cast<long long>(std::max(1, cfg.sync_interval_minutes)) * 60'000;
        next_due = env_.clock->unix_millis() + interval;
        // Background work: deferred while paused (Pause background work, battery saver, presentation mode).
        if (env_.paused && env_.paused()) {
            next_due = env_.clock->unix_millis() + 60'000;
            continue;
        }
        if (!connected()) continue;
        (void)sync_now();
    }
}

json MailService::status() {
    auto c = connection();
    json j;
    const MailConfig cfg = c ? c->cfg : MailConfig{};
    j["available"] = cfg.available;
    j["unavailable_code"] = cfg.unavailable_code;
    j["unavailable_reason"] = cfg.unavailable_reason;
    j["state"] = c ? to_string(c->auth->state()) : "disconnected";
    j["error"] = c ? c->auth->last_error() : std::string{};
    j["mock"] = cfg.auth == "mock";
    j["account"] = account();
    j["scopes"] = cfg.scopes;
    const long long last = store_.last_sync();
    j["last_sync"] = last ? iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(last))) : "";
    {
        const std::lock_guard lock(mx_);
        j["sync_error"] = last_error_;
    }
    const auto counts = store_.counts();
    j["messages"] = counts.messages;
    j["events"] = counts.events;
    j["withheld"] = counts.withheld;
    j["policy_exclusions"] = {{"folders", cfg.exclusions.folders}, {"categories", cfg.exclusions.categories}, {"private_items", cfg.exclusions.private_items}};
    j["user_exclusions"] = {{"folders", store_.user_exclusions("folder")}, {"categories", store_.user_exclusions("category")}};
    return j;
}

Status MailService::add_exclusion(const std::string& kind, const std::string& value) {
    Status s = store_.add_user_exclusion(kind, value);
    if (s) {
        store_.restart_sync();  // drops what is now excluded
        request_sync();
        if (env_.changed) env_.changed();
    }
    return s;
}

Status MailService::remove_exclusion(const std::string& kind, const std::string& value) {
    // Only the user's own exclusions can be removed; policy's are not in this table.
    Status s = store_.remove_user_exclusion(kind, value);
    if (s) {
        if (kind == "folder") {
            // The folder is synced again from the start.
            if (auto f = store_.folder(value)) store_.drop_sync_state("folder:" + f->id);
        }
        request_sync();
        if (env_.changed) env_.changed();
    }
    return s;
}

void MailService::on_event(std::function<void(const MailEvent&)> fn) {
    const std::lock_guard lock(mx_);
    listeners_.push_back(std::move(fn));
}

}  // namespace wb::mail
