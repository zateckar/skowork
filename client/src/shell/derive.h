// What the window derives from app.status (the host's status JSON): MCP health, the connection state, the notices
// that need the user and the reasons background work is deferred. Ported from the web UI's derive.ts.
#pragma once

#include <string>
#include <vector>

#include "base/json.h"

namespace wb::shell {

struct McpHealth {
    size_t total = 0;
    size_t healthy = 0;  // connected or idle, no warning
    std::vector<std::string> failed, unavailable, warnings;  // server names
    long long usable_tools = 0;
    std::string tone;  // ok | warn | bad | "" (no servers)
};
// client/mcp-client Health And Diagnostics.
McpHealth mcp_health(const json& status);

// connected | connecting | disconnected | not_configured | locked | update_required (client/control-channel).
std::string connection_kind(const json& status);
bool is_not_configured(const json& status);

struct BannerSpec {
    std::string id;        // locked, version_blocked, not_configured, sign_in_required, signing_in, storage_*
    std::string severity;  // error | warning | info
    bool operator==(const BannerSpec&) const = default;
};
// Notices that need the user, most severe first.
std::vector<BannerSpec> banners(const json& status);

// battery_saver, presentation, indexing_hold, storage_indexing_paused
std::vector<std::string> deferral_reasons(const json& status);

// True when the status object has the shape the window relies on (version, auth, control_plane, background, storage).
bool is_app_status(const json& status);

}  // namespace wb::shell
