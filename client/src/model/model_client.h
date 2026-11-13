// The client's calls to the control-plane LLM gateway (client/model-access): chat completions (streamed and not),
// the model list for role discovery, embeddings, rerank and image generation. Everything goes to the gateway URL from registration,
// with an access token for the gateway audience.
#pragma once

#include "model/image.h"
#include "model/transport.h"
#include "model/types.h"
#include "model/usage.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wb::model {

struct ModelClientConfig {
    // The gateway base URL (control-channel services().gateway, for example https://host/llm/v1). Empty: not registered.
    std::function<std::string()> gateway_url;
    std::function<std::string()> installation_id;  // X-Skowork-Installation, optional

    // policy.llm.timeouts (defaults 10 s and 60 s). When set, the providers are asked before every request, so a
    // changed policy applies to the next request (Request Timeouts); a non-positive answer keeps the fixed value.
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::function<std::chrono::milliseconds()> connect_timeout_now;
    std::function<std::chrono::milliseconds()> idle_timeout_now;

    // Retries With Backoff: up to 3 retries for RATE_LIMIT and MODEL_UNAVAILABLE, one for TIMEOUT (a TIMEOUT means
    // the gateway already waited its full first-byte or idle limit; fix-long-thread-model-calls D3).
    int max_retries = 3;
    int max_timeout_retries = 1;
    std::chrono::milliseconds backoff_initial{500};
    std::chrono::milliseconds backoff_cap{8'000};
    int max_retry_wait_s = 60;  // a longer retry delay is not waited for: the failure is returned

    int max_images_per_request = 4;  // policy.llm.max_images_per_request
    ImageLimits image;               // longest side 1568 px, 5 MB
    size_t embed_batch_size = 96;    // inputs per embeddings request
    // An image request waits this long for the gateway (which allows the upstream 120 s; add-image-generation D5).
    std::chrono::milliseconds image_timeout{150'000};

    // Waits for `delay`; returns false when cancelled. Default: sleeps in short steps and watches the cancel token.
    std::function<bool(std::chrono::milliseconds, const std::shared_ptr<CancelToken>&)> sleep;
};

// Attribution of a request that is not a chat (embedding, rerank, model list).
struct RequestContext {
    std::string run_id, thread_id, feature;
    std::shared_ptr<CancelToken> cancel;
};

// A generated-image request (client/model-access Image Generation Requests).
struct ImageRequest {
    std::string prompt;
    std::string size = "1024x1024";  // 1024x1024, 1024x1536 or 1536x1024
    int count = 1;                   // 1 to 4
};

struct EmbedResult {
    std::vector<std::vector<float>> vectors;  // one per input, in input order
    std::string model;                        // the upstream model identifier reported by the gateway
    int dimension = 0;
    Usage usage;
};

class ModelClient final : public IChatModel {
public:
    ModelClient(ModelClientConfig config, IGatewayAuth& auth, ITransport& transport, UsageLog* usage = nullptr);

    // Chat completion by role. Streams text, reasoning and tool-call deltas into `sink` while it arrives. Retries
    // transient failures. Validates tool-call arguments against request.tools (repair first, then one corrective
    // re-prompt) when request.validate_tool_calls is set; calls that stay invalid have valid == false.
    ModelResult<ChatResponse> chat(const ChatRequest& request, const ChatSink& sink) override;

    // Role discovery from the gateway's model list. role_info() answers from the last successful refresh.
    ModelResult<std::vector<RoleInfo>> refresh_roles(const RequestContext& ctx = {});
    [[nodiscard]] std::optional<RoleInfo> role_info(Role role) override;

    // Embeds a batch of texts with the `embed` role.
    ModelResult<EmbedResult> embed(const std::vector<std::string>& texts, const RequestContext& ctx = {});

    // Scores passages against a query with the `rerank` role: one score per passage, in input order.
    ModelResult<std::vector<double>> rerank(const std::string& query, const std::vector<std::string>& passages,
                                            const RequestContext& ctx = {});

    // Generates images with the `image` role through the gateway's image endpoint and returns their decoded bytes,
    // unchecked. One attempt, no retries: a request takes about 20 s per image and spends the image budget.
    // Cancelling ctx.cancel aborts the request.
    ModelResult<std::vector<std::vector<std::uint8_t>>> generate_images(const ImageRequest& request,
                                                                      const RequestContext& ctx = {});

    // Downscales an image for a vision request (longest side and byte limits of the configuration).
    Result<ImagePart> prepare_image(const std::vector<std::uint8_t>& file_bytes) const;

    [[nodiscard]] const ModelClientConfig& config() const { return cfg_; }
    // The number of HTTP requests sent (including retries), for tests.
    [[nodiscard]] int requests_sent() const { return requests_sent_; }

private:
    ModelResult<HttpResponse> send(HttpRequest request, const std::shared_ptr<CancelToken>& cancel);
    ModelResult<HttpRequest> make_request(const std::string& method, const std::string& path, std::string body,
                                          const std::string& accept, const std::string& run_id,
                                          const std::shared_ptr<CancelToken>& cancel) const;
    template <typename T, typename Attempt>
    ModelResult<T> with_retries(const std::shared_ptr<CancelToken>& cancel, const ChatSink* sink, Attempt&& attempt);
    bool wait(std::chrono::milliseconds delay, const std::shared_ptr<CancelToken>& cancel) const;

    ModelResult<ChatResponse> chat_attempt(const ChatRequest& request, const ChatSink& sink);
    ModelResult<ChatResponse> chat_with_retries(const ChatRequest& request, const ChatSink& sink);
    std::optional<ModelFailure> prepare_images(ChatRequest& request) const;
    void record(const std::string& run_id, const std::string& thread_id, const std::string& feature, Role role,
                const Usage& usage);

    ModelClientConfig cfg_;
    IGatewayAuth& auth_;
    ITransport& transport_;
    UsageLog* usage_;
    std::mutex mx_;
    std::map<Role, RoleInfo> roles_;
    std::atomic<int> requests_sent_{0};
};

// Validates the arguments of every tool call of a response against the schemas of `tools` (fills valid, arguments,
// repaired and validation_error). Exposed for tests and for callers that parse non-streamed output themselves.
void validate_tool_calls(ChatResponse& response, const std::vector<ToolSpec>& tools);

}  // namespace wb::model
