// A scripted gateway for model-access tests: an ITransport that replays prepared responses and records the requests,
// and a token source that counts refreshes.
#pragma once

#include "base/json.h"
#include "model/model_client.h"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::testing {

struct Scripted {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;                  // non-streamed body, or the error body
    std::vector<std::string> chunks;   // streamed body pieces (2xx and on_chunk set)
    std::optional<Error> error_after;  // transport error after the chunks (or instead of a response)
    std::function<void()> on_start;    // runs when the request arrives
};

struct Recorded {
    std::string method, url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    [[nodiscard]] std::string header(const std::string& name) const {
        for (const auto& [k, v] : headers)
            if (k == name) return v;
        return {};
    }
    [[nodiscard]] json json_body() const { return json::parse(body, nullptr, false); }
};

class FakeGateway final : public model::ITransport {
public:
    void push(Scripted s) {
        const std::lock_guard lock(mx);
        queue.push_back(std::move(s));
    }
    // Responses are used in order; when the queue is empty the last one is repeated.
    Result<HttpResponse> send(const HttpRequest& req) override {
        Scripted s;
        {
            const std::lock_guard lock(mx);
            Recorded r;
            r.method = req.method;
            r.url = req.url;
            r.body = req.body;
            r.headers = req.headers;
            requests.push_back(std::move(r));
            if (!queue.empty()) {
                s = queue.front();
                queue.pop_front();
                last = s;
            } else if (last) {
                s = *last;
            } else {
                return make_error("io", "no scripted response");
            }
        }
        if (s.on_start) s.on_start();
        if (s.error_after && s.chunks.empty() && s.body.empty() && s.status == 0) return *s.error_after;
        HttpResponse resp;
        resp.status = s.status;
        resp.headers = s.headers;
        const bool ok = s.status >= 200 && s.status < 300;
        if (req.on_chunk && (ok || !req.stream_success_only)) {
            for (const auto& c : s.chunks)
                if (!req.on_chunk(c)) return make_error("aborted", "stream aborted by caller");
            if (s.chunks.empty() && !s.body.empty() && !req.on_chunk(s.body)) return make_error("aborted", "aborted");
        } else {
            resp.body = s.chunks.empty() ? s.body : [&] {
                std::string all;
                for (const auto& c : s.chunks) all += c;
                return all;
            }();
        }
        if (s.error_after) return *s.error_after;
        return resp;
    }

    [[nodiscard]] size_t request_count() {
        const std::lock_guard lock(mx);
        return requests.size();
    }
    Recorded request(size_t i) {
        const std::lock_guard lock(mx);
        return requests.at(i);
    }

    std::mutex mx;
    std::deque<Scripted> queue;
    std::optional<Scripted> last;
    std::vector<Recorded> requests;
};

class FakeAuth final : public model::IGatewayAuth {
public:
    Result<std::string> access_token() override { return token; }
    Result<std::string> refresh_after_unauthorized(const std::string& rejected) override {
        ++refreshes;
        rejected_tokens.push_back(rejected);
        if (refresh_fails) return make_error("sign_in_required", "no session");
        token = "token-" + std::to_string(refreshes + 1);
        return token;
    }
    void session_rejected(const std::string&) override { ++rejected_sessions; }

    std::string token = "token-1";
    int refreshes = 0;
    int rejected_sessions = 0;
    bool refresh_fails = false;
    std::vector<std::string> rejected_tokens;
};

// ---- SSE builders (the shape of vLLM behind LiteLLM, as the llm-mock produces it) --------------------------------

inline std::string sse(const json& chunk) { return "data: " + chunk.dump() + "\n\n"; }
inline std::string sse_done() { return "data: [DONE]\n\n"; }

inline json delta_chunk(const json& delta, const json& finish = nullptr) {
    return {{"id", "c1"}, {"object", "chat.completion.chunk"}, {"model", "acme-llm-primary"},
            {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
}

inline json usage_chunk(int prompt, int completion, int reasoning) {
    return {{"id", "c1"}, {"object", "chat.completion.chunk"}, {"model", "acme-llm-primary"}, {"choices", json::array()},
            {"usage", {{"prompt_tokens", prompt}, {"completion_tokens", completion}, {"total_tokens", prompt + completion},
                       {"completion_tokens_details", {{"reasoning_tokens", reasoning}}}}}};
}

// A complete streamed text answer split into word pieces.
inline std::vector<std::string> text_stream(const std::vector<std::string>& pieces, const std::vector<std::string>& reasoning = {},
                                            int prompt = 10, int completion = 5, int reasoning_tokens = 0) {
    std::vector<std::string> out;
    out.push_back(sse(delta_chunk({{"role", "assistant"}, {"content", ""}})));
    for (const auto& r : reasoning) out.push_back(sse(delta_chunk({{"reasoning_content", r}})));
    for (const auto& p : pieces) out.push_back(sse(delta_chunk({{"content", p}})));
    out.push_back(sse(delta_chunk(json::object(), "stop")));
    out.push_back(sse(usage_chunk(prompt, completion, reasoning_tokens)));
    out.push_back(sse_done());
    return out;
}

// A streamed answer with one tool call whose arguments arrive in fragments.
inline std::vector<std::string> tool_stream(const std::string& id, const std::string& wire_name, const std::vector<std::string>& arg_pieces,
                                            int prompt = 20, int completion = 8) {
    std::vector<std::string> out;
    out.push_back(sse(delta_chunk({{"role", "assistant"}, {"content", ""}})));
    out.push_back(sse(delta_chunk({{"tool_calls", json::array({{{"index", 0}, {"id", id}, {"type", "function"},
                                                              {"function", {{"name", wire_name}, {"arguments", ""}}}}})}})));
    for (const auto& a : arg_pieces)
        out.push_back(sse(delta_chunk({{"tool_calls", json::array({{{"index", 0}, {"function", {{"arguments", a}}}}})}})));
    out.push_back(sse(delta_chunk(json::object(), "tool_calls")));
    out.push_back(sse(usage_chunk(prompt, completion, 0)));
    out.push_back(sse_done());
    return out;
}

inline Scripted streamed(std::vector<std::string> chunks) {
    Scripted s;
    s.chunks = std::move(chunks);
    return s;
}

inline Scripted error_response(int status, const std::string& code, const std::string& reason = {}, int retry_after_s = 0,
                               const std::string& reset_at = {}) {
    Scripted s;
    s.status = status;
    json e = {{"code", code}, {"message", "gateway message"}};
    if (!reason.empty()) e["reason"] = reason;
    if (retry_after_s > 0) e["retry_after_s"] = retry_after_s;
    if (!reset_at.empty()) e["reset_at"] = reset_at;
    s.body = json{{"error", e}}.dump();
    return s;
}

}  // namespace wb::testing
