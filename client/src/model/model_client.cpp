#include "model/model_client.h"

#include "base/backoff.h"
#include "base/encoding.h"
#include "base/json_schema.h"
#include "base/sse.h"
#include "base/strings.h"
#include "base/tool_args.h"
#include "base/url.h"
#include "model/errors.h"
#include "model/wire.h"

#include <algorithm>
#include <thread>

namespace wb::model {
namespace {

constexpr size_t kMaxPlainBody = 64u * 1024 * 1024;

bool success(int status) { return status >= 200 && status < 300; }

ModelFailure cancelled_failure() {
    ModelFailure f;
    f.code = code::cancelled;
    f.message = "The request was cancelled.";
    return f;
}

ModelFailure malformed(std::string what) {
    ModelFailure f;
    f.code = code::model_unavailable;
    f.message = "The gateway returned an unexpected response.";
    f.reason = std::move(what);
    f.retryable = true;
    return f;
}

ModelFailure not_registered() {
    ModelFailure f;
    f.code = code::model_unavailable;
    f.message = "The gateway address is not known yet.";
    f.reason = "gateway unknown";
    return f;
}

std::string join_errors(const std::vector<std::string>& errors) {
    std::string out;
    for (const auto& e : errors) out += (out.empty() ? "" : "; ") + e;
    return out;
}

}  // namespace

void validate_tool_calls(ChatResponse& response, const std::vector<ToolSpec>& tools) {
    for (auto& call : response.tool_calls) {
        auto parsed = parse_tool_arguments(call.raw_arguments);
        if (!parsed.value) {
            call.valid = false;
            call.validation_error = "the arguments are not valid JSON (" + parsed.error + ")";
            continue;
        }
        call.arguments = std::move(*parsed.value);
        call.repaired = parsed.repaired;
        const auto spec = std::find_if(tools.begin(), tools.end(), [&](const ToolSpec& t) { return t.id == call.tool; });
        if (spec == tools.end()) {
            call.valid = false;
            call.validation_error = "unknown tool '" + call.tool + "'";
            continue;
        }
        const auto errors = validate_json(spec->parameters, call.arguments);
        call.valid = errors.empty();
        call.validation_error = join_errors(errors);
    }
}

ModelClient::ModelClient(ModelClientConfig config, IGatewayAuth& auth, ITransport& transport, UsageLog* usage)
    : cfg_(std::move(config)), auth_(auth), transport_(transport), usage_(usage) {}

// ---- Transport plumbing -------------------------------------------------------------------------------------

ModelResult<HttpRequest> ModelClient::make_request(const std::string& method, const std::string& path, std::string body,
                                                   const std::string& accept, const std::string& run_id,
                                                   const std::shared_ptr<CancelToken>& cancel) const {
    const std::string base = cfg_.gateway_url ? cfg_.gateway_url() : std::string{};
    if (base.empty()) return not_registered();
    HttpRequest req;
    req.method = method;
    req.url = join_url(base, path);
    req.body = std::move(body);
    req.connect_timeout = cfg_.connect_timeout;
    req.idle_timeout = cfg_.idle_timeout;
    req.cancel = cancel;
    req.follow_redirects = false;  // a redirect must never carry the bearer token elsewhere
    req.headers.emplace_back("Accept", accept);
    if (!req.body.empty()) req.headers.emplace_back("Content-Type", "application/json");
    if (cfg_.installation_id)
        if (const std::string id = cfg_.installation_id(); !id.empty()) req.headers.emplace_back("X-Workbench-Installation", id);
    if (!run_id.empty()) req.headers.emplace_back("X-Workbench-Run", run_id);
    return req;
}

ModelResult<HttpResponse> ModelClient::send(HttpRequest request, const std::shared_ptr<CancelToken>& cancel) {
    (void)cancel;
    auto token = auth_.access_token();
    if (!token) {
        ModelFailure f;
        f.code = code::auth_expired;
        f.message = "Sign-in is required.";
        f.reason = token.error().code;
        return f;
    }
    const size_t auth_index = request.headers.size();
    request.headers.emplace_back("Authorization", "Bearer " + *token);
    ++requests_sent_;
    auto resp = transport_.send(request);
    if (resp && resp->status == 401) {
        // One refresh, one retry (client/identity); a second 401 ends the session.
        auto fresh = auth_.refresh_after_unauthorized(*token);
        if (!fresh) {
            ModelFailure f;
            f.code = code::auth_expired;
            f.message = "The access token was rejected and could not be refreshed.";
            f.http_status = 401;
            return f;
        }
        request.headers[auth_index].second = "Bearer " + *fresh;
        ++requests_sent_;
        resp = transport_.send(request);
        if (resp && resp->status == 401) {
            auth_.session_rejected("401 from the LLM gateway after a token refresh");
            ModelFailure f;
            f.code = code::auth_expired;
            f.message = "The gateway rejected the session.";
            f.http_status = 401;
            return f;
        }
    }
    if (!resp) return failure_from_transport(resp.error());
    return std::move(*resp);
}

bool ModelClient::wait(std::chrono::milliseconds delay, const std::shared_ptr<CancelToken>& cancel) const {
    if (cfg_.sleep) return cfg_.sleep(delay, cancel);
    const auto end = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < end) {
        if (cancel && cancel->cancelled()) return false;
        std::this_thread::sleep_for(std::min(std::chrono::milliseconds(50),
                                             std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now())));
    }
    return !(cancel && cancel->cancelled());
}

template <typename T, typename Attempt>
ModelResult<T> ModelClient::with_retries(const std::shared_ptr<CancelToken>& cancel, const ChatSink* sink, Attempt&& attempt) {
    Backoff backoff(cfg_.backoff_initial, cfg_.backoff_cap);
    for (int retry = 0;; ++retry) {
        if (cancel && cancel->cancelled()) return cancelled_failure();
        ModelResult<T> r = attempt();
        if (r.ok()) return r;
        const ModelFailure& f = r.failure();
        if (!f.retryable || retry >= cfg_.max_retries) return r;
        if (f.retry_after_s > cfg_.max_retry_wait_s) return r;  // too long to wait for: the caller decides
        const auto delay = std::max(backoff.next(), std::chrono::milliseconds(f.retry_after_s * 1000LL));
        if (sink && sink->on_retry) sink->on_retry(retry + 1, f, delay);
        if (!wait(delay, cancel)) return cancelled_failure();
    }
}

void ModelClient::record(const std::string& run_id, const std::string& thread_id, const std::string& feature, Role role,
                         const Usage& usage) {
    if (!usage_) return;
    UsageEntry e;
    e.run_id = run_id;
    e.thread_id = thread_id;
    e.feature = feature;
    e.role = role;
    e.usage = usage;
    usage_->record(e);
}

// ---- Chat ---------------------------------------------------------------------------------------------------

std::optional<ModelFailure> ModelClient::prepare_images(ChatRequest& request) const {
    int total = 0;
    for (const auto& m : request.messages) total += static_cast<int>(m.images.size());
    if (total == 0) return std::nullopt;
    // client/model-access Model Roles: images never go to other roles.
    if (request.role != Role::vision)
        return invalid_request("Images can only be sent with the vision role.", "images require the vision role");

    // At most max_images_per_request: the newest ones stay, older ones become a text note.
    int excess = total - std::max(1, cfg_.max_images_per_request);
    for (auto& m : request.messages) {
        if (excess <= 0) break;
        const int drop = std::min<int>(excess, static_cast<int>(m.images.size()));
        m.images.erase(m.images.begin(), m.images.begin() + drop);
        excess -= drop;
        m.content += (m.content.empty() ? "" : "\n") + std::string("[") + std::to_string(drop) +
                     " earlier image(s) omitted: at most " + std::to_string(cfg_.max_images_per_request) +
                     " images per request]";
    }
    for (auto& m : request.messages) {
        for (auto& img : m.images) {
            if (img.prepared) continue;
            const auto bytes = base64_decode(img.data_base64);
            if (!bytes) return invalid_request("An image is not valid base64.", "image");
            auto prepared = model::prepare_image(*bytes, cfg_.image);
            if (!prepared) return invalid_request("An image could not be processed: " + prepared.error().message, "image");
            img.mime = prepared->mime;
            img.data_base64 = base64_encode(prepared->bytes);
            img.prepared = true;
        }
    }
    return std::nullopt;
}

Result<ImagePart> ModelClient::prepare_image(const std::vector<std::uint8_t>& file_bytes) const {
    auto prepared = model::prepare_image(file_bytes, cfg_.image);
    if (!prepared) return prepared.error();
    ImagePart part;
    part.mime = prepared->mime;
    part.data_base64 = base64_encode(prepared->bytes);
    part.prepared = true;
    return part;
}

ModelResult<ChatResponse> ModelClient::chat_attempt(const ChatRequest& request, const ChatSink& sink) {
    ToolNameMap names(request);
    const bool stream = request.stream;
    const json body = build_chat_body(request, names, stream);
    auto made = make_request("POST", "chat/completions", body.dump(), stream ? "text/event-stream" : "application/json",
                             request.run_id, request.cancel);
    if (!made.ok()) return made.failure();
    HttpRequest http_req = std::move(made.value());

    ChatAccumulator acc(names, sink);
    SseParser parser;
    std::string plain_body;  // a JSON body where a stream was expected (a proxy that ignored "stream")
    enum class Mode { undecided, sse, plain } mode = Mode::undecided;
    if (stream) {
        http_req.stream_success_only = true;
        http_req.on_chunk = [&](std::string_view chunk) {
            if (mode == Mode::undecided) {
                const auto first = chunk.find_first_not_of(" \t\r\n");
                if (first == std::string_view::npos) return true;
                mode = chunk[first] == '{' ? Mode::plain : Mode::sse;
            }
            if (mode == Mode::plain) {
                plain_body.append(chunk);
                return plain_body.size() < kMaxPlainBody;
            }
            std::vector<SseEvent> events;
            if (!parser.feed(chunk, events)) return false;
            for (const auto& e : events) {
                if (e.data.empty()) continue;
                if (acc.feed(e.data) == ChatAccumulator::Step::failed) return false;
            }
            return true;
        };
    }

    auto resp = send(std::move(http_req), request.cancel);
    if (!resp.ok()) {
        if (acc.stream_error()) return *acc.stream_error();
        ModelFailure f = resp.failure();
        f.partial_output = acc.has_output();
        return f;
    }
    if (!success(resp->status)) return failure_from_http(resp->status, resp->headers, resp->body);
    if (acc.stream_error()) return *acc.stream_error();

    if (!stream || mode == Mode::plain) {
        const std::string& text = stream ? plain_body : resp->body;
        const auto parsed = parse_json(text);
        if (!parsed) return malformed("response is not JSON");
        return parse_chat_response(*parsed, names);
    }
    if (parser.failed()) return malformed("stream event too large");
    // Truncated Stream Detection: a stream needs a finish reason or the terminal marker.
    if (!acc.complete()) return truncated_stream_failure(acc.has_output());
    return acc.finish();
}

ModelResult<ChatResponse> ModelClient::chat_with_retries(const ChatRequest& request, const ChatSink& sink) {
    auto r = with_retries<ChatResponse>(request.cancel, &sink, [&] { return chat_attempt(request, sink); });
    if (r.ok()) {
        r.value().usage.requests = 1;
        record(request.run_id, request.thread_id, request.feature, request.role, r.value().usage);
    }
    return r;
}

ModelResult<ChatResponse> ModelClient::chat(const ChatRequest& original, const ChatSink& sink) {
    ChatRequest request = original;
    if (auto f = prepare_images(request)) return *f;
    if (request.cancel && request.cancel->cancelled()) return cancelled_failure();

    auto first = chat_with_retries(request, sink);
    if (!first.ok() || !request.validate_tool_calls || first.value().tool_calls.empty()) return first;

    ChatResponse resp = std::move(first.value());
    validate_tool_calls(resp, request.tools);
    std::vector<size_t> invalid;
    for (size_t i = 0; i < resp.tool_calls.size(); ++i)
        if (!resp.tool_calls[i].valid) invalid.push_back(i);
    if (invalid.empty()) return resp;

    // One corrective re-prompt (client/model-access Tool Call Validation And Repair): the model sees its invalid
    // calls and the errors as tool results, and answers with corrected calls.
    ChatRequest correction = request;
    correction.validate_tool_calls = false;
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = resp.text;
    std::vector<ChatMessage> results;
    for (const size_t i : invalid) {
        const ToolCall& c = resp.tool_calls[i];
        assistant.tool_calls.push_back({c.id, c.tool, "{}"});
        ChatMessage m;
        m.role = "tool";
        m.tool_call_id = c.id;
        m.content = "Invalid call of " + c.tool + ": " + c.validation_error + ". You sent: " +
                    utf8_truncate(c.raw_arguments, 500) + ". Call the tool again with corrected arguments.";
        results.push_back(std::move(m));
    }
    correction.messages.push_back(std::move(assistant));
    for (auto& m : results) correction.messages.push_back(std::move(m));

    auto second = chat_with_retries(correction, sink);
    resp.corrected = true;
    if (!second.ok()) return resp;  // the invalid calls stay invalid and go back to the Run as validation errors
    ChatResponse fix = std::move(second.value());
    validate_tool_calls(fix, request.tools);
    resp.usage += fix.usage;
    if (!fix.text.empty()) resp.text += (resp.text.empty() ? "" : "\n") + fix.text;
    resp.reasoning += fix.reasoning;

    std::vector<bool> used(fix.tool_calls.size(), false);
    std::vector<size_t> unmatched;
    for (const size_t i : invalid) {
        bool done = false;
        for (size_t j = 0; j < fix.tool_calls.size() && !done; ++j) {
            if (used[j] || fix.tool_calls[j].tool != resp.tool_calls[i].tool) continue;
            resp.tool_calls[i] = fix.tool_calls[j];
            used[j] = true;
            done = true;
        }
        if (!done) unmatched.push_back(i);
    }
    size_t next = 0;
    for (const size_t i : unmatched) {
        while (next < fix.tool_calls.size() && used[next]) ++next;
        if (next >= fix.tool_calls.size()) break;
        resp.tool_calls[i] = fix.tool_calls[next];
        used[next++] = true;
    }
    for (size_t j = 0; j < fix.tool_calls.size(); ++j)
        if (!used[j]) resp.tool_calls.push_back(fix.tool_calls[j]);
    return resp;
}

// ---- Role discovery -----------------------------------------------------------------------------------------

ModelResult<std::vector<RoleInfo>> ModelClient::refresh_roles(const RequestContext& ctx) {
    auto r = with_retries<std::vector<RoleInfo>>(ctx.cancel, nullptr, [&]() -> ModelResult<std::vector<RoleInfo>> {
        auto made = make_request("GET", "models", {}, "application/json", ctx.run_id, ctx.cancel);
        if (!made.ok()) return made.failure();
        auto resp = send(std::move(made.value()), ctx.cancel);
        if (!resp.ok()) return resp.failure();
        if (!success(resp->status)) return failure_from_http(resp->status, resp->headers, resp->body);
        const auto body = parse_json(resp->body);
        const json* data = body ? jobj(*body, "data") : nullptr;
        if (!data || !data->is_array()) return malformed("model list");
        std::vector<RoleInfo> roles;
        for (const auto& e : *data) {
            const auto role = parse_role(jstr(e, "id"));
            if (!role) continue;
            RoleInfo info;
            info.role = *role;
            info.kind = jstr(e, "kind");
            info.model = jstr(e, "model");
            info.context_window = jint(e, "context_window");
            info.max_output_tokens = jint(e, "max_output_tokens");
            info.tools = jbool(e, "tools");
            info.images = jbool(e, "images");
            info.reasoning = jbool(e, "reasoning");
            info.dimension = static_cast<int>(jint(e, "dimension"));
            roles.push_back(std::move(info));
        }
        return roles;
    });
    if (r.ok()) {
        const std::lock_guard lock(mx_);
        roles_.clear();
        for (const auto& info : r.value()) roles_[info.role] = info;
    }
    return r;
}

std::optional<RoleInfo> ModelClient::role_info(Role role) {
    const std::lock_guard lock(mx_);
    const auto it = roles_.find(role);
    if (it == roles_.end()) return std::nullopt;
    return it->second;
}

// ---- Embeddings and rerank ----------------------------------------------------------------------------------

ModelResult<EmbedResult> ModelClient::embed(const std::vector<std::string>& texts, const RequestContext& ctx) {
    EmbedResult out;
    if (texts.empty()) return out;
    const size_t batch = std::max<size_t>(1, cfg_.embed_batch_size);
    for (size_t start = 0; start < texts.size(); start += batch) {
        const size_t end = std::min(texts.size(), start + batch);
        json input = json::array();
        for (size_t i = start; i < end; ++i) input.push_back(texts[i]);
        const std::string body = json{{"model", "embed"}, {"input", std::move(input)}}.dump();

        auto r = with_retries<json>(ctx.cancel, nullptr, [&]() -> ModelResult<json> {
            auto made = make_request("POST", "embeddings", body, "application/json", ctx.run_id, ctx.cancel);
            if (!made.ok()) return made.failure();
            auto resp = send(std::move(made.value()), ctx.cancel);
            if (!resp.ok()) return resp.failure();
            if (!success(resp->status)) return failure_from_http(resp->status, resp->headers, resp->body);
            auto parsed = parse_json(resp->body);
            if (!parsed) return malformed("embeddings response is not JSON");
            return std::move(*parsed);
        });
        if (!r.ok()) return r.failure();
        const json& j = r.value();
        const json* data = jobj(j, "data");
        if (!data || !data->is_array() || data->size() != end - start) return malformed("embedding count");
        std::vector<std::vector<float>> vectors(end - start);
        for (const auto& item : *data) {
            const auto index = static_cast<size_t>(jint(item, "index", -1));
            const json* emb = jobj(item, "embedding");
            if (index >= vectors.size() || !emb || !emb->is_array() || !vectors[index].empty()) return malformed("embedding index");
            vectors[index].reserve(emb->size());
            for (const auto& x : *emb) vectors[index].push_back(x.is_number() ? x.get<float>() : 0.0F);
        }
        const std::string model = jstr(j, "model");
        int dimension = static_cast<int>(jint(j, "dimension"));
        if (dimension == 0 && !vectors.empty()) dimension = static_cast<int>(vectors[0].size());
        for (const auto& v : vectors)
            if (v.empty() || static_cast<int>(v.size()) != dimension) return malformed("embedding dimension");
        if (start > 0 && (model != out.model || dimension != out.dimension)) return malformed("embedding model changed");
        out.model = model;
        out.dimension = dimension;
        for (auto& v : vectors) out.vectors.push_back(std::move(v));
        Usage u;
        if (const json* uj = jobj(j, "usage")) u = parse_usage(*uj);
        if (u.total == 0) u.total = u.prompt;
        u.requests = 1;
        out.usage += u;
        record(ctx.run_id, ctx.thread_id, ctx.feature, Role::embed, u);
    }
    return out;
}

ModelResult<std::vector<double>> ModelClient::rerank(const std::string& query, const std::vector<std::string>& passages,
                                                    const RequestContext& ctx) {
    if (passages.empty()) return std::vector<double>{};
    json docs = json::array();
    for (const auto& p : passages) docs.push_back(p);
    const std::string body =
        json{{"model", "rerank"}, {"query", query}, {"documents", std::move(docs)}, {"top_n", passages.size()}}.dump();
    auto r = with_retries<json>(ctx.cancel, nullptr, [&]() -> ModelResult<json> {
        auto made = make_request("POST", "rerank", body, "application/json", ctx.run_id, ctx.cancel);
        if (!made.ok()) return made.failure();
        auto resp = send(std::move(made.value()), ctx.cancel);
        if (!resp.ok()) return resp.failure();
        if (!success(resp->status)) return failure_from_http(resp->status, resp->headers, resp->body);
        auto parsed = parse_json(resp->body);
        if (!parsed) return malformed("rerank response is not JSON");
        return std::move(*parsed);
    });
    if (!r.ok()) return r.failure();
    const json* results = jobj(r.value(), "results");
    if (!results || !results->is_array()) return malformed("rerank results");
    std::vector<double> scores(passages.size(), 0.0);
    std::vector<bool> seen(passages.size(), false);
    for (const auto& item : *results) {
        const auto index = static_cast<size_t>(jint(item, "index", -1));
        const auto it = item.find("relevance_score");
        if (index >= scores.size() || seen[index] || it == item.end() || !it->is_number()) return malformed("rerank entry");
        scores[index] = it->get<double>();
        seen[index] = true;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) return malformed("rerank scores missing");
    Usage u;
    if (const json* uj = jobj(r.value(), "usage")) u = parse_usage(*uj);
    u.requests = 1;
    record(ctx.run_id, ctx.thread_id, ctx.feature, Role::rerank, u);
    return scores;
}

// ---- Image generation ---------------------------------------------------------------------------------------

ModelResult<std::vector<std::vector<std::uint8_t>>> ModelClient::generate_images(const ImageRequest& request,
                                                                                 const RequestContext& ctx) {
    if (ctx.cancel && ctx.cancel->cancelled()) return cancelled_failure();
    const std::string body =
        json{{"model", "image"}, {"prompt", request.prompt}, {"n", request.count}, {"size", request.size}}.dump();
    auto made = make_request("POST", "images/generations", body, "application/json", ctx.run_id, ctx.cancel);
    if (!made.ok()) return made.failure();
    HttpRequest req = std::move(made.value());
    req.idle_timeout = cfg_.image_timeout;
    auto resp = send(std::move(req), ctx.cancel);
    if (!resp.ok()) return resp.failure();
    if (ctx.cancel && ctx.cancel->cancelled()) return cancelled_failure();
    if (!success(resp->status)) return failure_from_http(resp->status, resp->headers, resp->body);
    auto parsed = parse_json(resp->body);
    if (!parsed) return malformed("image response is not JSON");
    const json* data = jobj(*parsed, "data");
    if (!data || !data->is_array() || data->empty()) return malformed("image data");
    std::vector<std::vector<std::uint8_t>> images;
    for (const auto& item : *data) {
        auto decoded = base64_decode(jstr(item, "b64_json"));
        if (!decoded || decoded->empty()) return malformed("image encoding");
        images.push_back(std::move(*decoded));
    }
    Usage u;
    u.requests = 1;
    record(ctx.run_id, ctx.thread_id, ctx.feature, Role::image, u);
    return images;
}

}  // namespace wb::model
