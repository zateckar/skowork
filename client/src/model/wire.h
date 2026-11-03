// The OpenAI Chat Completions wire format as spoken through the gateway: request bodies, tool-name mapping
// (add-agent-runtime design D7), streamed and non-streamed responses with separate reasoning content.
#pragma once

#include "model/types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wb::model {

// OpenAI function names allow only [a-zA-Z0-9_-] (at most 64 characters), so a Tool id such as files.read goes on
// the wire as files__read. The map is built from the tool list of a request and also covers ids that only appear
// in the history.
std::string to_wire_name(std::string_view tool_id);

class ToolNameMap {
public:
    ToolNameMap() = default;
    explicit ToolNameMap(const ChatRequest& request);
    std::string wire(const std::string& tool_id);                 // adds the id when unknown
    [[nodiscard]] std::optional<std::string> id(const std::string& wire_name) const;

private:
    std::map<std::string, std::string> to_wire_;   // Tool id -> wire name
    std::map<std::string, std::string> from_wire_; // wire name -> Tool id
};

// Request body for POST {gateway}/chat/completions.
json build_chat_body(const ChatRequest& request, ToolNameMap& names, bool stream);

// Accumulates the `data:` payloads of a streamed response.
class ChatAccumulator {
public:
    ChatAccumulator(const ToolNameMap& names, const ChatSink& sink) : names_(names), sink_(sink) {}

    enum class Step { more, done, failed };
    // One SSE data payload: a JSON chunk or "[DONE]".
    Step feed(std::string_view data);

    // The stream carried a finish reason or the terminal marker (truncated-stream detection).
    [[nodiscard]] bool complete() const { return done_ || finish_seen_; }
    [[nodiscard]] bool has_output() const { return !response_.text.empty() || !response_.reasoning.empty() || !calls_.empty(); }
    [[nodiscard]] const std::optional<ModelFailure>& stream_error() const { return error_; }
    ChatResponse finish();

private:
    struct CallBuilder {
        std::string id, wire_name, arguments;
        bool announced = false;
    };
    const ToolNameMap& names_;
    const ChatSink& sink_;
    ChatResponse response_;
    std::map<int, CallBuilder> calls_;
    bool done_ = false;
    bool finish_seen_ = false;
    std::optional<ModelFailure> error_;
};

// A complete (non-streamed) response body.
ModelResult<ChatResponse> parse_chat_response(const json& body, const ToolNameMap& names);

// Usage object of a response or of the final stream chunk: prompt_tokens, completion_tokens,
// completion_tokens_details.reasoning_tokens, total_tokens.
Usage parse_usage(const json& usage);

}  // namespace wb::model
