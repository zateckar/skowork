// Types shared by model access (client/model-access) and the agent runtime (client/agent-runtime).
#pragma once

#include "base/json.h"
#include "platform/http.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wb::model {

// ---- Model roles (Glossary: Model role) ---------------------------------------------------------------------

enum class Role { main, vision, small, embed, rerank, image };

std::string_view role_name(Role r);
std::optional<Role> parse_role(std::string_view name);
inline bool is_chat_role(Role r) { return r == Role::main || r == Role::vision || r == Role::small; }

// What the gateway's model list says about one role (control-plane/llm-gateway Filtered Model List).
struct RoleInfo {
    Role role = Role::main;
    std::string kind;  // chat | embed | rerank | image
    std::string model;  // upstream model identifier when the gateway reports one (embed)
    long long context_window = 0;
    long long max_output_tokens = 0;
    bool tools = false;
    bool images = false;
    bool reasoning = false;
    int dimension = 0;  // embed only
};

// ---- Errors (client/model-access Typed Error Codes) ---------------------------------------------------------

namespace code {
inline constexpr const char* auth_expired = "AUTH_EXPIRED";
inline constexpr const char* rate_limit = "RATE_LIMIT";
inline constexpr const char* quota_exceeded = "QUOTA_EXCEEDED";
inline constexpr const char* context_length = "CONTEXT_LENGTH";
inline constexpr const char* model_unavailable = "MODEL_UNAVAILABLE";
inline constexpr const char* timeout = "TIMEOUT";
inline constexpr const char* invalid_request = "INVALID_REQUEST";  // never retried
inline constexpr const char* cancelled = "CANCELLED";              // the caller cancelled; never retried
}  // namespace code

struct ModelFailure {
    std::string code;            // one of model::code::*
    std::string message;         // safe to log and show: no tokens, no backend addresses
    std::string reason;          // for example "role not granted", "stream truncated"
    int retry_after_s = 0;       // retry delay the gateway asked for (RATE_LIMIT)
    std::string reset_at;        // ISO-8601 reset time (QUOTA_EXCEEDED)
    int http_status = 0;
    bool retryable = false;      // whether the retry rules apply to this failure
    bool partial_output = false; // output arrived before the failure and is incomplete
};

template <typename T>
class ModelResult {
public:
    ModelResult(T value) : v_(std::move(value)) {}              // NOLINT(google-explicit-constructor)
    ModelResult(ModelFailure failure) : v_(std::move(failure)) {}  // NOLINT(google-explicit-constructor)
    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }
    T& value() & { return std::get<0>(v_); }
    const T& value() const& { return std::get<0>(v_); }
    T&& value() && { return std::get<0>(std::move(v_)); }
    T* operator->() { return &std::get<0>(v_); }
    const T* operator->() const { return &std::get<0>(v_); }
    [[nodiscard]] const ModelFailure& failure() const { return std::get<1>(v_); }

private:
    std::variant<T, ModelFailure> v_;
};

// ---- Usage ---------------------------------------------------------------------------------------------------

struct Usage {
    long long prompt = 0;
    long long completion = 0;  // includes reasoning tokens, as the upstream reports them
    long long reasoning = 0;
    long long total = 0;
    int requests = 0;  // model calls
    Usage& operator+=(const Usage& o) {
        prompt += o.prompt;
        completion += o.completion;
        reasoning += o.reasoning;
        total += o.total;
        requests += o.requests;
        return *this;
    }
};

// ---- Chat ----------------------------------------------------------------------------------------------------

struct ImagePart {
    std::string mime;         // image/png, image/jpeg, ...
    std::string data_base64;  // base64 of the image file
    bool prepared = false;    // downscaled to the vision limits (model::ModelClient::prepare_image); chat() prepares the rest
};

struct ToolCallMessage {
    std::string id;
    std::string tool;       // Tool id (files.read); mapped to the wire name by the client
    std::string arguments;  // JSON text
};

struct ChatMessage {
    std::string role;  // system | user | assistant | tool
    std::string content;
    std::vector<ImagePart> images;  // user messages
    std::vector<ToolCallMessage> tool_calls;  // assistant messages
    std::string tool_call_id;                 // tool messages
    // Reasoning is deliberately not a field: it is never sent back to the model (client/model-access Reasoning Output).
};

struct ToolSpec {
    std::string id;           // Tool id, for example files.read
    std::string description;
    json parameters;          // JSON Schema (object) of the arguments
};

struct ChatRequest {
    Role role = Role::main;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    bool stream = true;
    // Attribution for usage accounting and the gateway audit (X-Workbench-Run).
    std::string run_id, thread_id, feature;
    std::shared_ptr<CancelToken> cancel;
    // Validate tool-call arguments against the tool schemas, with one corrective re-prompt.
    bool validate_tool_calls = true;
    // Optional caller-supplied guard, reapplied when model access changes the request for tool-call repair.
    std::function<bool(const ChatRequest&)> input_budget_check;
};

struct ToolCall {
    std::string id;
    std::string tool;       // Tool id (mapped back from the wire name)
    std::string raw_arguments;
    json arguments = json::object();  // the parsed arguments (an object) when valid
    bool repaired = false;
    bool valid = false;
    std::string validation_error;  // when !valid
    std::string wire_name;         // what the model called it (for unknown tools)
};

struct ChatResponse {
    std::string text;
    std::string reasoning;  // separate from the answer; never resent
    std::string finish_reason;
    std::string model;
    std::vector<ToolCall> tool_calls;
    Usage usage;
    bool corrected = false;  // a corrective re-prompt was made for invalid tool calls
};

struct ChatSink {
    std::function<void(std::string_view)> on_text;
    std::function<void(std::string_view)> on_reasoning;
    // A tool call started streaming (index in the response, call id, Tool id).
    std::function<void(int index, const std::string& call_id, const std::string& tool)> on_tool_call;
    // A retry follows after `delay`; output delivered so far belongs to an incomplete attempt and is discarded.
    std::function<void(int attempt, const ModelFailure&, std::chrono::milliseconds delay)> on_retry;
};

// The runtime's view of the model: the gateway client implements it, and tests substitute a scripted model.
class IChatModel {
public:
    virtual ~IChatModel() = default;
    virtual ModelResult<ChatResponse> chat(const ChatRequest& request, const ChatSink& sink) = 0;
    [[nodiscard]] virtual std::optional<RoleInfo> role_info(Role role) = 0;
};

}  // namespace wb::model
