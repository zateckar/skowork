// The Graph HTTP layer (client/mail-calendar Graph As Sole Mail And Calendar Backend, Graph Platform Connection,
// Throttling And Paging; design D1): every mail and calendar request goes through it. The base URL is validated
// against the compiled-in Graph host set (loopback too in development builds); paging links are followed only on the
// same origin; 429 and 503 honour Retry-After; transient server errors are retried with capped exponential backoff;
// a 401 refreshes the token once. Graph is a platform connection, so policy.network egress rules do not apply here.
#pragma once

#include "base/json.h"
#include "base/result.h"
#include "mail/graph_auth.h"
#include "platform/http.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace wb::mail {

struct GraphCall {
    std::string method = "GET";
    std::string path;  // relative to the base URL ("/me/messages?...") or an absolute paging link on the same origin
    std::optional<json> body;
    bool text_bodies = true;  // Prefer: outlook.body-content-type="text"
    int page_size = 0;        // Prefer: odata.maxpagesize (delta queries)
    std::shared_ptr<CancelToken> cancel;
    // A retry delay, for the Run's Activity log ("Graph throttled the request; retrying in 10 s").
    std::function<void(long long delay_ms, const std::string& reason)> on_delay;
};

struct GraphPage {
    std::vector<json> items;
    std::string next_link, delta_link;
};

// Error codes: offline (Graph unreachable), throttled (retries exhausted), not_connected, interaction_required,
// not_found (404), sync_state_not_found (410), access_denied (403), bad_request (400), malformed (not JSON), cancelled,
// graph_error (anything else, with Graph's error code in the message), policy_error (a host outside the Graph set).
class GraphClient {
public:
    GraphClient(std::string base_url, GraphAuth& auth, bool dev_build);

    // One request; the parsed body (null for 202 and 204).
    Result<json> call(const GraphCall& c);
    // One page of a collection.
    Result<GraphPage> page(const GraphCall& c);
    // A collection, following @odata.nextLink until `limit` items or the end. `truncated` tells whether more existed.
    Result<std::vector<json>> list(GraphCall c, size_t limit, bool* truncated = nullptr);

    // Waits between retries; returns false when cancelled. Tests replace it to record the delays without waiting.
    void set_sleeper(std::function<bool(std::chrono::milliseconds, const std::shared_ptr<CancelToken>&)> s) { sleep_ = std::move(s); }
    void set_max_attempts(int n) { max_attempts_ = n; }

    [[nodiscard]] const std::string& base_url() const { return base_; }
    [[nodiscard]] long long last_success_ms() const { return last_success_; }
    [[nodiscard]] long long requests() const { return requests_; }

private:
    Result<std::string> resolve(const std::string& path) const;

    std::string base_;
    std::string origin_;
    GraphAuth& auth_;
    bool dev_build_;
    int max_attempts_ = 5;
    std::function<bool(std::chrono::milliseconds, const std::shared_ptr<CancelToken>&)> sleep_;
    std::atomic<long long> last_success_{0};
    std::atomic<long long> requests_{0};
};

// The milliseconds a Retry-After header asks for (Graph sends delta-seconds); 0 when absent or unreadable.
long long retry_after_ms(const std::string& value);

}  // namespace wb::mail
