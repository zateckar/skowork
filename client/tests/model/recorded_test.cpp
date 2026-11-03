// client/model-access against responses recorded from the real gateway and organization LLM API (fixtures/, written by
// e2e_tests ModelE2E.* with WORKBENCH_E2E_RECORD; embedding vectors trimmed to their first 8 values). Covers tasks 2.1
// (recorded streams, cut streams), 2.5 (recorded embedding and rerank responses) and 2.6 (recorded reasoning).
#include "fake_gateway.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace wb;
using namespace wb::model;
using namespace wb::testing;

namespace {

std::string fixture(const std::string& name) {
    std::ifstream in(std::string(WORKBENCH_MODEL_FIXTURES) + "/" + name, std::ios::binary);
    EXPECT_TRUE(in) << name;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// What a stream says, read independently of the client: answer text, reasoning, finish reason, usage.
struct Expected {
    std::string text, reasoning, finish;
    json usage;
};

Expected scan(const std::string& sse) {
    Expected e;
    std::istringstream lines(sse);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("data: ", 0) != 0 || line == "data: [DONE]") continue;
        const json j = json::parse(line.substr(6));
        if (j.contains("usage") && j["usage"].is_object()) e.usage = j["usage"];
        for (const auto& c : j.value("choices", json::array())) {
            const json d = c.value("delta", json::object());
            if (d.contains("content") && d["content"].is_string()) e.text += d["content"].get<std::string>();
            if (d.contains("reasoning_content") && d["reasoning_content"].is_string()) e.reasoning += d["reasoning_content"].get<std::string>();
            if (c.contains("finish_reason") && c["finish_reason"].is_string()) e.finish = c["finish_reason"].get<std::string>();
        }
    }
    return e;
}

std::vector<std::string> split(const std::string& s, size_t size) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size(); i += size) out.push_back(s.substr(i, size));
    return out;
}

struct Rig {
    FakeGateway gateway;
    FakeAuth auth;
    std::unique_ptr<ModelClient> client;
    explicit Rig(int retries = 3) {
        ModelClientConfig cfg;
        cfg.gateway_url = [] { return "https://gateway.example/llm/v1"; };
        cfg.max_retries = retries;
        cfg.sleep = [](std::chrono::milliseconds, const std::shared_ptr<CancelToken>&) { return true; };
        client = std::make_unique<ModelClient>(std::move(cfg), auth, gateway);
    }
};

ChatRequest ask(const std::string& text, Role role = Role::main) {
    ChatRequest r;
    r.role = role;
    ChatMessage m;
    m.role = "user";
    m.content = text;
    r.messages.push_back(m);
    return r;
}

ToolSpec weather_tool() {
    return {"weather.current", "Current weather for a city",
            json::parse(R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})")};
}

}  // namespace

// ---- 2.1 / 2.6 recorded streams ----------------------------------------------------------------------------------

TEST(ModelRecorded, ReasoningStreamGivesTheSameResultAtEveryChunkSize) {
    const std::string sse = fixture("primary_reasoning_stream.sse");
    const Expected want = scan(sse);
    ASSERT_FALSE(want.reasoning.empty());
    ASSERT_NE(want.text.find("391"), std::string::npos);
    for (const size_t size : {size_t{1}, size_t{7}, size_t{64}, size_t{1000}, sse.size()}) {
        Rig rig;
        rig.gateway.push(streamed(split(sse, size)));
        std::string text, reasoning;
        ChatSink sink;
        sink.on_text = [&](std::string_view t) { text += t; };
        sink.on_reasoning = [&](std::string_view t) { reasoning += t; };
        auto r = rig.client->chat(ask("What is 17 multiplied by 23?"), sink);
        ASSERT_TRUE(r.ok()) << size << ": " << r.failure().reason;
        EXPECT_EQ(r->text, want.text) << size;
        EXPECT_EQ(r->reasoning, want.reasoning) << size;
        EXPECT_EQ(text, want.text);
        EXPECT_EQ(reasoning, want.reasoning);  // delivered separately from the answer
        EXPECT_EQ(r->finish_reason, "stop");
        EXPECT_EQ(r->usage.prompt, want.usage["prompt_tokens"].get<long long>());
        EXPECT_EQ(r->usage.completion, want.usage["completion_tokens"].get<long long>());
        EXPECT_EQ(r->usage.reasoning, want.usage["completion_tokens_details"]["reasoning_tokens"].get<long long>());
    }
}

TEST(ModelRecorded, ReasoningIsAbsentFromTheNextTurnsRequest) {
    const std::string sse = fixture("primary_reasoning_stream.sse");
    Rig rig;
    rig.gateway.push(streamed(split(sse, 256)));
    rig.gateway.push(streamed(split(sse, 256)));
    auto req = ask("What is 17 multiplied by 23?");
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok());
    ChatMessage answer;
    answer.role = "assistant";
    answer.content = r->text;
    req.messages.push_back(answer);
    ChatMessage next;
    next.role = "user";
    next.content = "And 18 multiplied by 23?";
    req.messages.push_back(next);
    ASSERT_TRUE(rig.client->chat(req, {}).ok());
    const std::string body = rig.gateway.request(1).body;
    EXPECT_EQ(body.find("reasoning"), std::string::npos);
    EXPECT_EQ(body.find(r->reasoning.substr(0, 30)), std::string::npos);
    EXPECT_NE(body.find(r->text), std::string::npos);
}

TEST(ModelRecorded, NonStreamedReasoningResponseKeepsReasoningSeparate) {
    const json plain = json::parse(fixture("primary_reasoning_plain.json"));
    Rig rig;
    Scripted s;
    s.body = plain.dump();
    rig.gateway.push(s);
    auto req = ask("Name the capital of the Czech Republic.");
    req.stream = false;
    auto r = rig.client->chat(req, {});
    ASSERT_TRUE(r.ok()) << r.failure().reason;
    EXPECT_EQ(r->text, plain["choices"][0]["message"]["content"].get<std::string>());
    EXPECT_EQ(r->reasoning, plain["choices"][0]["message"]["reasoning_content"].get<std::string>());
    EXPECT_EQ(r->usage.reasoning, plain["usage"]["completion_tokens_details"]["reasoning_tokens"].get<long long>());
}

TEST(ModelRecorded, StreamCutAnywhereBeforeTheFinishReasonIsTruncated) {
    const std::string sse = fixture("primary_reasoning_stream.sse");
    const size_t finish = sse.find("\"finish_reason\":\"stop\"");
    ASSERT_NE(finish, std::string::npos);
    const size_t finish_event = sse.rfind("data: ", finish);
    // Cut at every event boundary before the finish event, and in the middle of events.
    std::vector<size_t> cuts;
    for (size_t p = sse.find("\n\n"); p != std::string::npos && p < finish_event; p = sse.find("\n\n", p + 2)) cuts.push_back(p + 2);
    for (size_t p = 37; p < finish_event; p += 211) cuts.push_back(p);
    ASSERT_GT(cuts.size(), 10u);
    for (const size_t cut : cuts) {
        Rig rig(0);
        rig.gateway.push(streamed({sse.substr(0, cut)}));
        auto r = rig.client->chat(ask("What is 17 multiplied by 23?"), {});
        ASSERT_FALSE(r.ok()) << "cut at " << cut;
        EXPECT_EQ(r.failure().code, code::model_unavailable);
        EXPECT_EQ(r.failure().reason, "stream truncated") << "cut at " << cut;
        EXPECT_TRUE(r.failure().retryable);
    }
    // Cut after the finish reason but before [DONE]: complete.
    Rig rig;
    rig.gateway.push(streamed({sse.substr(0, sse.find("\n\n", finish) + 2)}));
    auto r = rig.client->chat(ask("What is 17 multiplied by 23?"), {});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->text, scan(sse).text);
}

TEST(ModelRecorded, CutStreamIsRetriedAndTheSecondAttemptSucceeds) {
    const std::string sse = fixture("primary_reasoning_stream.sse");
    Rig rig;
    rig.gateway.push(streamed({sse.substr(0, sse.size() / 2)}));
    rig.gateway.push(streamed(split(sse, 100)));
    int retries = 0;
    ChatSink sink;
    sink.on_retry = [&](int, const ModelFailure& f, std::chrono::milliseconds) {
        ++retries;
        EXPECT_EQ(f.reason, "stream truncated");
    };
    auto r = rig.client->chat(ask("What is 17 multiplied by 23?"), sink);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(retries, 1);
    EXPECT_EQ(r->text, scan(sse).text);
}

TEST(ModelRecorded, ToolCallStreamsOfBothChatModels) {
    for (const char* name : {"primary_tool_call_stream.sse", "secondary_tool_call_stream.sse"}) {
        const std::string sse = fixture(name);
        Rig rig;
        rig.gateway.push(streamed(split(sse, 13)));
        auto req = ask("What is the weather in Springfield right now?");
        req.tools = {weather_tool()};
        std::vector<std::string> announced;
        ChatSink sink;
        sink.on_tool_call = [&](int, const std::string&, const std::string& tool) { announced.push_back(tool); };
        auto r = rig.client->chat(req, sink);
        ASSERT_TRUE(r.ok()) << name << ": " << r.failure().reason;
        ASSERT_EQ(r->tool_calls.size(), 1u) << name;
        EXPECT_EQ(r->tool_calls[0].tool, "weather.current") << name;
        EXPECT_TRUE(r->tool_calls[0].valid) << name << ": " << r->tool_calls[0].validation_error;
        EXPECT_NE(jstr(r->tool_calls[0].arguments, "city").find("Springfield"), std::string::npos) << name;
        EXPECT_EQ(r->finish_reason, "tool_calls") << name;
        EXPECT_EQ(announced, std::vector<std::string>{"weather.current"}) << name;
        EXPECT_EQ(rig.gateway.request_count(), 1u) << name;  // valid as recorded: no corrective re-prompt
    }
}

// ---- 2.5 recorded embedding and rerank responses ----------------------------------------------------------------

TEST(ModelRecorded, EmbeddingOfSixteenChunks) {
    const json recorded = json::parse(fixture("embedding_16_trimmed.json"));
    Rig rig;
    Scripted s;
    s.body = recorded.dump();
    rig.gateway.push(s);
    std::vector<std::string> chunks(16, "chunk");
    auto r = rig.client->embed(chunks);
    ASSERT_TRUE(r.ok()) << r.failure().reason;
    ASSERT_EQ(r->vectors.size(), 16u);
    EXPECT_EQ(r->model, "acme-llm-embedding");
    EXPECT_EQ(r->dimension, 8);
    for (const auto& item : recorded["data"]) {
        const auto idx = item["index"].get<size_t>();
        ASSERT_LT(idx, 16u);
        ASSERT_EQ(r->vectors[idx].size(), 8u);
        EXPECT_FLOAT_EQ(r->vectors[idx][0], item["embedding"][0].get<float>());
    }
    EXPECT_EQ(r->usage.prompt, recorded["usage"]["prompt_tokens"].get<long long>());
}

TEST(ModelRecorded, RerankOfThirtyPassagesInInputOrder) {
    const json recorded = json::parse(fixture("rerank_30.json"));
    Rig rig;
    Scripted s;
    s.body = recorded.dump();
    rig.gateway.push(s);
    std::vector<std::string> passages(30, "passage");
    auto r = rig.client->rerank("Where is the Acme Falcon built?", passages);
    ASSERT_TRUE(r.ok()) << r.failure().reason;
    ASSERT_EQ(r->size(), 30u);
    // The upstream sorts by score; the client returns input order.
    for (const auto& item : recorded["results"])
        EXPECT_DOUBLE_EQ(r.value()[item["index"].get<size_t>()], item["relevance_score"].get<double>());
    EXPECT_EQ(std::max_element(r->begin(), r->end()) - r->begin(), 17);
}
