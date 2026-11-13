// Machine configuration from the organization distribution system (client/control-channel Control-Plane Discovery,
// client/identity Identity Provider Configuration; design D10). Never read from user-writable locations.
#pragma once

#include "base/result.h"

#include <filesystem>
#include <string>

namespace wb {

struct MachineConfig {
    std::string control_plane_url;  // https://...
    std::string idp_issuer;         // https://<keycloak>/realms/<realm>
    std::string idp_client_id = "skowork-desktop";
    std::string idp_redirect_uri = "http://localhost:4200/auth/callback";  // loopback only (client/identity)
    std::string idp_scope = "openid profile email";
    std::string channel = "stable";
    // Test builds only (SKOWORK_DEV_ENDPOINTS): "record" replaces the system browser with a launcher that only
    // records the authorize URL; the end-to-end test completes the sign-in itself.
    std::string dev_browser;
};

// HKLM\SOFTWARE\Policies\Skowork, then skowork.machine.json next to the executable.
Result<MachineConfig> load_machine_config();
Result<MachineConfig> load_machine_config_file(const std::filesystem::path& file);

}  // namespace wb
