// client/model-access: streaming SSE client, error taxonomy and retries, tool-call repair and validation, role
// discovery, images, usage, embeddings, rerank and reasoning handling, against a scripted gateway.
#include "base/encoding.h"
#include "fake_gateway.h"
#include "model/errors.h"
#include "model/image.h"
#include "model/wire.h"
#include "png.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace wb;
using namespace wb::model;
using namespace wb::testing;

namespace {

struct Rig {
    FakeGateway gateway;
    FakeAuth auth;
    UsageLog usage;
    std::vector<std::chrono::milliseconds> sleeps;
    std::unique_ptr<ModelClient> client;

    explicit Rig(std::function<void(ModelClientConfig&)> tweak = {}) {
        ModelClientConfig cfg;
        cfg.gateway_url = [] { return "https://gateway.example/llm/v1"; };
        cfg.installation_id = [] { return "inst-1"; };
        cfg.sleep = [this](std::chrono::milliseconds d, const std::shared_ptr<CancelToken>&) {
            sleeps.push_back(d);
            return true;
        };
        if (tweak) tweak(cfg);
        client = std::make_unique<ModelClient>(std::move(cfg), auth, gateway, &usage);
    }
};

ChatRequest simple_request(Role role = Role::main) {
    ChatRequest r;
    r.role = role;
    r.run_id = "run-1";
    r.thread_id = "thread-1";
    r.messages.push_back({"user", "hello", {}, {}, {}});
    return r;
}

ToolSpec read_tool() {
    return {"files.read", "Reads a file",
            json::parse(R"({"type":"object","properties":{"path":{"type":"string"},"rationale":{"type":"string","minLength":1}},
                            "required":["path","rationale"],"additionalProperties":false})")};
}

Scripted transport_error(const char* code) {
    Scripted s;
    s.status = 0;
    s.error_after = make_error(code, "boom");
    return s;
}

}  // namespace

// ---- 2.1 Streaming SSE client ------------------------------------------------------------------------------

TEST(ModelStreaming, PassesTextOnWhileTheResponseIsStillArriving) {
    Rig rig;
    std::vector<std::string> log;
    Scripted s = streamed(text_stream({"Hel", "lo ", "world"}));
    rig.gateway.push(s);

    ChatSink sink;
    sink.on_text = [&](std::string_view t) { log.push_back("text:" + std::string(t)); };
    auto r = rig.client->chat(simple_request(), sink);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->text, "Hello world");
    EXPECT_EQ(r->finish_reason, "stop");
    EXPECT_EQ(log, (std::vector<std::string>{"text:Hel", "text:lo ", "text:world"}));
    EXPECT_EQ(r->usage.prompt, 10);
    EXPECT_EQ(r->usage.completion, 5);
}

TEST(ModelStreaming, RequestShapeAuthAndRunAttribution) {
    Rig rig;
    rig.gateway.push(streamed(text_stream({"ok"})));
    auto req = simple_request(Role::small);
    ASSERT_TRUE(rig.client->chat(req, {}).ok());
    const Recorded rec = rig.gateway.request(0);
    EXPECT_EQ(rec.url, "https://gateway.example/llm/v1/chat/completions");
    EXPECT_EQ(rec.header("Authorization"), "Bearer token-1");
    EXPECT_EQ(rec.header("X-Skowork-Run"), "run-1");
    EXPECT_EQ(rec.header("X-Skowork-Installation"), "inst-1");
    const json body = rec.json_body();
    EXPECT_EQ(body["model"], "small");  // roles only, never an upstream model id
    EXPECT_TRUE(body["stream"].get<bool>());
    EXPECT_TRUE(body["stream_options"]["include_usage"].get<bool>());
}

TEST(ModelStreaming, ToolCallDeltasAreAssembledAndNamesMappedBack) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("call_1", "files__read", {"{\"pa", "th\":\"a.txt\",", "\"rationale\":\"needed\"}"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    std::vector<std::string> announced;
    ChatSink sink;
    sink.on_tool_call = [&](int, const std::string& id, const std::string& tool) { announced.push_back(id + "=" + tool); };
    auto r = rig.client->chat(req, sink);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    ASSERT_EQ(r->tool_calls.size(), 1U);
    EXPECT_EQ(r->tool_calls[0].tool, "files.read");  // runtime and policy see the Tool id (design D7)
    EXPECT_EQ(r->tool_calls[0].arguments["path"], "a.txt");
    EXPECT_TRUE(r->tool_calls[0].valid);
    EXPECT_EQ(announced, (std::vector<std::string>{"call_1=files.read"}));
    // The request carried the wire name.
    EXPECT_EQ(rig.gateway.request(0).json_body()["tools"][0]["function"]["name"], "files__read");
}

TEST(ModelStreaming, WireNames) {
    EXPECT_EQ(to_wire_name("files.read"), "files__read");
    EXPECT_EQ(to_wire_name("browser.click"), "browser__click");
    EXPECT_EQ(to_wire_name("ask_user"), "ask_user");
    const std::string mcp = to_wire_name("mcp:github/create_issue");
    for (const char c : mcp) EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') << mcp;
    EXPECT_LE(to_wire_name(std::string(200, 'x')).size(), 64U);
    ChatRequest r;
    r.tools = {{"a.b", "", json::object()}, {"a_.b", "", json::object()}, {"a.b", "", json::object()}};
    ToolNameMap names(r);
    EXPECT_NE(names.wire("a.b"), names.wire("a_.b"));
    EXPECT_EQ(names.id(names.wire("a.b")).value_or(""), "a.b");
}

TEST(ModelStreaming, CutStreamIsAFailedRequestWithPartialOutput) {
    Rig rig([](ModelClientConfig& c) { c.max_retries = 0; });
    auto chunks = text_stream({"partial ", "answer"});
    chunks.resize(2);  // the role chunk and the first text piece: cut before the finish reason, no [DONE]
    rig.gateway.push(streamed(chunks));
    std::string seen;
    ChatSink sink;
    sink.on_text = [&](std::string_view t) { seen += t; };
    auto r = rig.client->chat(simple_request(), sink);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::model_unavailable);
    EXPECT_EQ(r.failure().reason, "stream truncated");
    EXPECT_TRUE(r.failure().partial_output);
    EXPECT_TRUE(r.failure().retryable);
    EXPECT_EQ(seen, "partial ");  // delivered as it arrived, but the request did not succeed
}

TEST(ModelStreaming, ConnectionDropMidStreamIsReportedAndRetried) {
    Rig rig;
    Scripted cut = streamed({text_stream({"par", "tial"})[0], text_stream({"par", "tial"})[1]});
    cut.error_after = make_error("connect", "connection reset");
    rig.gateway.push(cut);
    rig.gateway.push(streamed(text_stream({"full ", "answer"})));
    int retries = 0;
    ChatSink sink;
    sink.on_retry = [&](int attempt, const ModelFailure& f, std::chrono::milliseconds) {
        ++retries;
        EXPECT_EQ(attempt, 1);
        EXPECT_EQ(f.code, code::model_unavailable);
        EXPECT_TRUE(f.partial_output);
    };
    auto r = rig.client->chat(simple_request(), sink);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->text, "full answer");  // the incomplete first attempt left nothing in the result
    EXPECT_EQ(retries, 1);
    EXPECT_EQ(rig.gateway.request_count(), 2U);
}

TEST(ModelStreaming, FinishReasonWithoutDoneIsComplete) {
    Rig rig;
    auto chunks = text_stream({"hi"});
    chunks.pop_back();  // no [DONE], but the finish reason arrived
    rig.gateway.push(streamed(chunks));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->text, "hi");
}

TEST(ModelStreaming, EventsSplitAtArbitraryByteBoundaries) {
    Rig rig;
    std::string all;
    for (const auto& c : text_stream({"Grüße ", "wörld"})) all += c;
    std::vector<std::string> bytes;
    for (size_t i = 0; i < all.size(); i += 5) bytes.push_back(all.substr(i, 5));
    rig.gateway.push(streamed(bytes));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->text, "Grüße wörld");
}

TEST(ModelStreaming, ErrorEventInsideTheStreamFailsTheRequest) {
    Rig rig([](ModelClientConfig& c) { c.max_retries = 0; });
    std::vector<std::string> chunks = {sse(delta_chunk({{"content", "x"}})),
                                       sse({{"error", {{"code", "MODEL_UNAVAILABLE"}, {"message", "gone"}}}})};
    rig.gateway.push(streamed(chunks));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::model_unavailable);
}

TEST(ModelStreaming, JsonBodyWhereAStreamWasExpectedIsAccepted) {
    Rig rig;
    Scripted s;
    s.body = R"({"model":"m","choices":[{"message":{"role":"assistant","content":"plain"},"finish_reason":"stop"}],
                 "usage":{"prompt_tokens":3,"completion_tokens":1,"total_tokens":4}})";
    rig.gateway.push(s);
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->text, "plain");
}

// ---- 2.2 Error taxonomy, retries, auth refresh -------------------------------------------------------------

TEST(ModelErrors, EachGatewayErrorClassMapsToOneCode) {
    struct Case {
        int status;
        std::string body_code, reason;
        const char* expect;
        bool retryable;
    };
    const std::vector<Case> cases = {
        {429, "QUOTA_EXCEEDED", "quota", code::quota_exceeded, false},
        {429, "RATE_LIMIT", "rate", code::rate_limit, true},
        {400, "CONTEXT_LENGTH", "context window", code::context_length, false},
        {503, "MODEL_UNAVAILABLE", "no healthy backend", code::model_unavailable, true},
        {502, "MODEL_UNAVAILABLE", "", code::model_unavailable, true},
        {403, "MODEL_UNAVAILABLE", "role not granted", code::model_unavailable, false},
        {400, "INVALID_REQUEST", "role not valid for endpoint", code::invalid_request, false},
        {504, "TIMEOUT", "backend timeout", code::timeout, true},
    };
    for (const auto& c : cases) {
        const auto s = error_response(c.status, c.body_code, c.reason);
        const ModelFailure f = failure_from_http(c.status, s.headers, s.body);
        EXPECT_EQ(f.code, c.expect) << c.status << " " << c.body_code;
        EXPECT_EQ(f.retryable, c.retryable) << c.status << " " << c.body_code;
        EXPECT_EQ(f.http_status, c.status);
    }
    EXPECT_EQ(failure_from_http(401, {}, "{}").code, code::auth_expired);
    EXPECT_EQ(failure_from_http(401, {}, "{}").retryable, false);  // handled by the one-shot refresh instead
    EXPECT_EQ(failure_from_http(500, {}, "garbage").code, code::model_unavailable);
    EXPECT_EQ(failure_from_http(504, {}, "").code, code::timeout);
    EXPECT_EQ(failure_from_http(429, {{"retry-after", "7"}}, "").retry_after_s, 7);
    EXPECT_EQ(failure_from_http(400, {}, "maximum context length exceeded").code, code::context_length);
    EXPECT_EQ(failure_from_transport(make_error("timeout", "")).code, code::timeout);
    EXPECT_EQ(failure_from_transport(make_error("connect", "")).code, code::model_unavailable);
    EXPECT_EQ(failure_from_transport(make_error("cancelled", "")).code, code::cancelled);
}

TEST(ModelErrors, QuotaExhaustedCarriesTheResetTimeAndIsNotRetried) {
    Rig rig;
    rig.gateway.push(error_response(429, "QUOTA_EXCEEDED", "quota", 0, "2026-10-23T00:00:00Z"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::quota_exceeded);
    EXPECT_EQ(r.failure().reset_at, "2026-10-23T00:00:00Z");
    EXPECT_EQ(rig.gateway.request_count(), 1U);
}

TEST(ModelErrors, RoleNotGrantedIsNotRetried) {
    Rig rig;
    rig.gateway.push(error_response(403, "MODEL_UNAVAILABLE", "role not granted"));
    auto r = rig.client->chat(simple_request(Role::small), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::model_unavailable);
    EXPECT_EQ(r.failure().reason, "role not granted");
    EXPECT_EQ(rig.gateway.request_count(), 1U);
    EXPECT_TRUE(rig.sleeps.empty());
}

TEST(ModelRetries, TransientOutageIsRetriedWithoutAnErrorAndReportedToTheSink) {
    Rig rig;
    rig.gateway.push(error_response(503, "MODEL_UNAVAILABLE", "backend unavailable"));
    rig.gateway.push(streamed(text_stream({"back"})));
    std::vector<std::string> activity;
    ChatSink sink;
    sink.on_retry = [&](int attempt, const ModelFailure& f, std::chrono::milliseconds d) {
        activity.push_back(std::to_string(attempt) + ":" + f.code + ":" + std::to_string(d.count() > 0));
    };
    auto r = rig.client->chat(simple_request(), sink);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->text, "back");
    EXPECT_EQ(activity, (std::vector<std::string>{"1:MODEL_UNAVAILABLE:1"}));
    EXPECT_EQ(rig.sleeps.size(), 1U);
}

TEST(ModelRetries, BudgetIsRecheckedBeforeEachRetry) {
    Rig rig;
    rig.gateway.push(error_response(503, "MODEL_UNAVAILABLE", "backend unavailable"));
    auto req = simple_request();
    int checks = 0;
    req.input_budget_check = [&](const ChatRequest&) { return ++checks == 1; };
    const auto result = rig.client->chat(req, {});
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.failure().code, code::context_length);
    EXPECT_EQ(checks, 2);
    EXPECT_EQ(rig.gateway.request_count(), 1u);
}

TEST(ModelRetries, UpToThreeRetriesThenTheFailure) {
    Rig rig;
    rig.gateway.push(error_response(503, "MODEL_UNAVAILABLE"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::model_unavailable);
    EXPECT_EQ(rig.gateway.request_count(), 4U);  // first try + 3 retries
    EXPECT_EQ(rig.sleeps.size(), 3U);
}

TEST(ModelRetries, BackoffGrowsAndRetryAfterIsHonoured) {
    Rig rig;
    rig.gateway.push(error_response(429, "RATE_LIMIT", "rate", 12));
    rig.gateway.push(streamed(text_stream({"ok"})));
    ASSERT_TRUE(rig.client->chat(simple_request(), {}).ok());
    ASSERT_EQ(rig.sleeps.size(), 1U);
    EXPECT_GE(rig.sleeps[0], std::chrono::seconds(12));
}

TEST(ModelRetries, ARetryDelayBeyondTheCapIsNotWaitedFor) {
    Rig rig;
    rig.gateway.push(error_response(429, "RATE_LIMIT", "rate", 3600));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::rate_limit);
    EXPECT_EQ(r.failure().retry_after_s, 3600);
    EXPECT_EQ(rig.gateway.request_count(), 1U);
}

TEST(ModelRetries, TimeoutIsRetried) {
    Rig rig;
    rig.gateway.push(transport_error("timeout"));
    rig.gateway.push(streamed(text_stream({"late"})));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(rig.gateway.request_count(), 2U);
}

// fix-long-thread-model-calls D3: a TIMEOUT means the gateway already waited its full limit; one retry only.
TEST(ModelRetries, RepeatedTimeoutFailsAfterTwoAttempts) {
    Rig rig;
    rig.gateway.push(transport_error("timeout"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::timeout);
    EXPECT_EQ(rig.gateway.request_count(), 2U);
}

TEST(ModelRetries, TimeoutErrorEventInsideAKeepAliveStreamIsRetriedOnce) {
    Rig rig;
    rig.gateway.push(streamed({": keep-alive\n\n", ": keep-alive\n\n",
                               sse({{"error", {{"code", "TIMEOUT"}, {"message", "no first token"}, {"reason", "first byte"}}}})}));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::timeout);
    EXPECT_FALSE(r.failure().partial_output);
    EXPECT_EQ(rig.gateway.request_count(), 2U);
}

TEST(ModelStreaming, KeepAliveCommentsBeforeTheFirstTokenCarryNoContent) {
    Rig rig;
    std::vector<std::string> chunks = {": keep-alive\n\n", ": keep-alive\n\n"};
    for (auto& c : text_stream({"hel", "lo"})) chunks.push_back(c);
    rig.gateway.push(streamed(chunks));
    std::string seen;
    ChatSink sink;
    sink.on_text = [&](std::string_view t) { seen += t; };
    auto r = rig.client->chat(simple_request(), sink);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->text, "hello");
    EXPECT_EQ(seen, "hello");
}

TEST(ModelTimeouts, PolicyValuesApplyToTheNextRequest) {
    std::chrono::milliseconds idle{0};
    Rig rig([&](ModelClientConfig& c) {
        c.idle_timeout_now = [&] { return idle; };
        c.connect_timeout_now = [] { return std::chrono::milliseconds(5'000); };
    });
    rig.gateway.push(streamed(text_stream({"a"})));
    ASSERT_TRUE(rig.client->chat(simple_request(), {}).ok());
    idle = std::chrono::milliseconds(120'000);  // the administrator raised policy.llm.timeouts.idle_seconds
    ASSERT_TRUE(rig.client->chat(simple_request(), {}).ok());
    ASSERT_EQ(rig.gateway.requests.size(), 2U);
    EXPECT_EQ(rig.gateway.requests[0].idle_timeout, std::chrono::milliseconds(60'000));  // unset: the default
    EXPECT_EQ(rig.gateway.requests[1].idle_timeout, std::chrono::milliseconds(120'000));
    EXPECT_EQ(rig.gateway.requests[1].connect_timeout, std::chrono::milliseconds(5'000));
}

TEST(ModelRetries, InvalidRequestIsNotRetried) {
    Rig rig;
    rig.gateway.push(error_response(400, "INVALID_REQUEST"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::invalid_request);
    EXPECT_EQ(rig.gateway.request_count(), 1U);
}

TEST(ModelRetries, CancellationDuringTheWaitEndsTheRequest) {
    Rig rig([](ModelClientConfig& c) { c.sleep = [](std::chrono::milliseconds, const std::shared_ptr<CancelToken>&) { return false; }; });
    rig.gateway.push(error_response(503, "MODEL_UNAVAILABLE"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::cancelled);
}

TEST(ModelAuth, ExpiredTokenIsRefreshedOnceAndTheRequestRepeated) {
    Rig rig;
    rig.gateway.push(error_response(401, "UNAUTHENTICATED"));
    rig.gateway.push(streamed(text_stream({"ok"})));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(rig.auth.refreshes, 1);
    EXPECT_EQ(rig.auth.rejected_tokens, (std::vector<std::string>{"token-1"}));
    EXPECT_EQ(rig.gateway.request(0).header("Authorization"), "Bearer token-1");
    EXPECT_EQ(rig.gateway.request(1).header("Authorization"), "Bearer token-2");
    EXPECT_EQ(rig.sleeps.size(), 0U);  // a refresh is not a backoff retry
}

TEST(ModelAuth, SecondRejectionEndsTheSession) {
    Rig rig;
    rig.gateway.push(error_response(401, "UNAUTHENTICATED"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::auth_expired);
    EXPECT_EQ(rig.auth.refreshes, 1);
    EXPECT_EQ(rig.auth.rejected_sessions, 1);
    EXPECT_EQ(rig.gateway.request_count(), 2U);
}

TEST(ModelAuth, FailedRefreshIsAuthExpired) {
    Rig rig;
    rig.auth.refresh_fails = true;
    rig.gateway.push(error_response(401, "UNAUTHENTICATED"));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::auth_expired);
}

TEST(ModelAuth, UnknownGatewayAddressFailsWithoutAnyRequest) {
    Rig rig([](ModelClientConfig& c) { c.gateway_url = [] { return std::string(); }; });
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::model_unavailable);
    EXPECT_EQ(rig.gateway.request_count(), 0U);
}

// ---- 2.3 Tool-call repair and validation -------------------------------------------------------------------

TEST(ModelToolCalls, TrailingCommaIsRepairedWithoutAnExtraModelCall) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("c1", "files__read", {"{\"path\":\"a.txt\",\"rationale\":\"needed\",}"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->tool_calls.size(), 1U);
    EXPECT_TRUE(r->tool_calls[0].valid);
    EXPECT_TRUE(r->tool_calls[0].repaired);
    EXPECT_FALSE(r->corrected);
    EXPECT_EQ(rig.gateway.request_count(), 1U);
}

TEST(ModelToolCalls, InvalidCallGetsOneCorrectiveRePrompt) {
    Rig rig;
    // First answer lacks the required rationale; the corrected call has it.
    rig.gateway.push(streamed(tool_stream("c1", "files__read", {"{\"path\":\"a.txt\"}"})));
    rig.gateway.push(streamed(tool_stream("c2", "files__read", {"{\"path\":\"a.txt\",\"rationale\":\"to answer the question\"}"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->tool_calls.size(), 1U);
    EXPECT_TRUE(r->tool_calls[0].valid);
    EXPECT_EQ(r->tool_calls[0].id, "c2");
    EXPECT_TRUE(r->corrected);
    EXPECT_EQ(r->usage.requests, 2);
    ASSERT_EQ(rig.gateway.request_count(), 2U);

    // The corrective request shows the model its invalid call and the error as a tool result.
    const json second = rig.gateway.request(1).json_body();
    const json& msgs = second["messages"];
    ASSERT_EQ(msgs.size(), 3U);
    EXPECT_EQ(msgs[1]["role"], "assistant");
    EXPECT_EQ(msgs[1]["tool_calls"][0]["function"]["name"], "files__read");
    EXPECT_EQ(msgs[2]["role"], "tool");
    EXPECT_EQ(msgs[2]["tool_call_id"], "c1");
    EXPECT_NE(msgs[2]["content"].get<std::string>().find("rationale"), std::string::npos);
}

TEST(ModelToolCalls, PersistentlyInvalidCallStaysInvalid) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("c1", "files__read", {"{\"path\":5}"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    auto r = rig.client->chat(req, {});  // the repeated last response is invalid again
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->tool_calls.size(), 1U);
    EXPECT_FALSE(r->tool_calls[0].valid);
    EXPECT_FALSE(r->tool_calls[0].validation_error.empty());
    EXPECT_EQ(rig.gateway.request_count(), 2U);  // exactly one correction, never more
}

TEST(ModelToolCalls, OversizedCorrectionIsNotSent) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("c1", "files__read", {"{\"path\":5}"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    req.input_budget_check = [](const ChatRequest& request) { return request.messages.size() == 1; };
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->tool_calls.size(), 1u);
    EXPECT_FALSE(r->tool_calls[0].valid);
    EXPECT_FALSE(r->tool_calls[0].validation_error.empty());
    EXPECT_FALSE(r->corrected);
    EXPECT_EQ(rig.gateway.request_count(), 1u);
}

TEST(ModelToolCalls, ValidCallsAreKeptWhenAnotherCallNeedsCorrection) {
    Rig rig;
    std::vector<std::string> chunks;
    chunks.push_back(sse(delta_chunk({{"role", "assistant"}})));
    chunks.push_back(sse(delta_chunk({{"tool_calls", json::array({
        {{"index", 0}, {"id", "a"}, {"function", {{"name", "files__read"}, {"arguments", R"({"path":"1","rationale":"r"})"}}}},
        {{"index", 1}, {"id", "b"}, {"function", {{"name", "files__read"}, {"arguments", R"({"nope":1})"}}}}})}})));
    chunks.push_back(sse(delta_chunk(json::object(), "tool_calls")));
    chunks.push_back(sse_done());
    rig.gateway.push(streamed(chunks));
    rig.gateway.push(streamed(tool_stream("b2", "files__read", {R"({"path":"2","rationale":"r"})"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->tool_calls.size(), 2U);
    EXPECT_EQ(r->tool_calls[0].id, "a");
    EXPECT_TRUE(r->tool_calls[0].valid);
    EXPECT_EQ(r->tool_calls[1].id, "b2");
    EXPECT_TRUE(r->tool_calls[1].valid);
}

TEST(ModelToolCalls, UnknownToolIsInvalid) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("c1", "files__delete", {"{}"})));
    auto req = simple_request();
    req.tools.push_back(read_tool());
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r->tool_calls[0].valid);
    EXPECT_NE(r->tool_calls[0].validation_error.find("unknown tool"), std::string::npos);
}

// ---- 2.4 Role discovery, images, usage ---------------------------------------------------------------------

TEST(ModelRoles, DiscoveredFromTheGatewayModelList) {
    Rig rig;
    Scripted s;
    s.body = R"({"object":"list","data":[
      {"id":"main","kind":"chat","context_window":32768,"max_output_tokens":8192,"tools":true,"images":true,"reasoning":true},
      {"id":"small","kind":"chat","context_window":16384,"max_output_tokens":4096,"tools":true,"images":false,"reasoning":false},
      {"id":"embed","kind":"embed","dimension":4096,"model":"acme-llm-embedding"},
      {"id":"mystery","kind":"chat"}]})";
    rig.gateway.push(s);
    EXPECT_FALSE(rig.client->role_info(Role::main).has_value());  // nothing known before discovery
    auto r = rig.client->refresh_roles();
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->size(), 3U);  // unknown role ids are ignored
    const auto main = rig.client->role_info(Role::main);
    ASSERT_TRUE(main.has_value());
    EXPECT_EQ(main->context_window, 32768);
    EXPECT_TRUE(main->reasoning);
    EXPECT_TRUE(main->images);
    EXPECT_FALSE(rig.client->role_info(Role::small)->images);
    EXPECT_EQ(rig.client->role_info(Role::embed)->dimension, 4096);
    EXPECT_FALSE(rig.client->role_info(Role::vision).has_value());  // not granted to this user
    EXPECT_EQ(rig.gateway.request(0).method, "GET");
    EXPECT_EQ(rig.gateway.request(0).url, "https://gateway.example/llm/v1/models");
}

TEST(ModelImages, ImageWithANonVisionRoleIsRefusedBeforeSending) {
    Rig rig;
    auto req = simple_request(Role::main);
    req.messages[0].images.push_back({"image/png", base64_encode(make_png(8, 8)), true});
    auto r = rig.client->chat(req, {});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::invalid_request);
    EXPECT_EQ(rig.gateway.request_count(), 0U);
    for (const Role role : {Role::small, Role::embed, Role::rerank}) {
        req.role = role;
        EXPECT_FALSE(rig.client->chat(req, {}).ok());
    }
    EXPECT_EQ(rig.gateway.request_count(), 0U);
}

TEST(ModelImages, FourKScreenshotIsDownscaledBeforeSending) {
    Rig rig;
    rig.gateway.push(streamed(text_stream({"seen"})));
    const auto png = make_png(3840, 2160);
    ASSERT_GT(png.size(), 20u * 1024 * 1024);
    auto req = simple_request(Role::vision);
    req.messages[0].images.push_back({"image/png", base64_encode(png), false});
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok()) << r.failure().message;

    const json body = rig.gateway.request(0).json_body();
    const std::string url = body["messages"][0]["content"][1]["image_url"]["url"].get<std::string>();
    const std::string prefix = "data:image/png;base64,";
    ASSERT_EQ(url.substr(0, prefix.size()), prefix);
    const auto bytes = base64_decode(url.substr(prefix.size()));
    ASSERT_TRUE(bytes.has_value());
    EXPECT_LE(bytes->size(), 5u * 1024 * 1024);
    auto info = prepare_image(*bytes);
    ASSERT_TRUE(info) << info.error().message;
    EXPECT_EQ(info->width, 1568);  // aspect ratio preserved: 3840x2160 -> 1568x882
    EXPECT_EQ(info->height, 882);
    EXPECT_FALSE(info->resized);   // already within the limits
}

TEST(ModelImages, FitWithinPreservesAspectRatioAndNeverEnlarges) {
    EXPECT_EQ(fit_within(3840, 2160, 1568), (std::pair<int, int>{1568, 882}));
    EXPECT_EQ(fit_within(2160, 3840, 1568), (std::pair<int, int>{882, 1568}));
    EXPECT_EQ(fit_within(800, 600, 1568), (std::pair<int, int>{800, 600}));
    EXPECT_EQ(fit_within(10000, 1, 1568), (std::pair<int, int>{1568, 1}));
}

TEST(ModelImages, ByteLimitReencodesAsJpeg) {
    ImageLimits limits;
    limits.max_side = 4000;
    limits.max_bytes = 600 * 1024;
    auto r = prepare_image(make_png(1600, 900), limits);  // a ~4 MB uncompressed-PNG source
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_LE(r->bytes.size(), limits.max_bytes);
    EXPECT_TRUE(r->resized);
}

TEST(ModelImages, GarbageIsRejected) {
    EXPECT_FALSE(prepare_image({1, 2, 3, 4}));
    EXPECT_FALSE(prepare_image({}));
}

TEST(ModelImages, OnlyTheNewestImagesUpToThePolicyLimitAreSent) {
    Rig rig([](ModelClientConfig& c) { c.max_images_per_request = 2; });
    rig.gateway.push(streamed(text_stream({"ok"})));
    auto req = simple_request(Role::vision);
    const std::string png = base64_encode(make_png(16, 16));
    for (int i = 0; i < 5; ++i) {
        ChatMessage m{"user", "img " + std::to_string(i), {}, {}, {}};
        m.images.push_back({"image/png", png, true});
        req.messages.push_back(m);
    }
    ASSERT_TRUE(rig.client->chat(req, {}).ok());
    int images = 0;
    const json sent = rig.gateway.request(0).json_body();
    for (const auto& m : sent["messages"])
        if (m["content"].is_array())
            for (const auto& part : m["content"])
                if (part["type"] == "image_url") ++images;
    EXPECT_EQ(images, 2);
}

TEST(ModelUsage, TotalsPerRunIncludeReasoningTokens) {
    Rig rig;
    rig.gateway.push(streamed(text_stream({"a"}, {"thinking"}, 100, 40, 25)));
    rig.gateway.push(streamed(text_stream({"b"}, {}, 120, 10, 0)));
    auto first = simple_request();
    auto second = simple_request(Role::small);
    auto other = simple_request();
    other.run_id = "run-2";
    ASSERT_TRUE(rig.client->chat(first, {}).ok());
    ASSERT_TRUE(rig.client->chat(second, {}).ok());
    rig.gateway.push(streamed(text_stream({"c"}, {}, 7, 3, 0)));
    ASSERT_TRUE(rig.client->chat(other, {}).ok());

    const Usage run1 = rig.usage.run_total("run-1");
    EXPECT_EQ(run1.prompt, 220);
    EXPECT_EQ(run1.completion, 50);
    EXPECT_EQ(run1.reasoning, 25);
    EXPECT_EQ(run1.requests, 2);
    EXPECT_EQ(rig.usage.run_total("run-2").prompt, 7);
    EXPECT_EQ(rig.usage.thread_total("thread-1").requests, 3);
    EXPECT_EQ(rig.usage.role_total(Role::small).prompt, 120);
}

// ---- 2.5 Embeddings and rerank -----------------------------------------------------------------------------

TEST(ModelEmbeddings, SixteenChunksGiveSixteenVectorsWithModelAndDimension) {
    Rig rig;
    json data = json::array();
    for (int i = 15; i >= 0; --i) {  // out of order on purpose: results are matched by index
        json vec = json::array();
        for (int k = 0; k < 8; ++k) vec.push_back(static_cast<double>(i) + k / 10.0);
        data.push_back({{"index", i}, {"embedding", vec}});
    }
    Scripted s;
    s.body = json{{"object", "list"}, {"model", "acme-llm-embedding"}, {"dimension", 8}, {"data", data},
                  {"usage", {{"prompt_tokens", 160}, {"total_tokens", 160}}}}.dump();
    rig.gateway.push(s);
    std::vector<std::string> texts;
    for (int i = 0; i < 16; ++i) texts.push_back("chunk " + std::to_string(i));
    RequestContext ctx;
    ctx.feature = "knowledge.index";
    auto r = rig.client->embed(texts, ctx);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(rig.gateway.request_count(), 1U);  // one request for the whole batch
    EXPECT_EQ(rig.gateway.request(0).url, "https://gateway.example/llm/v1/embeddings");
    EXPECT_EQ(rig.gateway.request(0).json_body()["model"], "embed");
    EXPECT_EQ(rig.gateway.request(0).json_body()["input"].size(), 16U);
    ASSERT_EQ(r->vectors.size(), 16U);
    EXPECT_EQ(r->model, "acme-llm-embedding");
    EXPECT_EQ(r->dimension, 8);
    EXPECT_FLOAT_EQ(r->vectors[3][0], 3.0F);
    EXPECT_EQ(rig.usage.feature_total("knowledge.index").prompt, 160);  // attributed to the feature
}

TEST(ModelEmbeddings, LargeBatchesAreSplit) {
    Rig rig([](ModelClientConfig& c) { c.embed_batch_size = 10; });
    for (int b = 0; b < 3; ++b) {
        const int n = b < 2 ? 10 : 5;
        json data = json::array();
        for (int i = 0; i < n; ++i) data.push_back({{"index", i}, {"embedding", {1.0, 2.0}}});
        Scripted s;
        s.body = json{{"model", "m"}, {"dimension", 2}, {"data", data}}.dump();
        rig.gateway.push(s);
    }
    auto r = rig.client->embed(std::vector<std::string>(25, "x"));
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->vectors.size(), 25U);
    EXPECT_EQ(rig.gateway.request_count(), 3U);
}

TEST(ModelEmbeddings, WrongVectorCountIsAFailure) {
    Rig rig([](ModelClientConfig& c) { c.max_retries = 0; });
    Scripted s;
    s.body = R"({"model":"m","data":[{"index":0,"embedding":[1,2]}]})";
    rig.gateway.push(s);
    EXPECT_FALSE(rig.client->embed({"a", "b"}).ok());
}

TEST(ModelRerank, ThirtyPassagesGiveThirtyScoresInInputOrder) {
    Rig rig;
    json results = json::array();
    for (int i = 0; i < 30; ++i) results.push_back({{"index", 29 - i}, {"relevance_score", (29 - i) / 100.0}});  // sorted by score
    Scripted s;
    s.body = json{{"results", results}, {"meta", {{"billed_units", {{"input_tokens", 800}}}}}}.dump();
    rig.gateway.push(s);
    std::vector<std::string> passages;
    for (int i = 0; i < 30; ++i) passages.push_back("passage " + std::to_string(i));
    auto r = rig.client->rerank("query", passages);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    ASSERT_EQ(r->size(), 30U);
    for (int i = 0; i < 30; ++i) EXPECT_DOUBLE_EQ(r.value()[static_cast<size_t>(i)], i / 100.0);
    const json body = rig.gateway.request(0).json_body();
    EXPECT_EQ(body["model"], "rerank");
    EXPECT_EQ(body["documents"].size(), 30U);
    EXPECT_EQ(rig.gateway.request(0).url, "https://gateway.example/llm/v1/rerank");
}

TEST(ModelRerank, MissingScoresAreAFailure) {
    Rig rig([](ModelClientConfig& c) { c.max_retries = 0; });
    Scripted s;
    s.body = R"({"results":[{"index":0,"relevance_score":0.5}]})";
    rig.gateway.push(s);
    EXPECT_FALSE(rig.client->rerank("q", {"a", "b"}).ok());
}

// ---- add-image-generation: image generation requests ----------------------------------------------------------

namespace {

Scripted images_response(const std::vector<std::vector<std::uint8_t>>& images) {
    json data = json::array();
    for (const auto& img : images) data.push_back({{"b64_json", base64_encode(img)}, {"revised_prompt", ""}});
    Scripted s;
    s.body = json{{"created", 1}, {"output_format", "png"}, {"data", data}, {"usage", {{"total_tokens", 0}}}}.dump();
    return s;
}

}  // namespace

TEST(ModelImages, OneRequestWithTheImageRoleReturnsTheDecodedImages) {
    Rig rig;
    const std::vector<std::uint8_t> a = {1, 2, 3, 4}, b = {5, 6, 7};
    rig.gateway.push(images_response({a, b}));
    ImageRequest req;
    req.prompt = "a red car";
    req.size = "1536x1024";
    req.count = 2;
    RequestContext ctx;
    ctx.run_id = "run-7";
    auto r = rig.client->generate_images(req, ctx);
    ASSERT_TRUE(r.ok()) << r.failure().message;
    ASSERT_EQ(r->size(), 2U);
    EXPECT_EQ(r.value()[0], a);
    EXPECT_EQ(r.value()[1], b);
    ASSERT_EQ(rig.gateway.request_count(), 1U);
    const Recorded sent = rig.gateway.request(0);
    EXPECT_EQ(sent.url, "https://gateway.example/llm/v1/images/generations");
    EXPECT_EQ(sent.json_body(), (json{{"model", "image"}, {"prompt", "a red car"}, {"n", 2}, {"size", "1536x1024"}}));
    EXPECT_EQ(sent.header("X-Skowork-Run"), "run-7");
    EXPECT_EQ(rig.usage.role_total(Role::image).requests, 1);
    EXPECT_EQ(parse_role("image"), Role::image);
}

TEST(ModelImages, FailuresAreReturnedWithoutRetrying) {
    Rig rig;
    rig.gateway.push(error_response(503, "MODEL_UNAVAILABLE", "backend unavailable"));
    auto r = rig.client->generate_images({"a car", "1024x1024", 1});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::model_unavailable);
    EXPECT_EQ(rig.gateway.request_count(), 1U);
    EXPECT_TRUE(rig.sleeps.empty());

    rig.gateway.push(error_response(429, "QUOTA_EXCEEDED", "quota", 0, "2026-10-25T00:00:00.000Z"));
    r = rig.client->generate_images({"a car", "1024x1024", 1});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::quota_exceeded);
    EXPECT_EQ(rig.gateway.request_count(), 2U);
}

TEST(ModelImages, UndecodableImageDataIsAFailure) {
    Rig rig;
    Scripted s;
    s.body = R"({"data":[{"b64_json":"%%% not base64 %%%"}]})";
    rig.gateway.push(s);
    EXPECT_FALSE(rig.client->generate_images({"a car", "1024x1024", 1}).ok());
    s.body = R"({"data":[]})";
    rig.gateway.push(s);
    EXPECT_FALSE(rig.client->generate_images({"a car", "1024x1024", 1}).ok());
}

TEST(ModelImages, CancellingWhileGeneratingDropsTheImages) {
    Rig rig;
    RequestContext ctx;
    ctx.cancel = std::make_shared<CancelToken>();
    Scripted s = images_response({{1, 2, 3}});
    s.on_start = [&] { ctx.cancel->cancel(); };  // the user stops the Run while the request is in flight
    rig.gateway.push(s);
    auto r = rig.client->generate_images({"a car", "1024x1024", 1}, ctx);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.failure().code, code::cancelled);
    EXPECT_EQ(rig.usage.role_total(Role::image).requests, 0);
    // Already cancelled: nothing is sent.
    EXPECT_FALSE(rig.client->generate_images({"a car", "1024x1024", 1}, ctx).ok());
    EXPECT_EQ(rig.gateway.request_count(), 1U);
}
// ---- 2.6 Reasoning ------------------------------------------------------------------------------------------

TEST(ModelReasoning, StreamedReasoningIsDeliveredSeparatelyAndNeverResent) {
    Rig rig;
    rig.gateway.push(streamed(text_stream({"The answer ", "is 42."}, {"Let me ", "think about it."}, 30, 20, 8)));
    std::string reasoning, text;
    ChatSink sink;
    sink.on_reasoning = [&](std::string_view t) { reasoning += t; };
    sink.on_text = [&](std::string_view t) { text += t; };
    auto first = rig.client->chat(simple_request(), sink);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(reasoning, "Let me think about it.");
    EXPECT_EQ(text, "The answer is 42.");
    EXPECT_EQ(first->reasoning, "Let me think about it.");
    EXPECT_EQ(first->text, "The answer is 42.");
    EXPECT_EQ(first->usage.reasoning, 8);

    // The next turn carries the answer only.
    rig.gateway.push(streamed(text_stream({"fine"})));
    auto next = simple_request();
    next.messages.push_back({"assistant", first->text, {}, {}, {}});
    next.messages.push_back({"user", "thanks", {}, {}, {}});
    ASSERT_TRUE(rig.client->chat(next, {}).ok());
    const std::string sent = rig.gateway.request(1).body;
    EXPECT_NE(sent.find("The answer is 42."), std::string::npos);
    EXPECT_EQ(sent.find("think about it"), std::string::npos);
    EXPECT_EQ(sent.find("reasoning"), std::string::npos);
}

TEST(ModelReasoning, NonStreamedReasoningContent) {
    Rig rig;
    Scripted s;
    s.body = R"({"model":"acme-llm-primary","choices":[{"message":{"role":"assistant","content":"done","reasoning_content":"hmm"},
                 "finish_reason":"stop"}],"usage":{"prompt_tokens":5,"completion_tokens":9,"total_tokens":14,
                 "completion_tokens_details":{"reasoning_tokens":6}}})";
    rig.gateway.push(s);
    auto req = simple_request();
    req.stream = false;
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok()) << r.failure().message;
    EXPECT_EQ(r->text, "done");
    EXPECT_EQ(r->reasoning, "hmm");
    EXPECT_EQ(r->usage.reasoning, 6);
    EXPECT_FALSE(rig.gateway.request(0).json_body()["stream"].get<bool>());
}

TEST(ModelReasoning, AlternativeFieldNameIsAccepted) {
    Rig rig;
    rig.gateway.push(streamed({sse(delta_chunk({{"reasoning", "mm"}})), sse(delta_chunk({{"content", "ok"}})),
                               sse(delta_chunk(json::object(), "stop")), sse_done()}));
    auto r = rig.client->chat(simple_request(), {});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->reasoning, "mm");
    EXPECT_EQ(r->text, "ok");
}
