#include "mail/mail_config.h"

#include "base/strings.h"
#include "base/url.h"

#include <algorithm>

namespace wb::mail {

namespace {

std::string str(const PolicyLookup& p, const std::string& key, const std::string& fallback = {}) {
    const auto v = p ? p(key) : std::nullopt;
    return v && v->is_string() ? v->get<std::string>() : fallback;
}

int integer(const PolicyLookup& p, const std::string& key, int fallback, int min) {
    const auto v = p ? p(key) : std::nullopt;
    if (!v || !v->is_number_integer()) return fallback;
    return std::max(min, v->get<int>());
}

bool boolean(const PolicyLookup& p, const std::string& key, bool fallback) {
    const auto v = p ? p(key) : std::nullopt;
    return v && v->is_boolean() ? v->get<bool>() : fallback;
}

std::vector<std::string> list(const PolicyLookup& p, const std::string& key) {
    std::vector<std::string> out;
    const auto v = p ? p(key) : std::nullopt;
    if (v && v->is_array())
        for (const auto& x : *v)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

bool loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

std::string trim_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

bool is_graph_host(std::string_view host) {
    const std::string h = to_lower(std::string(host));
    return std::any_of(std::begin(kGraphHosts), std::end(kGraphHosts), [&](const char* g) { return h == g; });
}

bool is_platform_url(std::string_view url, bool dev_build) {
    const auto u = parse_url(std::string(url));
    if (!u) return false;
    if (u->scheme == "https" && is_graph_host(u->host)) return true;
    return dev_build && (u->scheme == "http" || u->scheme == "https") && loopback_host(u->host);
}

std::string MailConfig::authorize_endpoint() const { return trim_slash(authority) + "/" + tenant_id + "/oauth2/v2.0/authorize"; }
std::string MailConfig::token_endpoint() const { return trim_slash(authority) + "/" + tenant_id + "/oauth2/v2.0/token"; }

std::string MailConfig::scope_parameter() const {
    std::string out;
    for (const auto& s : scopes) out += "https://graph.microsoft.com/" + s + " ";
    return out + "offline_access";
}

MailConfig read_config(const PolicyLookup& p, const std::function<bool(const std::string&)>& tool_permitted, bool dev_build) {
    MailConfig c;
    c.graph_base_url = trim_slash(str(p, "mail.graph_base_url", kDefaultGraphBase));
    c.auth = str(p, "mail.auth", "oauth");
    c.authority = trim_slash(str(p, "mail.oauth.authority"));
    c.tenant_id = str(p, "mail.oauth.tenant_id");
    c.client_id = str(p, "mail.oauth.client_id");
    for (auto& l : list(p, "mail.excluded_labels")) c.excluded_labels.push_back(to_lower(l));
    c.exclude_unlabelled = str(p, "mail.unlabelled", "include") == "exclude";
    c.exclusions.folders = list(p, "mail.exclusions.folders");
    c.exclusions.categories = list(p, "mail.exclusions.categories");
    c.exclusions.private_items = boolean(p, "mail.exclusions.private_items", true);
    c.window.mail_days = integer(p, "mail.sync_window.mail_days", 180, 1);
    c.window.calendar_past_days = integer(p, "mail.sync_window.calendar_past_days", 90, 0);
    c.window.calendar_future_days = integer(p, "mail.sync_window.calendar_future_days", 365, 0);
    c.sync_interval_minutes = integer(p, "mail.sync_interval_minutes", 5, 1);

    // Least-privilege scopes: only for what policy enables (design: Mail.Send only when mail.send may be used).
    const auto allowed = [&](const char* tool) { return !tool_permitted || tool_permitted(tool); };
    const bool knowledge_mail = str(p, "knowledge.sources.mail", "on") != "off";
    const bool knowledge_calendar = str(p, "knowledge.sources.calendar", "on") != "off";
    c.mail_enabled = knowledge_mail || allowed("mail.search") || allowed("mail.read") || allowed("mail.draft") || allowed("mail.move") ||
                     allowed("mail.delete");
    c.send_enabled = allowed("mail.send");
    c.calendar_enabled = knowledge_calendar || allowed("calendar.list") || allowed("calendar.get") || allowed("calendar.find_free_time") ||
                         allowed("calendar.create") || allowed("calendar.respond");
    c.scopes = {"User.Read"};
    if (c.mail_enabled) c.scopes.push_back("Mail.ReadWrite");
    if (c.send_enabled) c.scopes.push_back("Mail.Send");
    if (c.calendar_enabled) {
        c.scopes.push_back("Calendars.ReadWrite");
        c.scopes.push_back("MailboxSettings.Read");
    }
    // Directory enrichment of knowledge Person entities reads department, job title and manager, which only User.Read.All
    // grants (User.ReadBasic.All stops at names and addresses).
    c.directory_enabled = str(p, "knowledge.directory_enrichment", "on") != "off";
    if (c.directory_enabled) c.scopes.push_back("User.Read.All");

    const auto refuse = [&](const std::string& code, const std::string& why) {
        c.available = false;
        c.unavailable_code = code;
        c.unavailable_reason = why;
        return c;
    };
    // Release builds: Graph hosts only, and never the mock (Non-Graph Host In Release Build, Mock Auth In Release Build).
    if (c.auth != "oauth" && c.auth != "mock") return refuse("policy_error", "mail.auth must be oauth or mock");
    if (c.auth == "mock" && !dev_build) return refuse("policy_error", "mail.auth = mock is not available in release builds");
    if (!is_platform_url(c.graph_base_url, dev_build))
        return refuse("policy_error", "mail.graph_base_url is not a Microsoft Graph service host: " + c.graph_base_url);
    if (c.authority.empty() || c.tenant_id.empty() || c.client_id.empty())
        return refuse("not_configured", "the organisation has not configured mail and calendar access (policy.mail.oauth)");
    const auto a = parse_url(c.authority);
    if (!a || !(a->scheme == "https" || (dev_build && a->scheme == "http" && loopback_host(a->host))))
        return refuse("policy_error", "mail.oauth.authority must be an https URL");
    c.available = true;
    return c;
}

}  // namespace wb::mail
