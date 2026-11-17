#include <gtest/gtest.h>

#include "fake_gateway.h"

using namespace wb;
using namespace wb::model;
using namespace wb::testing;

namespace {
struct Rig {
    FakeGateway gateway;
    FakeAuth auth;
    ModelClientConfig config;
    std::unique_ptr<ModelClient> client;
    Rig() {
        config.gateway_url = [] { return "https://gateway.example/llm/v1"; };
        config.sleep = [](auto, auto) { return true; };
        client = std::make_unique<ModelClient>(config, auth, gateway);
    }
};
ChatRequest request() {
    ChatRequest req;
    req.single_attempt = true;
    req.messages.push_back({"user", "classify"});
    return req;
}
}

TEST(ModelSingleAttempt, TransientFailureDoesNotResend) {
    Rig rig;
    rig.gateway.push(error_response(503, code::model_unavailable));
    rig.gateway.push(streamed(text_stream({"later"})));
    const auto result = rig.client->chat(request(), {});
    EXPECT_FALSE(result);
    EXPECT_EQ(rig.gateway.request_count(), 1u);
}

TEST(ModelSingleAttempt, UnauthorizedRefreshDoesNotResend) {
    Rig rig;
    rig.gateway.push(error_response(401, code::auth_expired));
    rig.gateway.push(streamed(text_stream({"later"})));
    const auto result = rig.client->chat(request(), {});
    EXPECT_FALSE(result);
    EXPECT_EQ(rig.gateway.request_count(), 1u);
    EXPECT_EQ(rig.auth.refreshes, 1);
    ASSERT_TRUE(rig.client->chat(request(), {}));
    EXPECT_EQ(rig.gateway.request_count(), 2u);
    EXPECT_EQ(rig.gateway.request(1).header("Authorization"), "Bearer token-2");
}

TEST(ModelSingleAttempt, UnexpectedToolCallDoesNotTriggerRepairRequest) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("c", "unexpected", {"{}"})));
    rig.gateway.push(streamed(text_stream({"repair"})));
    const auto result = rig.client->chat(request(), {});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->tool_calls.size(), 1u);
    EXPECT_EQ(rig.gateway.request_count(), 1u);
}

TEST(ModelSingleAttempt, DefaultRequestsStillRetry) {
    Rig rig;
    rig.gateway.push(error_response(503, code::model_unavailable));
    rig.gateway.push(streamed(text_stream({"recovered"})));
    auto req = request();
    req.single_attempt = false;
    const auto result = rig.client->chat(req, {});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->text, "recovered");
    EXPECT_EQ(rig.gateway.request_count(), 2u);
}

TEST(ModelSingleAttempt, ValidToolArgumentsAreStillValidatedLocally) {
    Rig rig;
    rig.gateway.push(streamed(tool_stream("c", "files__read", {R"({"path":"report.txt"})"})));
    auto req = request();
    req.tools.push_back({"files.read", "read a file", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}},
                                                      {"required", json::array({"path"})}}});
    const auto result = rig.client->chat(req, {});
    ASSERT_TRUE(result);
    ASSERT_EQ(result->tool_calls.size(), 1u);
    EXPECT_TRUE(result->tool_calls[0].valid);
    EXPECT_EQ(rig.gateway.request_count(), 1u);
}
