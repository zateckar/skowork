// Authenticated calls to the control plane (client/identity Access Token Freshness Before Every Control-Plane
// Call) and the registration and heartbeat API (client/control-channel; design D10).
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "identity/token_manager.h"
#include "platform/http.h"

#include <functional>
#include <mutex>
#include <string>

namespace wb {

struct Services {
    std::string gateway, policy, skills, telemetry, tasks, mcp, push;
};

struct RegistrationInfo {
    std::string installation_id, machine_name, user_principal, version, channel, commit, os_build, hardware_class;
};

struct RegistrationResult {
    std::string installation_id;
    int heartbeat_interval_s = 300;
    std::string min_version;
    bool revoked = false;
    Services services;
};

struct HeartbeatResult {
    int heartbeat_interval_s = 300;
    std::string min_version;
};

struct CallOptions {
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds idle_timeout{30000};
    std::function<bool(std::string_view)> on_chunk;
    std::shared_ptr<CancelToken> cancel;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string content_type = "application/json";
};

class ControlPlaneClient {
public:
    ControlPlaneClient(std::string base_url, TokenManager& tokens) : base_(std::move(base_url)), tokens_(tokens) {}

    // Sends a request with a fresh bearer token. On 401 it refreshes once and retries once; a second 401 enters the
    // sign-in required state and returns error "unauthorized". `url` may be absolute or relative to the base URL.
    Result<HttpResponse> call(const std::string& method, const std::string& url, const std::string& body = {},
                              const CallOptions& options = {});
    Result<json> call_json(const std::string& method, const std::string& url, const json& body = nullptr);

    Result<RegistrationResult> register_installation(const RegistrationInfo& info);
    Result<HeartbeatResult> heartbeat(const std::string& installation_id, const std::string& policy_version,
                                      const std::string& status, const std::string& network_enforcement = {});

    void set_installation_id(std::string id);
    [[nodiscard]] const std::string& base_url() const { return base_; }

    static Result<RegistrationResult> parse_registration(const json& j);

private:
    std::string base_;
    TokenManager& tokens_;
    std::mutex mx_;
    std::string installation_id_;
};

}  // namespace wb
