// In-process egress checks for client tools (client/policy-enforcement Network Egress Rules For Client Tools). Every
// request a tool makes - web.fetch, remote MCP, and each agent-browser navigation, redirect and subresource - is
// checked here before connecting. Platform connections (control plane, identity provider, Microsoft Graph sync) do not
// use this guard; they are limited to their own hosts by their owning specs, and the guard never exempts those hosts
// for tools (Platform Connections Exempt From Egress Rules).
#pragma once

#include "platform/http.h"
#include "policy/core.h"

#include <functional>
#include <memory>

namespace wb::policy {

class EgressGuard {
public:
    explicit EgressGuard(std::function<std::shared_ptr<const EffectivePolicy>()> policy) : policy_(std::move(policy)) {}

    // The interface the agent browser calls for every request. Without a valid policy every request is denied.
    [[nodiscard]] RuleVerdict check_url(const std::string& url) const;

private:
    std::function<std::shared_ptr<const EffectivePolicy>()> policy_;
};

// Resolves a redirect Location against the URL that returned it.
std::string resolve_location(const std::string& base, const std::string& location);

// Sends `req` with redirects followed by hand, checking the first URL and every redirect target before connecting.
// A denied URL is not contacted: the result is error "policy_denied" and `denied` holds the rule. A redirect from https
// to http is refused (error "insecure_redirect"). `before_send` may adjust each hop's request (for example per-host
// authentication); `final_url` receives the URL of the last hop.
Result<HttpResponse> guarded_fetch(HttpClient& client, HttpRequest req, const EgressGuard& guard, int max_redirects = 10,
                                   RuleVerdict* denied = nullptr, const std::function<void(HttpRequest&)>& before_send = {},
                                   std::string* final_url = nullptr);

}  // namespace wb::policy
