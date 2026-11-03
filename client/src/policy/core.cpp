#include "policy/core.h"

#include "base/url.h"

#include <algorithm>
#include <array>

namespace wb::policy {

const char* to_string(Decision d) {
    switch (d) {
    case Decision::allow: return "allow";
    case Decision::approve: return "approve";
    case Decision::deny: return "deny";
    }
    return "deny";
}

std::optional<Decision> parse_decision(std::string_view s) {
    if (s == "allow") return Decision::allow;
    if (s == "approve") return Decision::approve;
    if (s == "deny") return Decision::deny;
    return std::nullopt;
}

Decision stricter(Decision a, Decision b) { return static_cast<int>(a) >= static_cast<int>(b) ? a : b; }

int risk_rank(std::string_view r) {
    for (int i = 0; i < 4; ++i)
        if (r == kRiskClasses[i]) return i;
    return -1;
}

// ---- glob -------------------------------------------------------------------------------------------------------------

namespace {

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_sep(char c) { return c == '\\' || c == '/'; }

enum class Tok : unsigned char { lit, any_char, star, dstar };
struct Token {
    Tok kind;
    char c;
};

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
    // Bounded so that fuzzed or hostile inputs cannot make matching expensive.
    if (pattern.size() > 4096 || text.size() > 32768) return false;
    std::vector<Token> toks;
    toks.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                while (i + 1 < pattern.size() && pattern[i + 1] == '*') ++i;
                toks.push_back({Tok::dstar, 0});
            } else {
                toks.push_back({Tok::star, 0});
            }
        } else if (c == '?') {
            toks.push_back({Tok::any_char, 0});
        } else {
            toks.push_back({Tok::lit, is_sep(c) ? '\\' : fold(c)});
        }
    }
    const size_t P = toks.size(), T = text.size();
    // next[j]: tokens i+1.. match text j..; cur[j]: tokens i.. match text j..
    std::vector<char> next(T + 1, 0), cur(T + 1, 0);
    next[T] = 1;  // empty pattern matches empty text
    for (size_t ii = P; ii-- > 0;) {
        const Token& t = toks[ii];
        for (size_t jj = T + 1; jj-- > 0;) {
            const bool more = jj < T;
            const char ch = more ? (is_sep(text[jj]) ? '\\' : fold(text[jj])) : 0;
            bool m = false;
            switch (t.kind) {
            case Tok::lit: m = more && ch == t.c && next[jj + 1]; break;
            case Tok::any_char: m = more && ch != '\\' && next[jj + 1]; break;
            case Tok::star: m = next[jj] || (more && ch != '\\' && cur[jj + 1]); break;
            case Tok::dstar: m = next[jj] || (more && cur[jj + 1]); break;
            }
            cur[jj] = m ? 1 : 0;
        }
        std::swap(cur, next);
    }
    return next[0] != 0;
}

int pattern_specificity(std::string_view pattern) {
    int literal = 0;
    bool wild = false;
    for (char c : pattern) {
        if (c == '*' || c == '?') wild = true;
        else ++literal;
    }
    return wild ? literal : 1'000'000 + literal;
}

std::string entry_path(std::string_view key, std::string_view entry) {
    return std::string(key) + "[" + json(std::string(entry)).dump() + "]";
}

// ---- bundle -------------------------------------------------------------------------------------------------------------

Result<PolicyBundle> bundle_from_payload(const json& p) {
    if (!p.is_object()) return make_error("bundle", "payload is not an object");
    PolicyBundle b;
    auto need_int = [&](const char* k, long long& out) -> bool {
        const auto it = p.find(k);
        if (it == p.end() || !it->is_number_integer()) return false;
        out = it->get<long long>();
        return true;
    };
    b.sub = jstr(p, "sub");
    b.uid = jstr(p, "uid");
    long long schema = 0;
    if (b.sub.empty() || !need_int("ver", b.ver) || !need_int("iat", b.iat) || !need_int("exp", b.exp) || !need_int("schema", schema))
        return make_error("bundle", "missing or malformed claim (sub, ver, iat, exp, schema)");
    b.schema = static_cast<int>(schema);
    if (b.ver < 1) return make_error("bundle", "version must be positive");
    const auto pol = p.find("policy");
    if (pol == p.end() || !pol->is_object()) return make_error("bundle", "policy is not an object");
    b.policy = *pol;
    if (const auto s = p.find("src"); s != p.end() && s->is_object()) b.src = *s;
    if (const auto g = p.find("groups"); g != p.end() && g->is_array())
        for (const auto& x : *g)
            if (x.is_string()) b.groups.push_back(x.get<std::string>());
    return b;
}

const json& client_policy_defaults() {
    // Defaults stated by the owning specs (client/policy-enforcement, client/tool-sandbox, client/computer-use).
    static const json d = {
        {"tools.rules", json::object()},
        {"tools.risk", {{"read", "allow"}, {"write-local", "approve"}, {"external", "approve"}, {"destructive", "deny"}}},
        {"unattended.allowed_risk", {"read", "write-local"}},
        {"unattended.rules", json::object()},
        {"filesystem.read", json::array()},
        {"filesystem.write", json::array()},
        {"filesystem.deny", json::array()},
        {"filesystem.grantable", json::array()},
        {"network.egress_allow", json::array()},
        {"network.egress_deny", json::array()},
        {"tools.shell.mode", "off"},
        {"tools.shell.limits.memory_mb", 1024},
        {"tools.shell.limits.cpu_percent", 50},
        {"tools.shell.limits.processes", 32},
        {"tools.shell.limits.wall_minutes", 15},
        {"tools.shell.limits.output_kb", 1024},
        {"mcp.idle_timeout_minutes", 10},
        {"tools.mcp.limits.memory_mb", 1024},
        {"tools.mcp.limits.cpu_percent", 50},
        {"tools.mcp.limits.processes", 32},
        {"tools.computer.mode", "off"},
        {"apps.mode", "on"},
        {"apps.mcp", "read"},
        {"messaging.mode", "on"},
        {"dictation.mode", "on"},
        {"notifications.completed", true},
        {"notifications.failed", true},
        {"notifications.action", true},
        {"skills.script_runtimes", {"powershell"}},
        {"skills.publish", {"shared"}},
        {"scheduler.enabled_kinds", {"schedule", "file", "mail", "calendar", "meeting", "session"}},
        {"scheduler.max_triggers", 50},
        {"scheduler.min_interval_minutes", 15},
        {"scheduler.max_concurrent_runs", 2},
        {"scheduler.catch_up_window_hours", 12},
        {"scheduler.history_days", 90},
        {"scheduler.remote_trusted_issuers", json::array()},
        {"scheduler.remote_share_results", json::array()},
    };
    return d;
}

EffectivePolicy::EffectivePolicy(PolicyBundle b) : b_(std::move(b)) {}

namespace {
const json* walk(const json& root, std::string_view key) {
    const json* node = &root;
    size_t start = 0;
    while (start <= key.size()) {
        const size_t dot = key.find('.', start);
        const std::string part(key.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (!node->is_object()) return nullptr;
        const auto it = node->find(part);
        if (it == node->end()) return nullptr;
        node = &*it;
        if (dot == std::string_view::npos) return node;
        start = dot + 1;
    }
    return nullptr;
}
}  // namespace

bool EffectivePolicy::is_set(std::string_view key) const { return walk(b_.policy, key) != nullptr; }

const json* EffectivePolicy::find(std::string_view key) const {
    if (const json* v = walk(b_.policy, key)) return v;
    const json& d = client_policy_defaults();
    const auto it = d.find(std::string(key));
    return it == d.end() ? nullptr : &*it;
}

std::vector<std::string> EffectivePolicy::strings(std::string_view key) const {
    std::vector<std::string> out;
    if (const json* v = find(key); v && v->is_array())
        for (const auto& x : *v)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

std::string EffectivePolicy::string(std::string_view key, std::string_view fallback) const {
    const json* v = find(key);
    return v && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

long long EffectivePolicy::integer(std::string_view key, long long fallback) const {
    const json* v = find(key);
    return v ? json_int(*v).value_or(fallback) : fallback;
}

std::string EffectivePolicy::source(std::string_view rule_path) const {
    const std::string p(rule_path);
    if (const auto it = b_.src.find(p); it != b_.src.end() && it->is_string()) return it->get<std::string>();
    const std::string key = p.substr(0, p.find('['));
    if (const auto it = b_.src.find(key); it != b_.src.end() && it->is_string()) return it->get<std::string>();
    return is_set(key) ? std::string{} : "default";
}

// ---- paths --------------------------------------------------------------------------------------------------------------

namespace {

bool istarts(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (fold(s[i]) != fold(prefix[i]) && !(is_sep(s[i]) && is_sep(prefix[i]))) return false;
    return true;
}

bool is_drive(std::string_view s) {
    return s.size() >= 2 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && s[1] == ':';
}

bool reserved_name(std::string_view comp) {
    // The base name before the first dot, without trailing spaces, is what Windows maps to a device.
    std::string base(comp.substr(0, comp.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (auto& c : base) c = fold(c);
    static const std::array<std::string_view, 4> plain = {"con", "prn", "aux", "nul"};
    if (std::find(plain.begin(), plain.end(), base) != plain.end()) return true;
    if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) && base[3] >= '0' && base[3] <= '9') return true;
    if (base == "conin$" || base == "conout$") return true;
    return false;
}

}  // namespace

std::optional<std::string> path_syntax_problem(std::string_view path) {
    if (path.empty()) return "empty path";
    if (path.size() > 32767) return "path too long";
    for (unsigned char c : path)
        if (c < 0x20) return "control character in path";
    std::string_view rest = path;
    bool unc = false;
    if (istarts(rest, "\\\\.\\") || istarts(rest, "\\??\\")) return "device namespace paths are not allowed";
    if (istarts(rest, "\\\\?\\")) {
        rest.remove_prefix(4);
        if (istarts(rest, "UNC\\")) {
            rest.remove_prefix(4);
            unc = true;
        } else if (!is_drive(rest)) {
            return "device namespace paths are not allowed";  // \\?\GLOBALROOT, \\?\Volume{...}
        }
    } else if (rest.size() >= 2 && is_sep(rest[0]) && is_sep(rest[1])) {
        rest.remove_prefix(2);
        unc = true;
    }
    if (unc) {
        if (rest.empty() || is_sep(rest[0])) return "incomplete UNC path";
        if (rest.find(':') != std::string_view::npos) return "alternate data streams are not allowed";
    } else {
        if (!is_drive(rest)) return "not a full path";
        if (rest.size() == 2 || !is_sep(rest[2])) return "drive-relative paths are not allowed";
        if (rest.find(':', 2) != std::string_view::npos) return "alternate data streams are not allowed";
        rest.remove_prefix(3);
    }
    // Reserved device names anywhere in the path.
    size_t start = 0;
    while (start < rest.size()) {
        size_t end = start;
        while (end < rest.size() && !is_sep(rest[end])) ++end;
        const std::string_view comp = rest.substr(start, end - start);
        if (!comp.empty() && reserved_name(comp)) return "reserved device names are not allowed";
        start = end + 1;
    }
    return std::nullopt;
}

std::string normalize_final_path(std::string_view p) {
    std::string s(p);
    for (auto& c : s)
        if (c == '/') c = '\\';
    if (istarts(s, "\\\\?\\UNC\\")) s = "\\\\" + s.substr(8);
    else if (istarts(s, "\\\\?\\")) s = s.substr(4);
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    if (s.size() == 2 && is_drive(s)) s += '\\';
    return s;
}

bool path_within(std::string_view path, std::string_view folder) {
    const std::string p = normalize_final_path(path), f = normalize_final_path(folder);
    if (f.empty() || p.size() < f.size() || !istarts(p, f)) return false;
    if (p.size() == f.size()) return true;
    return f.back() == '\\' || p[f.size()] == '\\';
}

RuleVerdict check_path(std::string_view final_path, Access access, const PathRules& rules) {
    const std::string path = normalize_final_path(final_path);
    // A rule for a folder's contents (`X\**`) also covers the folder X itself.
    const std::string as_folder = path + "\\";
    const auto matches = [&](const std::string& pattern) { return glob_match(pattern, path) || glob_match(pattern, as_folder); };
    for (const auto& d : rules.deny)
        if (matches(d)) return {false, entry_path("filesystem.deny", d), "the path matches a denied pattern"};
    if (!rules.workspace.empty() && path_within(path, rules.workspace)) return {true, "workspace", ""};
    for (const auto& a : rules.app_folders)
        if (path_within(path, a)) return {true, "app:" + a, ""};
    if (!rules.data_root.empty() && path_within(path, rules.data_root))
        return {false, "data-root", "the client's own data folder is not reachable by tools"};
    for (const auto& w : rules.write)
        if (matches(w)) return {true, entry_path("filesystem.write", w), ""};
    for (const auto& g : rules.grants)
        if (g.write && path_within(path, g.folder)) return {true, "grant:" + g.folder, ""};
    if (access == Access::write) return {false, "filesystem.write", "the path is not in a folder the agent may write to"};
    for (const auto& r : rules.read)
        if (matches(r)) return {true, entry_path("filesystem.read", r), ""};
    for (const auto& g : rules.grants)
        if (path_within(path, g.folder)) return {true, "grant:" + g.folder, ""};
    return {false, "filesystem.read", "the path is not in a folder the agent may read"};
}

// ---- egress -------------------------------------------------------------------------------------------------------------

namespace {

// An ASCII host name (dot-separated non-empty labels of letters, digits, '-' and '_'; IDNs only as punycode), an IPv4
// literal, or an IPv6 literal without brackets. Anything else could be decoded or IDNA-mapped by a network stack into
// a different host than the one the rules matched (Egress Host Validation).
bool valid_host(std::string_view h) {
    if (h.find(':') != std::string_view::npos)
        return std::all_of(h.begin(), h.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' || c == '.'; });
    size_t label = 0;
    for (const char c : h) {
        if (c == '.') {
            if (label == 0) return false;
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            ++label;
        } else {
            return false;
        }
    }
    return label > 0;
}

}  // namespace

RuleVerdict check_egress_host(std::string_view host_in, const std::vector<std::string>& allow, const std::vector<std::string>& deny) {
    std::string host(host_in);
    for (auto& c : host) c = fold(c);
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (!valid_host(host)) return {false, "network", "the destination is not a valid host name or IP address"};
    for (const auto& d : deny)
        if (glob_match(d, host)) return {false, entry_path("network.egress_deny", d), "the host " + host + " is denied by policy"};
    for (const auto& a : allow)
        if (glob_match(a, host)) return {true, entry_path("network.egress_allow", a), ""};
    return {false, "network.egress_allow", "the host " + host + " is not in the allowed network destinations"};
}

RuleVerdict check_egress_url(std::string_view url, const std::vector<std::string>& allow, const std::vector<std::string>& deny) {
    const auto u = parse_url(url);
    if (!u || (u->scheme != "http" && u->scheme != "https") || u->host.empty()) return {false, "network", "only http and https URLs can be fetched"};
    return check_egress_host(u->host, allow, deny);
}

}  // namespace wb::policy
