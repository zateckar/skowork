// Mail and calendar configuration from policy (client/mail-calendar Graph As Sole Mail And Calendar Backend, Graph
// Platform Connection, Entra OAuth Delegated Access, Least-Privilege Scopes, Mock Authentication Restricted To
// Development Builds, exclusions, labels and the sync window).
#pragma once

#include "base/json.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::mail {

// The Graph host set, compiled in (design D4): only these are platform connections in release builds.
inline constexpr const char* kGraphHosts[] = {"graph.microsoft.com", "graph.microsoft.us", "dod-graph.microsoft.us",
                                              "microsoftgraph.chinacloudapi.cn"};
inline constexpr const char* kDefaultGraphBase = "https://graph.microsoft.com/v1.0";

struct Exclusions {
    std::vector<std::string> folders;     // well-known or display names
    std::vector<std::string> categories;
    bool private_items = true;
};

struct SyncWindow {
    int mail_days = 180;
    int calendar_past_days = 90;
    int calendar_future_days = 365;
};

struct MailConfig {
    // False while policy.mail.oauth is incomplete ("not_configured") or policy is not acceptable ("policy_error").
    bool available = false;
    std::string unavailable_code;    // not_configured | policy_error
    std::string unavailable_reason;  // for the settings section and the policy error event
    std::string graph_base_url = kDefaultGraphBase;  // without a trailing slash
    std::string auth = "oauth";                      // oauth | mock (development builds only)
    std::string authority, tenant_id, client_id;
    std::vector<std::string> scopes;  // delegated Graph scopes, without the resource prefix
    bool mail_enabled = true, calendar_enabled = true, send_enabled = true;
    // Directory lookups for knowledge enrichment (policy.knowledge.directory_enrichment): User.Read.All.
    bool directory_enabled = true;
    std::vector<std::string> excluded_labels;  // label GUIDs, lower case
    bool exclude_unlabelled = false;
    Exclusions exclusions;
    SyncWindow window;
    int sync_interval_minutes = 5;

    [[nodiscard]] std::string authorize_endpoint() const;
    [[nodiscard]] std::string token_endpoint() const;
    // The scope parameter of token requests: the Graph scopes with their resource prefix, plus offline_access.
    [[nodiscard]] std::string scope_parameter() const;
};

using PolicyLookup = std::function<std::optional<json>(const std::string& key)>;

// Reads policy.mail.* (defaults as specified when a key is missing). `tool_permitted` says whether policy allows a tool at
// all (scope selection). `dev_build`: WORKBENCH_DEV_ENDPOINTS builds, which also accept loopback http hosts and mock auth.
MailConfig read_config(const PolicyLookup& policy, const std::function<bool(const std::string& tool)>& tool_permitted, bool dev_build);

[[nodiscard]] bool is_graph_host(std::string_view host);
// True when the URL is on the Graph host set (https), or on a loopback host in development builds.
[[nodiscard]] bool is_platform_url(std::string_view url, bool dev_build);

}  // namespace wb::mail
