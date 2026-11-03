#include "sandbox/enforcement.h"

#include "platform/win.h"

#include <vector>

namespace wb::sandbox {

namespace {

// Where Windows Firewall rules live: local rules, Group Policy, and MDM (Intune).
constexpr const wchar_t* kRuleStores[] = {
    L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules",
    L"SOFTWARE\\Policies\\Microsoft\\WindowsFirewall\\FirewallRules",
    L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\Mdm\\FirewallRules",
};

bool has_field(std::wstring_view rule, std::wstring_view field) {
    for (size_t at = 0; at < rule.size();) {
        const size_t end = rule.find(L'|', at);
        const std::wstring_view f = rule.substr(at, end == std::wstring_view::npos ? std::wstring_view::npos : end - at);
        if (f.size() == field.size() && _wcsnicmp(f.data(), field.data(), f.size()) == 0) return true;
        if (end == std::wstring_view::npos) break;
        at = end + 1;
    }
    return false;
}

}  // namespace

bool is_enforcing_rule(std::wstring_view rule) {
    return has_field(rule, L"Active=TRUE") && has_field(rule, L"Dir=Out") && has_field(rule, L"Action=Block") &&
           has_field(rule, std::wstring(L"EmbedCtxt=") + kFirewallRuleGroup);
}

std::string detect_network_enforcement() {
    for (const wchar_t* store : kRuleStores) {
        HKEY raw = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, store, 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) continue;
        unique_hkey key(raw);
        std::vector<wchar_t> name(256), data(4096);
        for (DWORD i = 0;; ++i) {
            DWORD nlen = static_cast<DWORD>(name.size()), dlen = static_cast<DWORD>(data.size() * sizeof(wchar_t)), type = 0;
            const LSTATUS st = ::RegEnumValueW(key.get(), i, name.data(), &nlen, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &dlen);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st == ERROR_MORE_DATA) {
                data.resize(dlen / sizeof(wchar_t) + 1);
                --i;
                continue;
            }
            if (st != ERROR_SUCCESS || type != REG_SZ) continue;
            if (is_enforcing_rule(std::wstring_view(data.data(), dlen / sizeof(wchar_t)))) return "os-enforced";
        }
    }
    return "proxy-only";
}

}  // namespace wb::sandbox
