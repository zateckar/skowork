#include "policy/egress.h"

#include "base/url.h"

namespace wb::policy {

RuleVerdict EgressGuard::check_url(const std::string& url) const {
    const auto p = policy_ ? policy_() : nullptr;
    if (!p) return {false, "fail-closed", "no valid policy is in force"};
    return check_egress_url(url, p->strings("network.egress_allow"), p->strings("network.egress_deny"));
}

std::string resolve_location(const std::string& base, const std::string& location) {
    if (parse_url(location)) return location;
    const auto b = parse_url(base);
    if (!b) return location;
    if (location.rfind("//", 0) == 0) return b->scheme + ":" + location;
    if (!location.empty() && location[0] == '/') return b->origin() + location;
    std::string dir = b->path.empty() ? "/" : b->path.substr(0, b->path.rfind('/') + 1);
    return b->origin() + dir + location;
}

Result<HttpResponse> guarded_fetch(HttpClient& client, HttpRequest req, const EgressGuard& guard, int max_redirects, RuleVerdict* denied,
                                   const std::function<void(HttpRequest&)>& before_send, std::string* final_url) {
    req.follow_redirects = false;
    for (int hop = 0;; ++hop) {
        const RuleVerdict v = guard.check_url(req.url);
        if (!v.allowed) {
            if (denied) *denied = v;
            return make_error("policy_denied", v.reason + " (" + v.rule + ")");
        }
        if (before_send) before_send(req);
        if (final_url) *final_url = req.url;
        auto r = client.send(req);
        if (!r) return r;
        const int s = r->status;
        const bool redirect = s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
        if (!redirect || r->header("location").empty()) return r;
        if (hop >= max_redirects) return make_error("too_many_redirects", "too many redirects");
        const std::string next = resolve_location(req.url, r->header("location"));
        if (req.url.rfind("https:", 0) == 0 && next.rfind("http:", 0) == 0)
            return make_error("insecure_redirect", "the server redirected from https to plain http: " + next);
        req.url = next;
        if (s == 303 || ((s == 301 || s == 302) && req.method == "POST")) {
            req.method = "GET";
            req.body.clear();
        }
    }
}

}  // namespace wb::policy
