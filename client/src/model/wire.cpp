#include "model/wire.h"

#include "model/errors.h"

#include <cctype>
#include <cstdint>

namespace wb::model {

std::string_view role_name(Role r) {
    switch (r) {
    case Role::main: return "main";
    case Role::vision: return "vision";
    case Role::small: return "small";
    case Role::embed: return "embed";
    case Role::rerank: return "rerank";
    case Role::image: return "image";
    }
    return "main";
}

std::optional<Role> parse_role(std::string_view name) {
    for (const Role r : {Role::main, Role::vision, Role::small, Role::embed, Role::rerank, Role::image})
        if (role_name(r) == name) return r;
    return std::nullopt;
}

std::string to_wire_name(std::string_view tool_id) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const char c : tool_id) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-') {
            out += c;
        } else if (c == '.') {
            out += "__";
        } else {
            out += '_';
            out += hex[u >> 4];
            out += hex[u & 15];
            out += '_';
        }
    }
    if (out.empty()) out = "tool";
    if (out.size() > 64) {
        std::uint32_t h = 2166136261u;  // FNV-1a over the full id keeps truncated names distinct
        for (const char c : tool_id) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
        std::string suffix = "_";
        for (int shift = 28; shift >= 0; shift -= 4) suffix += hex[(h >> shift) & 15];
        out = out.substr(0, 64 - suffix.size()) + suffix;
    }
    return out;
}

ToolNameMap::ToolNameMap(const ChatRequest& request) {
    for (const auto& t : request.tools) wire(t.id);
    for (const auto& m : request.messages)
        for (const auto& c : m.tool_calls) wire(c.tool);
}

std::string ToolNameMap::wire(const std::string& tool_id) {
    if (const auto it = to_wire_.find(tool_id); it != to_wire_.end()) return it->second;
    std::string name = to_wire_name(tool_id);
    for (int n = 2; from_wire_.count(name) != 0; ++n) {
        const std::string suffix = "_" + std::to_string(n);
        name = to_wire_name(tool_id).substr(0, 64 - suffix.size()) + suffix;
    }
    to_wire_[tool_id] = name;
    from_wire_[name] = tool_id;
    return name;
}

std::optional<std::string> ToolNameMap::id(const std::string& wire_name) const {
    const auto it = from_wire_.find(wire_name);
    if (it == from_wire_.end()) return std::nullopt;
    return it->second;
}

namespace {

json message_to_json(const ChatMessage& m, ToolNameMap& names) {
    json j = {{"role", m.role}};
    if (m.role == "tool") {
        j["tool_call_id"] = m.tool_call_id;
        j["content"] = m.content;
        return j;
    }
    if (m.role == "assistant") {
        if (m.content.empty() && !m.tool_calls.empty()) j["content"] = nullptr;
        else j["content"] = m.content;
        if (!m.tool_calls.empty()) {
            json calls = json::array();
            for (const auto& c : m.tool_calls)
                calls.push_back({{"id", c.id}, {"type", "function"},
                                 {"function", {{"name", names.wire(c.tool)}, {"arguments", c.arguments}}}});
            j["tool_calls"] = std::move(calls);
        }
        return j;
    }
    if (!m.images.empty()) {
        json parts = json::array();
        if (!m.content.empty()) parts.push_back({{"type", "text"}, {"text", m.content}});
        for (const auto& img : m.images)
            parts.push_back({{"type", "image_url"}, {"image_url", {{"url", "data:" + img.mime + ";base64," + img.data_base64}}}});
        j["content"] = std::move(parts);
    } else {
        j["content"] = m.content;
    }
    return j;
}

}  // namespace

json build_chat_body(const ChatRequest& request, ToolNameMap& names, bool stream) {
    json body = {{"model", std::string(role_name(request.role))}, {"stream", stream}};
    json messages = json::array();
    for (const auto& m : request.messages) messages.push_back(message_to_json(m, names));
    body["messages"] = std::move(messages);
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools)
            tools.push_back({{"type", "function"},
                             {"function", {{"name", names.wire(t.id)}, {"description", t.description},
                                           {"parameters", t.parameters.is_object() ? t.parameters : json{{"type", "object"}}}}}});
        body["tools"] = std::move(tools);
        body["tool_choice"] = "auto";
    }
    if (request.max_tokens) body["max_tokens"] = *request.max_tokens;
    if (request.temperature) body["temperature"] = *request.temperature;
    if (stream) body["stream_options"] = {{"include_usage", true}};
    return body;
}

Usage parse_usage(const json& usage) {
    Usage u;
    if (!usage.is_object()) return u;
    u.prompt = jint(usage, "prompt_tokens");
    u.completion = jint(usage, "completion_tokens");
    if (const json* d = jobj(usage, "completion_tokens_details")) u.reasoning = jint(*d, "reasoning_tokens");
    u.total = jint(usage, "total_tokens", u.prompt + u.completion);
    return u;
}

namespace {

std::string reasoning_of(const json& obj) {
    // vLLM and LiteLLM use reasoning_content; some stacks use reasoning.
    std::string r = jstr(obj, "reasoning_content");
    if (r.empty()) r = jstr(obj, "reasoning");
    return r;
}

ToolCall finish_call(const std::string& id, const std::string& wire_name, std::string arguments, const ToolNameMap& names,
                     size_t ordinal) {
    ToolCall c;
    c.id = id.empty() ? "call_" + std::to_string(ordinal) : id;
    c.wire_name = wire_name;
    c.tool = names.id(wire_name).value_or(wire_name);
    c.raw_arguments = std::move(arguments);
    return c;
}

}  // namespace

ChatAccumulator::Step ChatAccumulator::feed(std::string_view data) {
    if (data == "[DONE]") {
        done_ = true;
        return Step::done;
    }
    const auto parsed = parse_json(data);
    if (!parsed || !parsed->is_object()) return Step::more;  // keep-alive or garbage: ignore, truncation checks decide
    const json& chunk = *parsed;

    if (const json* e = jobj(chunk, "error"); e && e->is_object()) {
        error_ = failure_from_error_object(*e);
        error_->partial_output = has_output();
        return Step::failed;
    }
    if (response_.model.empty()) response_.model = jstr(chunk, "model");
    if (const json* u = jobj(chunk, "usage"); u && u->is_object()) {
        response_.usage = parse_usage(*u);
    }
    const json* choices = jobj(chunk, "choices");
    if (!choices || !choices->is_array() || choices->empty()) return Step::more;
    const json& choice = (*choices)[0];
    if (const json* delta = jobj(choice, "delta")) {
        const std::string reasoning = reasoning_of(*delta);
        if (!reasoning.empty()) {
            response_.reasoning += reasoning;
            if (sink_.on_reasoning) sink_.on_reasoning(reasoning);
        }
        const std::string text = jstr(*delta, "content");
        if (!text.empty()) {
            response_.text += text;
            if (sink_.on_text) sink_.on_text(text);
        }
        if (const json* tcs = jobj(*delta, "tool_calls"); tcs && tcs->is_array()) {
            for (const auto& tc : *tcs) {
                if (!tc.is_object()) continue;
                const int index = static_cast<int>(jint(tc, "index", static_cast<long long>(calls_.size())));
                CallBuilder& b = calls_[index];
                if (const std::string id = jstr(tc, "id"); !id.empty()) b.id = id;
                if (const json* fn = jobj(tc, "function")) {
                    if (const std::string name = jstr(*fn, "name"); !name.empty()) b.wire_name += name;
                    b.arguments += jstr(*fn, "arguments");
                }
                if (!b.announced && !b.wire_name.empty()) {
                    b.announced = true;
                    if (sink_.on_tool_call) sink_.on_tool_call(index, b.id, names_.id(b.wire_name).value_or(b.wire_name));
                }
            }
        }
    }
    if (const auto fr = choice.find("finish_reason"); fr != choice.end() && fr->is_string()) {
        response_.finish_reason = fr->get<std::string>();
        finish_seen_ = true;
    }
    return Step::more;
}

ChatResponse ChatAccumulator::finish() {
    size_t ordinal = 0;
    for (auto& [index, b] : calls_) {
        (void)index;
        response_.tool_calls.push_back(finish_call(b.id, b.wire_name, std::move(b.arguments), names_, ordinal++));
    }
    calls_.clear();
    return std::move(response_);
}

ModelResult<ChatResponse> parse_chat_response(const json& body, const ToolNameMap& names) {
    if (const json* e = jobj(body, "error"); e && e->is_object()) return failure_from_error_object(*e);
    const json* choices = jobj(body, "choices");
    if (!choices || !choices->is_array() || choices->empty()) {
        ModelFailure f;
        f.code = code::model_unavailable;
        f.message = "The model response had no choices.";
        f.reason = "malformed response";
        f.retryable = true;
        return f;
    }
    const json& choice = (*choices)[0];
    ChatResponse r;
    r.model = jstr(body, "model");
    r.finish_reason = jstr(choice, "finish_reason");
    if (const json* u = jobj(body, "usage")) r.usage = parse_usage(*u);
    if (const json* msg = jobj(choice, "message")) {
        r.text = jstr(*msg, "content");
        r.reasoning = reasoning_of(*msg);
        if (const json* tcs = jobj(*msg, "tool_calls"); tcs && tcs->is_array()) {
            size_t ordinal = 0;
            for (const auto& tc : *tcs) {
                const json* fn = jobj(tc, "function");
                if (!fn) continue;
                std::string args = jstr(*fn, "arguments");
                if (const json* a = jobj(*fn, "arguments"); a && a->is_object()) args = a->dump();  // some servers send an object
                r.tool_calls.push_back(finish_call(jstr(tc, "id"), jstr(*fn, "name"), std::move(args), names, ordinal++));
            }
        }
    }
    return r;
}

}  // namespace wb::model
