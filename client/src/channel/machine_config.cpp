#include "channel/machine_config.h"

#include "base/json.h"
#include "identity/loopback_listener.h"
#include "platform/http.h"
#include "platform/registry.h"
#include "platform/win.h"

#include <fstream>
#include <sstream>

namespace wb {
namespace {

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\Policies\\Workbench";

// Where an administrator sets the values (improve-deployment-guidance); every configuration error names it.
constexpr char kWhere[] =
    " Set ControlPlaneUrl, IdpIssuer and IdpClientId under HKLM\\SOFTWARE\\Policies\\Workbench, or in "
    "workbench.machine.json next to Workbench.exe (see the README).";

Status validate(const MachineConfig& c, const std::string& source) {
    if (c.control_plane_url.empty() || c.idp_issuer.empty() || c.idp_client_id.empty())
        return make_error("config", "machine configuration in " + source + " is incomplete." + kWhere);
    if (!HttpClient::url_allowed(c.control_plane_url) || !HttpClient::url_allowed(c.idp_issuer))
        return make_error("config", "machine configuration in " + source + " must use https URLs (ControlPlaneUrl, IdpIssuer).");
    if (!LoopbackListener::is_loopback_redirect(c.idp_redirect_uri))
        return make_error("config", "the sign-in redirect URI in " + source + " must be an http loopback URI (IdpRedirectUri).");
    return {};
}

}  // namespace

Result<MachineConfig> load_machine_config_file(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return make_error("not_found", std::string("no machine configuration.") + kWhere);
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = parse_json(ss.str());
    if (!j || !j->is_object()) return make_error("config", "workbench.machine.json is not valid JSON.");
    MachineConfig c;
    c.control_plane_url = jstr(*j, "control_plane_url");
    c.idp_issuer = jstr(*j, "idp_issuer");
    c.idp_client_id = jstr(*j, "idp_client_id", c.idp_client_id);
    c.idp_redirect_uri = jstr(*j, "idp_redirect_uri", c.idp_redirect_uri);
    c.idp_scope = jstr(*j, "idp_scope", c.idp_scope);
    c.channel = jstr(*j, "channel", c.channel);
#ifdef WORKBENCH_DEV_ENDPOINTS
    c.dev_browser = jstr(*j, "dev_browser");
#endif
    if (Status s = validate(c, "workbench.machine.json"); !s) return s.error();
    return c;
}

Result<MachineConfig> load_machine_config() {
    if (auto cp = reg_read_string(HKEY_LOCAL_MACHINE, kPolicyKey, L"ControlPlaneUrl")) {
        MachineConfig c;
        c.control_plane_url = narrow(*cp);
        c.idp_issuer = narrow(reg_read_string(HKEY_LOCAL_MACHINE, kPolicyKey, L"IdpIssuer").value_or(L""));
        if (auto id = reg_read_string(HKEY_LOCAL_MACHINE, kPolicyKey, L"IdpClientId")) c.idp_client_id = narrow(*id);
        if (auto r = reg_read_string(HKEY_LOCAL_MACHINE, kPolicyKey, L"IdpRedirectUri")) c.idp_redirect_uri = narrow(*r);
        if (auto s = reg_read_string(HKEY_LOCAL_MACHINE, kPolicyKey, L"IdpScope")) c.idp_scope = narrow(*s);
        if (auto ch = reg_read_string(HKEY_LOCAL_MACHINE, kPolicyKey, L"Channel")) c.channel = narrow(*ch);
        if (Status s = validate(c, "HKLM\\SOFTWARE\\Policies\\Workbench"); !s) return s.error();
        return c;
    }
    return load_machine_config_file(exe_dir() / L"workbench.machine.json");
}

}  // namespace wb
