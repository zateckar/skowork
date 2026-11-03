#include "mail/graph_client.h"

#include "base/backoff.h"
#include "base/url.h"
#include "platform/log.h"

#include <thread>

namespace wb::mail {
namespace {

constexpr char kLog[] = "mail";

bool default_sleep(std::chrono::milliseconds d, const std::shared_ptr<CancelToken>& cancel) {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) {
        if (cancel && cancel->cancelled()) return false;
        std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(std::chrono::milliseconds(100),
                                                                        std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()) +
                                                                            std::chrono::milliseconds(1)));
    }
    return !(cancel && cancel->cancelled());
}

Error graph_error(int status, const std::string& body) {
    const auto j = parse_json(body);
    const json* e = j ? jobj(*j, "error") : nullptr;
    const std::string code = e ? jstr(*e, "code") : std::string{};
    const std::string message = e ? jstr(*e, "message") : std::string{};
    const std::string detail = (code.empty() ? "HTTP " + std::to_string(status) : code) + (message.empty() ? "" : ": " + message);
    if (status == 404) return make_error("not_found", detail);
    if (status == 410) return make_error("sync_state_not_found", detail);
    if (status == 403) return make_error("access_denied", detail);
    if (status == 400) return make_error("bad_request", detail);
    if (status == 429) return make_error("throttled", detail);
    if (status >= 500) return make_error("offline", detail);
    return make_error("graph_error", detail);
}

}  // namespace

long long retry_after_ms(const std::string& value) {
    if (value.empty()) return 0;
    long long s = 0;
    for (const char c : value) {
        if (c == ' ') continue;
        if (c < '0' || c > '9') return 0;
        s = s * 10 + (c - '0');
        if (s > 3600) return 3600 * 1000;
    }
    return s * 1000;
}

GraphClient::GraphClient(std::string base_url, GraphAuth& auth, bool dev_build)
    : base_(std::move(base_url)), auth_(auth), dev_build_(dev_build), sleep_(default_sleep) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    if (const auto u = parse_url(base_)) origin_ = u->origin();
}

Result<std::string> GraphClient::resolve(const std::string& path) const {
    if (!is_platform_url(base_, dev_build_)) return make_error("policy_error", "the Graph base URL is not a Microsoft Graph service host");
    if (path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0) {
        // Paging and delta links: only on the Graph origin the client was configured with.
        const auto u = parse_url(path);
        if (!u || u->origin() != origin_) return make_error("policy_error", "a paging link left the Graph host");
        return path;
    }
    return base_ + (path.empty() || path[0] != '/' ? "/" : "") + path;
}

Result<json> GraphClient::call(const GraphCall& c) {
    auto url = resolve(c.path);
    if (!url) return url.error();
    auto token = auth_.access_token();
    if (!token) return token.error();
    Backoff backoff(std::chrono::seconds(1), std::chrono::seconds(30));
    bool refreshed = false;
    for (int attempt = 1;; ++attempt) {
        if (c.cancel && c.cancel->cancelled()) return make_error("cancelled", "cancelled");
        HttpRequest req;
        req.method = c.method;
        req.url = *url;
        req.cancel = c.cancel;
        req.connect_timeout = std::chrono::seconds(10);
        req.idle_timeout = std::chrono::seconds(60);
        req.headers = {{"Authorization", "Bearer " + *token}, {"Accept", "application/json"}};
        std::string prefer;
        if (c.text_bodies) prefer = "outlook.body-content-type=\"text\"";
        if (c.page_size > 0) prefer += (prefer.empty() ? "" : ", ") + std::string("odata.maxpagesize=") + std::to_string(c.page_size);
        if (!prefer.empty()) req.headers.emplace_back("Prefer", prefer);
        if (c.body) {
            req.headers.emplace_back("Content-Type", "application/json");
            req.body = c.body->dump();
        }
        ++requests_;
        auto resp = http().send(req);
        if (!resp) {
            if (resp.error().code == "cancelled" || resp.error().code == "aborted") return make_error("cancelled", "cancelled");
            return make_error("offline", "Microsoft Graph cannot be reached: " + resp.error().message);
        }
        const int status = resp->status;
        if (status == 401 && !refreshed) {
            refreshed = true;
            auto t = auth_.refresh_after_unauthorized(*token);
            if (!t) return t.error();
            token = std::move(t);
            --attempt;
            continue;
        }
        const bool throttled = status == 429 || ((status == 503 || status == 504) && !resp->header("retry-after").empty());
        const bool transient = status == 500 || status == 502 || status == 503 || status == 504;
        if ((throttled || transient) && attempt < max_attempts_) {
            long long delay = throttled ? retry_after_ms(resp->header("retry-after")) : 0;
            if (delay <= 0) delay = backoff.next().count();
            const std::string why = throttled ? "Microsoft Graph throttled the request" : "Microsoft Graph answered HTTP " + std::to_string(status);
            log::info(kLog, why + "; retrying in " + std::to_string(delay) + " ms");
            if (c.on_delay) c.on_delay(delay, why);
            if (!sleep_(std::chrono::milliseconds(delay), c.cancel)) return make_error("cancelled", "cancelled");
            continue;
        }
        if (status == 401) return make_error("not_connected", "Microsoft Graph rejected the mailbox token");
        if (status < 200 || status >= 300) return graph_error(status, resp->body);
        last_success_ = unix_millis_now();
        if (status == 202 || status == 204 || resp->body.empty()) return json(nullptr);
        auto j = parse_json(resp->body);
        if (!j) return make_error("malformed", "Microsoft Graph sent a response that is not valid JSON");
        return std::move(*j);
    }
}

Result<GraphPage> GraphClient::page(const GraphCall& c) {
    auto j = call(c);
    if (!j) return j.error();
    GraphPage p;
    if (!j->is_object()) return make_error("malformed", "a collection response is not an object");
    if (const auto v = j->find("value"); v != j->end()) {
        if (!v->is_array()) return make_error("malformed", "a collection's value is not an array");
        for (auto& x : *v)
            if (x.is_object()) p.items.push_back(std::move(x));
    }
    p.next_link = jstr(*j, "@odata.nextLink");
    p.delta_link = jstr(*j, "@odata.deltaLink");
    return p;
}

Result<std::vector<json>> GraphClient::list(GraphCall c, size_t limit, bool* truncated) {
    std::vector<json> out;
    if (truncated) *truncated = false;
    for (int pages = 0; pages < 1000; ++pages) {
        auto p = page(c);
        if (!p) return p.error();
        for (auto& x : p->items) {
            if (out.size() >= limit) {
                if (truncated) *truncated = true;
                return out;
            }
            out.push_back(std::move(x));
        }
        if (p->next_link.empty()) return out;
        if (out.size() >= limit) {
            if (truncated) *truncated = true;
            return out;
        }
        c.path = p->next_link;
        c.body.reset();
        c.method = "GET";
    }
    return out;
}

}  // namespace wb::mail
