#include "channel/machine_config.h"

#include "base/json.h"
#include "identity/loopback_listener.h"
#include "platform/http.h"
#include "platform/registry.h"
#include "platform/win.h"

namespace wb {
namespace {

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\Policies\\Workbench";

// Where an administrator sets the values (improve-deployment-guidance); every configuration error names it.
constexpr char kWhere[] =
    " Set ControlPlaneUrl and IdpIssuer under HKLM\\SOFTWARE\\Policies\\Workbench, or control_plane_url and "
    "idp_issuer in workbench.machine.json next to SkoWork.exe (see the README).";

Status validate(const MachineConfig& c, const std::string& source, bool from_file = false) {
    std::string missing;
    const auto require = [&](const std::string& value, const char* file_key, const char* registry_key) {
        if (!value.empty()) return;
        if (!missing.empty()) missing += ", ";
        missing += from_file ? file_key : registry_key;
    };
    require(c.control_plane_url, "control_plane_url", "ControlPlaneUrl");
    require(c.idp_issuer, "idp_issuer", "IdpIssuer");
    require(c.idp_client_id, "idp_client_id", "IdpClientId");
    if (!missing.empty())
        return make_error("config", "machine configuration in " + source + " is missing required settings: " + missing + "." + kWhere);
    if (!HttpClient::url_allowed(c.control_plane_url) || !HttpClient::url_allowed(c.idp_issuer))
        return make_error("config", "machine configuration in " + source + " must use https URLs (ControlPlaneUrl, IdpIssuer).");
    if (!LoopbackListener::is_loopback_redirect(c.idp_redirect_uri))
        return make_error("config", "the sign-in redirect URI in " + source + " must be an http loopback URI (IdpRedirectUri).");
    return {};
}

std::optional<json> parse_config_json(std::string_view data) {
    const bool little_endian = data.starts_with("\xff\xfe");
    if (!little_endian && !data.starts_with("\xfe\xff")) return parse_json(data);
    data.remove_prefix(2);
    if (data.empty() || data.size() % 2 != 0 || data.size() / 2 > INT_MAX) return std::nullopt;
    std::wstring units;
    units.reserve(data.size() / 2);
    for (size_t i = 0; i < data.size(); i += 2) {
        const auto a = static_cast<unsigned char>(data[i]);
        const auto b = static_cast<unsigned char>(data[i + 1]);
        units.push_back(static_cast<wchar_t>(little_endian ? a | (b << 8) : (a << 8) | b));
    }
    // Unlike narrow(), reject malformed surrogate pairs instead of replacing them.
    const int length = static_cast<int>(units.size());
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, units.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) return std::nullopt;
    std::string utf8(static_cast<size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, units.data(), length, utf8.data(), size, nullptr, nullptr) != size)
        return std::nullopt;
    return parse_json(utf8);
}

}  // namespace

Result<MachineConfig> load_machine_config_file(const std::filesystem::path& file) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(file, ec);
    const auto& path = ec ? file : absolute;
    const std::string source = "\"" + narrow(path.wstring()) + "\"";
    const auto read_error = [&](DWORD code) {
        return make_error("config", "Cannot read machine configuration at " + source + " (Windows error " +
                              std::to_string(code) + "). Check file permissions and whether another program has locked it.");
    };
    unique_file f(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) {
        const DWORD code = ::GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) return read_error(code);
        auto txt = path;
        txt += L".txt";
        if (std::filesystem::is_regular_file(txt, ec))
            return make_error("not_found", "No machine configuration at " + source + ". Found \"" + narrow(txt.wstring()) +
                                  "\" instead. Rename it to \"" + narrow(path.filename().wstring()) +
                                  "\" (remove the extra .txt extension; in Notepad, save as All files).");
        return make_error("not_found", "No machine configuration at " + source + "." + kWhere);
    }
    std::string data;
    char buffer[4096];
    for (;;) {
        DWORD count = 0;
        if (!::ReadFile(f.get(), buffer, sizeof(buffer), &count, nullptr)) return read_error(::GetLastError());
        if (count == 0) break;
        data.append(buffer, count);
    }
    auto j = parse_config_json(data);
    if (!j || !j->is_object())
        return make_error("config", "Machine configuration at " + source +
                              " is not a valid JSON object. Save it as UTF-8 or UTF-16 with a byte-order mark.");
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
    if (Status s = validate(c, source, true); !s) return s.error();
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
