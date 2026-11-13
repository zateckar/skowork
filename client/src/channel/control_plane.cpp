#include "channel/control_plane.h"

#include "base/strings.h"
#include "base/url.h"
#include "platform/log.h"

namespace wb {

void ControlPlaneClient::set_installation_id(std::string id) {
    const std::lock_guard lock(mx_);
    installation_id_ = std::move(id);
}

Result<HttpResponse> ControlPlaneClient::call(const std::string& method, const std::string& url, const std::string& body,
                                              const CallOptions& options) {
    HttpRequest req;
    req.method = method;
    req.url = starts_with(url, "https://") || starts_with(url, "http://") ? url : join_url(base_, url);
    req.body = body;
    req.connect_timeout = options.connect_timeout;
    req.idle_timeout = options.idle_timeout;
    req.on_chunk = options.on_chunk;
    req.cancel = options.cancel;
    {
        const std::lock_guard lock(mx_);
        if (!installation_id_.empty()) req.headers.emplace_back("X-Skowork-Installation", installation_id_);
    }
    for (const auto& h : options.headers) req.headers.push_back(h);
    if (!body.empty()) req.headers.emplace_back("Content-Type", options.content_type);
    req.headers.emplace_back("Accept", "application/json");

    auto token = tokens_.access_token();
    if (!token) return token.error();
    const size_t auth_index = req.headers.size();
    req.headers.emplace_back("Authorization", "Bearer " + *token);
    auto resp = http().send(req);
    if (!resp || resp->status != 401) return resp;

    // One refresh, one retry (client/identity).
    auto fresh = tokens_.refresh_after_unauthorized(*token);
    if (!fresh) return fresh.error();
    req.headers[auth_index].second = "Bearer " + *fresh;
    resp = http().send(req);
    if (resp && resp->status == 401) {
        tokens_.session_rejected("401 after refresh for " + method + " " + parse_url(req.url).value_or(Url{}).path);
        return make_error("unauthorized", "the control plane rejected the session");
    }
    return resp;
}

Result<json> ControlPlaneClient::call_json(const std::string& method, const std::string& url, const json& body) {
    auto resp = call(method, url, body.is_null() ? std::string{} : body.dump());
    if (!resp) return resp.error();
    auto j = parse_json(resp->body);
    if (resp->status < 200 || resp->status >= 300) {
        std::string code = "http_" + std::to_string(resp->status);
        std::string message = "HTTP " + std::to_string(resp->status);
        if (j) {
            if (const json* e = jobj(*j, "error")) {
                if (!jstr(*e, "code").empty()) code = jstr(*e, "code");
                message = jstr(*e, "message", message);
            }
        }
        return make_error(code, message);
    }
    if (!j) return json(nullptr);
    return std::move(*j);
}

Result<RegistrationResult> ControlPlaneClient::parse_registration(const json& j) {
    RegistrationResult r;
    r.installation_id = jstr(j, "installation_id");
    r.heartbeat_interval_s = static_cast<int>(jint(j, "heartbeat_interval_s", 300));
    if (r.heartbeat_interval_s < 10) r.heartbeat_interval_s = 10;
    r.min_version = jstr(j, "min_version");
    r.revoked = jbool(j, "revoked");
    if (const json* s = jobj(j, "services")) {
        r.services.gateway = jstr(*s, "gateway");
        r.services.policy = jstr(*s, "policy");
        r.services.skills = jstr(*s, "skills");
        r.services.telemetry = jstr(*s, "telemetry");
        r.services.tasks = jstr(*s, "tasks");
        r.services.mcp = jstr(*s, "mcp");
        r.services.push = jstr(*s, "push");
    }
    // Service URLs must be acceptable endpoints (https, or loopback in dev builds).
    for (const auto* u : {&r.services.gateway, &r.services.policy, &r.services.skills, &r.services.telemetry,
                          &r.services.tasks, &r.services.mcp, &r.services.push})
        if (!u->empty() && !HttpClient::url_allowed(*u)) return make_error("registration", "insecure service URL");
    if (r.installation_id.empty()) return make_error("registration", "no installation id in the response");
    return r;
}

Result<RegistrationResult> ControlPlaneClient::register_installation(const RegistrationInfo& info) {
    json body = {{"installation_id", info.installation_id}, {"machine_name", info.machine_name},
                 {"user_principal", info.user_principal},   {"version", info.version},
                 {"channel", info.channel},                 {"commit", info.commit},
                 {"os_build", info.os_build},               {"hardware_class", info.hardware_class}};
    set_installation_id(info.installation_id);
    auto j = call_json("POST", "api/v1/fleet/register", body);
    if (!j) return j.error();
    return parse_registration(*j);
}

Result<HeartbeatResult> ControlPlaneClient::heartbeat(const std::string& installation_id,
                                                      const std::string& policy_version, const std::string& status,
                                                      const std::string& network_enforcement) {
    json body = {{"installation_id", installation_id}, {"policy_version", policy_version}, {"status", status}};
    if (!network_enforcement.empty()) body["network_enforcement"] = network_enforcement;
    auto j = call_json("POST", "api/v1/fleet/heartbeat", body);
    if (!j) return j.error();
    HeartbeatResult r;
    r.heartbeat_interval_s = static_cast<int>(jint(*j, "heartbeat_interval_s", 300));
    if (r.heartbeat_interval_s < 10) r.heartbeat_interval_s = 10;
    r.min_version = jstr(*j, "min_version");
    return r;
}

}  // namespace wb
