// Network enforcement level of client/tool-sandbox (Enforcement Limits Disclosure; design D6): `os-enforced` when the
// endpoint-management firewall profile for Workbench child processes is deployed, otherwise `proxy-only`.
#pragma once

#include <string>
#include <string_view>

namespace wb::sandbox {

// The firewall rule group (EmbedCtxt) endpoint management deploys for Workbench child processes.
inline constexpr const wchar_t* kFirewallRuleGroup = L"Workbench Child Processes";

// True for an active outbound block rule of that group, in the registry form of a Windows Firewall rule
// ("v2.30|Action=Block|Active=TRUE|Dir=Out|...|EmbedCtxt=Workbench Child Processes|").
bool is_enforcing_rule(std::wstring_view rule);

// Reads the local, Group Policy and MDM firewall rule stores; "os-enforced" or "proxy-only".
std::string detect_network_enforcement();

}  // namespace wb::sandbox
