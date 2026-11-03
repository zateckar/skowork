#include "model/errors.h"

#include "base/strings.h"

#include <algorithm>

namespace wb::model {
namespace {

ModelFailure make(const char* code, std::string message, std::string reason, bool retryable, int status = 0) {
    ModelFailure f;
    f.code = code;
    f.message = std::move(message);
    f.reason = std::move(reason);
    f.retryable = retryable;
    f.http_status = status;
    return f;
}

bool mentions_context(std::string_view text) {
    const std::string lower = to_lower(text);
    for (const char* needle : {"context_length", "context length", "context window", "maximum context", "too many tokens"})
        if (lower.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

ModelFailure invalid_request(std::string message, std::string reason) {
    return make(code::invalid_request, std::move(message), std::move(reason), false);
}

ModelFailure failure_from_error_object(const json& error) {
    const std::string gateway_code = jstr(error, "code");
    const std::string message = jstr(error, "message", "The model request failed.");
    const std::string reason = jstr(error, "reason");
    ModelFailure f;
    if (gateway_code == "QUOTA_EXCEEDED") f = make(code::quota_exceeded, message, reason, false);
    else if (gateway_code == "RATE_LIMIT") f = make(code::rate_limit, message, reason, true);
    else if (gateway_code == "CONTEXT_LENGTH") f = make(code::context_length, message, reason, false);
    else if (gateway_code == "TIMEOUT") f = make(code::timeout, message, reason, true);
    else if (gateway_code == "INVALID_REQUEST") f = make(code::invalid_request, message, reason, false);
    else f = make(code::model_unavailable, message, reason, reason != "role not granted");
    f.retry_after_s = static_cast<int>(jint(error, "retry_after_s", 0));
    f.reset_at = jstr(error, "reset_at");
    return f;
}

ModelFailure failure_from_http(int status, const std::map<std::string, std::string>& headers, std::string_view body) {
    ModelFailure f;
    const auto parsed = parse_json(body);
    const json* err = parsed ? jobj(*parsed, "error") : nullptr;
    const bool gateway_error = err && !jstr(*err, "code").empty();

    if (status == 401) {
        f = make(code::auth_expired, "The access token was rejected.", {}, false);
    } else if (gateway_error) {
        f = failure_from_error_object(*err);
    } else if (status == 429) {
        f = make(code::rate_limit, "Too many requests.", "rate", true);
    } else if (status == 403) {
        f = make(code::model_unavailable, "The request is not allowed.", {}, false);
    } else if (status == 400 || status == 413 || status == 422) {
        f = mentions_context(body) ? make(code::context_length, "The request is too large for the model.", "context window", false)
                                   : make(code::invalid_request, "The request was rejected as malformed.", {}, false);
    } else if (status == 404) {
        f = make(code::model_unavailable, "The gateway endpoint was not found.", "endpoint not found", false);
    } else if (status == 408 || status == 504) {
        f = make(code::timeout, "The model server did not answer in time.", {}, true);
    } else {
        f = make(code::model_unavailable, "The model server is not available.", {}, true);
    }
    f.http_status = status;
    if (f.retry_after_s <= 0) {
        const auto it = headers.find("retry-after");
        if (it != headers.end()) {
            try {
                f.retry_after_s = std::max(0, std::stoi(it->second));
            } catch (...) {
                // an HTTP date or garbage: ignore, the backoff applies
            }
        }
    }
    // Statuses decide retryability for codes the gateway body did not classify itself.
    if (f.code == code::model_unavailable && status == 403) f.retryable = false;
    return f;
}

ModelFailure failure_from_transport(const Error& error) {
    const std::string& c = error.code;
    if (c == "cancelled") return make(code::cancelled, "The request was cancelled.", {}, false);
    if (c == "timeout") return make(code::timeout, "The model request timed out.", "timeout", true);
    if (c == "connect") return make(code::model_unavailable, "The gateway cannot be reached.", "connect", true);
    if (c == "io" || c == "aborted") return make(code::model_unavailable, "The connection to the gateway failed.", "io", true);
    return make(code::model_unavailable, "The gateway cannot be used: " + error.message, c, false);
}

ModelFailure truncated_stream_failure(bool partial_output) {
    ModelFailure f = make(code::model_unavailable, "The model response was cut off.", "stream truncated", true);
    f.partial_output = partial_output;
    return f;
}

}  // namespace wb::model
