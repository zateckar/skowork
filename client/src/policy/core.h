// Portable part of client/policy-enforcement (design D3, D4): the Policy bundle model, the glob matcher, and the rule
// checks for paths and network hosts. No Win32 headers, so it builds on the Linux fuzz job too.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::policy {

enum class Decision { allow, approve, deny };

const char* to_string(Decision d);
std::optional<Decision> parse_decision(std::string_view s);
// The stricter of two decisions: deny over approve over allow.
Decision stricter(Decision a, Decision b);

// Risk classes as policy keys spell them (tools.risk, unattended.allowed_risk).
inline constexpr const char* kRiskClasses[] = {"read", "write-local", "external", "destructive"};
// 0 read .. 3 destructive; -1 for an unknown class.
int risk_rank(std::string_view risk_class);

// Case-insensitive glob (ASCII case folding): `**` matches across path segments, `*` within one segment, `?` one
// character of a segment. Segment separators are `\` and `/`. The same rules as the control plane's matcher.
bool glob_match(std::string_view pattern, std::string_view text);
// How specific a tool-id pattern is: an exact id beats every wildcard; otherwise more literal characters win.
int pattern_specificity(std::string_view pattern);

// The entry path used in `src` and in decisions: key["entry"] (control-plane design D1, D9).
std::string entry_path(std::string_view key, std::string_view entry);

// A verified Policy bundle (design D1, D12).
struct PolicyBundle {
    std::string sub;   // user principal
    std::string uid;   // token subject
    long long ver = 0;
    long long iat = 0;
    long long exp = 0;
    int schema = 0;
    std::vector<std::string> groups;
    json policy = json::object();  // the sparse effective policy, nested
    json src = json::object();     // rule path -> Group, "guardrail" or "mcp-catalog"
    std::string raw;               // the compact JWS
};

// Reads the claims of a bundle payload; fails on a missing or malformed claim.
Result<PolicyBundle> bundle_from_payload(const json& payload);

// The defaults of the policy keys the client applies when the effective policy leaves them unset (Sparse Effective
// Policy). Each value is the default its owning spec states; a test compares them with the generated schema.
const json& client_policy_defaults();  // {"tools.risk": {...}, "unattended.allowed_risk": [...], ...}

// The effective policy with the defaults of client_policy_defaults() behind it.
class EffectivePolicy {
public:
    explicit EffectivePolicy(PolicyBundle b);

    [[nodiscard]] const PolicyBundle& bundle() const { return b_; }
    [[nodiscard]] long long version() const { return b_.ver; }

    // The value at a dotted key, or its client default, or nullptr.
    [[nodiscard]] const json* find(std::string_view key) const;
    [[nodiscard]] bool is_set(std::string_view key) const;  // set by a Group or guardrail (not a default)
    [[nodiscard]] std::vector<std::string> strings(std::string_view key) const;
    [[nodiscard]] std::string string(std::string_view key, std::string_view fallback = {}) const;
    [[nodiscard]] long long integer(std::string_view key, long long fallback) const;
    // The Group (or guardrail) that produced a rule path, or "default" when the value is a client default.
    [[nodiscard]] std::string source(std::string_view rule_path) const;

private:
    PolicyBundle b_;
};

// ---- Paths (design D4) ----------------------------------------------------------------------------------------------

// Lexical checks on a full path before it is opened: device namespaces (\\.\, \\?\GLOBALROOT, \??\), alternate data
// streams (a ':' after the drive letter), reserved device names (CON, NUL, COM1...) and control characters are
// refused. Returns the reason, or nullopt when the path may be resolved. `\\?\C:\` and `\\?\UNC\` long-path forms are
// accepted and normalised by normalize_final_path.
std::optional<std::string> path_syntax_problem(std::string_view path);
// Strips the \\?\ prefix from a final path (\\?\UNC\server\share -> \\server\share) and drops a trailing separator
// except after a drive root.
std::string normalize_final_path(std::string_view final_path);
// True when `path` equals `folder` or lies below it (case-insensitive, separator-aware).
bool path_within(std::string_view path, std::string_view folder);

enum class Access { read, write };

struct FolderGrant {
    std::string folder;  // final path
    bool write = false;
};

// The filesystem rules with environment variables already expanded (policy.filesystem.*).
struct PathRules {
    std::vector<std::string> read, write, deny;
    std::string workspace;  // the agent workspace folder: always readable and writable
    std::string data_root;  // the client's data root: never reachable outside the workspace
    std::vector<FolderGrant> grants;
    // Local app folders the Run builds (client/local-apps App Tools): readable and writable although under the data root.
    std::vector<std::string> app_folders;
};

struct RuleVerdict {
    bool allowed = false;
    std::string rule;    // matched rule path, for example filesystem.deny["**\.ssh\**"]
    std::string reason;  // for the tool result and the approval log
};

// Checks a final (canonical) path. A deny rule always wins; then the workspace; then the data root (denied); then the
// write or read lists and the user's folder grants.
RuleVerdict check_path(std::string_view final_path, Access access, const PathRules& rules);

// ---- Network egress -------------------------------------------------------------------------------------------------

// Checks a host against policy.network.egress_deny and egress_allow. A deny match wins; an unmatched host is denied.
RuleVerdict check_egress_host(std::string_view host, const std::vector<std::string>& allow, const std::vector<std::string>& deny);
// The same for a URL: only http and https URLs with a host are allowed at all.
RuleVerdict check_egress_url(std::string_view url, const std::vector<std::string>& allow, const std::vector<std::string>& deny);

}  // namespace wb::policy
