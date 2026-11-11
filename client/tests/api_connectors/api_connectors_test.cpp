// client/api-connectors (add-api-connectors tasks 6.1, 6.2 and 6.4): the API connector tools of the Policy bundle, the
// argument check before any request, the call through the control plane (against a local fake control plane), the risk
// and provenance of the tools, and the personal access token calls (never stored, logged or returned).
#include "api_connectors/connectors.h"
#include "api_connectors/token_draft.h"
#include "platform/log.h"
#include "policy/core.h"
#include "support/http_server.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace wb;
using namespace wb::connectors;
using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

const std::string kToken = "pat-7f3c9d2e-SECRET-VALUE";

// The control plane as the tests need it: every request is recorded; the answer is chosen by `answer`.
struct FakeControlPlane {
    std::function<std::string(const test::LoopbackHttpServer::Request&)> answer = [](const test::LoopbackHttpServer::Request&) {
        return test::LoopbackHttpServer::response(404, "Not Found", R"({"error":{"code":"NOT_FOUND","message":"no route"}})",
                                                  "Content-Type: application/json\r\n");
    };
    std::unique_ptr<test::LoopbackHttpServer> server;
    FakeControlPlane() {
        server = std::make_unique<test::LoopbackHttpServer>([this](const test::LoopbackHttpServer::Request& r) { return answer(r); });
    }
    [[nodiscard]] std::vector<test::LoopbackHttpServer::Request> requests() const { return server->requests(); }
    [[nodiscard]] Request request() const {
        return [this](const std::string& method, const std::string& path, const std::string& body) -> Result<HttpResponse> {
            HttpRequest r;
            r.method = method;
            r.url = server->url("/" + path);
            r.body = body;
            if (!body.empty()) r.headers.emplace_back("Content-Type", "application/json");
            r.connect_timeout = 3s;
            r.idle_timeout = 10s;
            return http().send(r);
        };
    }
};

std::string json_reply(int status, const std::string& reason, const json& body) {
    return test::LoopbackHttpServer::response(status, reason, body.dump(), "Content-Type: application/json\r\n");
}

// The agent runtime's registry, as the manager sees it.
struct Registry {
    std::mutex mx;
    std::map<std::string, ToolDef> tools;
    Status add(ToolDef d) {
        const std::lock_guard lock(mx);
        const std::string id = d.id;
        tools[id] = std::move(d);
        return {};
    }
    bool remove(const std::string& id) {
        const std::lock_guard lock(mx);
        return tools.erase(id) > 0;
    }
    bool has(const std::string& id) {
        const std::lock_guard lock(mx);
        return tools.count(id) > 0;
    }
    std::optional<ToolDef> get(const std::string& id) {
        const std::lock_guard lock(mx);
        const auto it = tools.find(id);
        if (it == tools.end()) return std::nullopt;
        return it->second;
    }
    size_t size() {
        const std::lock_guard lock(mx);
        return tools.size();
    }
};

json orders_operation(const std::string& name, const std::string& risk) {
    const json schema = {{"type", "object"},
                         {"properties", {{"id", {{"type", "string"}}}, {"qty", {{"type", "integer"}}}}},
                         {"required", json::array({"id"})},
                         {"additionalProperties", false}};
    return {{"name", name}, {"description", "Returns one order by its id, from the order service of the warehouse."}, {"risk", risk}, {"input_schema", schema}};
}

// An api entry as the control plane delivers it. `selected` replaces the two selected operations when given.
json orders_connector(const std::string& state = "ready", std::optional<json> selected = std::nullopt) {
    const json ops = selected ? *selected : json::array({orders_operation("getOrder", "read"), orders_operation("createOrder", "external")});
    return {{"id", "orders-api"}, {"name", "Orders"}, {"kind", "api"}, {"auth", "none"}, {"state", state}, {"operations", ops},
            {"include_tools", json::array()}, {"exclude_tools", json::array()}, {"risk", {{"getOrder", "read"}, {"createOrder", "external"}}}};
}

json bundle_with(const json& servers, const json& tools_rules = json::object()) {
    json p = {{"mcp", {{"servers", servers}}}};
    if (!tools_rules.empty()) p["tools"] = {{"rules", tools_rules}};
    return p;
}

struct ManagerFixture {
    FakeControlPlane cp;
    Registry reg;
    std::mutex pmx;
    std::shared_ptr<const policy::EffectivePolicy> current;
    std::unique_ptr<ConnectorManager> mgr;

    void set_policy(const json& p) {
        policy::PolicyBundle b;
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = p;
        const std::lock_guard lock(pmx);
        current = std::make_shared<policy::EffectivePolicy>(b);
    }
    std::shared_ptr<const policy::EffectivePolicy> policy() {
        const std::lock_guard lock(pmx);
        return current;
    }
    ConnectorManager& start() {
        ManagerEnv env;
        env.policy = [this] { return policy(); };
        env.request = cp.request();
        env.add_tool = [this](ToolDef d) { return reg.add(std::move(d)); };
        env.remove_tool = [this](const std::string& id) { return reg.remove(id); };
        mgr = std::make_unique<ConnectorManager>(std::move(env));
        return *mgr;
    }
    runtime::ToolOutput call(const std::string& id, const json& args) {
        auto d = reg.get(id);
        if (!d) return runtime::ToolOutput::failure("no such tool " + id);
        ToolContext ctx;
        ctx.run_id = "run-1";
        ctx.call_id = "call-1";
        return d->run(ctx, args);
    }
};

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

// ---- catalog entries of kind api ----------------------------------------------------------------------------------------

TEST(ApiConnectorEntries, KeepsApiEntriesAndReportsTheBadOnes) {
    const json servers = json::array({
        {{"id", "jira"}, {"name", "Jira"}, {"kind", "remote"}, {"url", "https://jira.example/mcp"}, {"auth", "none"}},
        {{"id", "bad id!"}, {"name", "Bad"}, {"kind", "api"}, {"auth", "none"}, {"state", "ready"}, {"operations", json::array()}},
        orders_connector(),
        {{"id", "dup-api"}, {"name", "Dup"}, {"kind", "api"}, {"auth", "none"}, {"state", "ready"},
         {"operations", json::array({orders_operation("a", "read"), orders_operation("a", "read"), orders_operation("bad name!", "read")})}},
    });
    std::vector<std::string> problems;
    const auto list = parse_connectors(&servers, &problems);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].id, "orders-api");
    ASSERT_EQ(list[0].operations.size(), 2u);
    EXPECT_EQ(list[0].operations[0].risk, Risk::read);
    EXPECT_EQ(list[0].operations[1].risk, Risk::external);
    EXPECT_EQ(list[1].id, "dup-api");
    EXPECT_EQ(list[1].operations.size(), 1u);  // the repeated name and the invalid name are skipped
    // The MCP entry is not a problem; the invalid id and the two bad operations are.
    std::string all;
    for (const auto& p : problems) all += p + " | ";
    EXPECT_EQ(problems.size(), 3u) << all;
}

TEST(ApiConnectorEntries, ToolIdsFollowTheContract) { EXPECT_EQ(tool_id("orders-api", "getOrder"), "api:orders-api/getOrder"); }

TEST(ApiConnectorEntries, ArgumentsAreCheckedAgainstTheSchema) {
    const json schema = orders_operation("getOrder", "read")["input_schema"];
    const auto missing = argument_problem(schema, json::object());
    ASSERT_TRUE(missing.has_value());
    EXPECT_NE(missing->find("id"), std::string::npos) << *missing;
    const auto wrong_type = argument_problem(schema, {{"id", 5}});
    ASSERT_TRUE(wrong_type.has_value());
    EXPECT_NE(wrong_type->find("id"), std::string::npos) << *wrong_type;
    const auto wrong_qty = argument_problem(schema, {{"id", "1"}, {"qty", "many"}});
    ASSERT_TRUE(wrong_qty.has_value());
    EXPECT_NE(wrong_qty->find("qty"), std::string::npos) << *wrong_qty;
    EXPECT_FALSE(argument_problem(schema, {{"id", "1"}, {"qty", 2}}).has_value());
}

TEST(ApiConnectorEntries, CallAnswersBecomeToolResults) {
    const auto ok = call_output(200, R"({"status":200,"body":{"id":"42"},"truncated":false})");
    EXPECT_TRUE(ok.ok);
    EXPECT_NE(ok.content.find("\"id\":\"42\""), std::string::npos) << ok.content;

    const auto cut = call_output(200, R"({"status":200,"body":"abc","truncated":true})");
    EXPECT_TRUE(cut.ok);
    EXPECT_EQ(cut.content.rfind("The response was truncated at 256 KB.", 0), 0u);

    const auto forbidden = call_output(403, R"({"error":{"code":"FORBIDDEN","message":"The API refused this operation for your account (403).","status":403,"text":"no access to project X","reason":"forbidden operation"}})");
    EXPECT_FALSE(forbidden.ok);
    EXPECT_NE(forbidden.content.find("FORBIDDEN"), std::string::npos);
    EXPECT_NE(forbidden.content.find("no access to project X"), std::string::npos);

    const auto busy = call_output(429, R"({"error":{"code":"BUSY","message":"The connector is busy with other calls; the call was not made. Ask again later.","reason":"busy"}})");
    EXPECT_FALSE(busy.ok);
    EXPECT_NE(busy.content.find("busy"), std::string::npos);
    EXPECT_NE(busy.content.find("not made"), std::string::npos);

    const auto upstream = call_output(502, R"({"error":{"code":"UPSTREAM_ERROR","message":"The API answered 500.","status":500,"text":"boom"}})");
    EXPECT_FALSE(upstream.ok);
    EXPECT_NE(upstream.content.find("500"), std::string::npos);

    EXPECT_FALSE(call_output(200, "not json").ok);
    EXPECT_FALSE(call_output(200, R"({"body":1})").ok);  // no status member: not a call answer
}

// ---- tools in the agent runtime (6.1, 6.2) ------------------------------------------------------------------------------

TEST(ApiConnectorTools, SelectedOperationsAreOfferedWithTheirRiskAndLabel) {
    ManagerFixture f;
    f.set_policy(bundle_with(json::array({orders_connector(), {{"id", "jira"}, {"name", "Jira"}, {"kind", "remote"}, {"url", "https://j.example/mcp"}, {"auth", "none"}}})));
    auto& m = f.start();
    m.apply();
    EXPECT_EQ(f.reg.size(), 2u);
    ASSERT_TRUE(f.reg.has("api:orders-api/getOrder"));
    ASSERT_TRUE(f.reg.has("api:orders-api/createOrder"));
    EXPECT_FALSE(f.reg.has("mcp:jira/anything"));
    const auto get = *f.reg.get("api:orders-api/getOrder");
    ASSERT_TRUE(get.risk.has_value());
    EXPECT_EQ(*get.risk, Risk::read);
    const auto create = *f.reg.get("api:orders-api/createOrder");
    ASSERT_TRUE(create.risk.has_value());
    EXPECT_EQ(*create.risk, Risk::external);
    EXPECT_EQ(get.description.rfind("[Orders, set by its administrator] ", 0), 0u) << get.description;
    EXPECT_EQ(get.provenance.source_kind, "api");
    EXPECT_EQ(get.provenance.source_id, "orders-api/getOrder");
    const json st = m.status();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0]["usable_tools"], 2);
    EXPECT_TRUE(st[0]["reason"].is_null());
    EXPECT_TRUE(st[0]["warning"].is_null());
}

TEST(ApiConnectorTools, InvalidArgumentsAreRefusedBeforeAnyRequest) {
    ManagerFixture f;
    f.cp.answer = [](const test::LoopbackHttpServer::Request&) { return json_reply(200, "OK", {{"status", 200}, {"body", json::object()}, {"truncated", false}}); };
    f.set_policy(bundle_with(json::array({orders_connector()})));
    f.start().apply();
    const auto out = f.call("api:orders-api/getOrder", {{"qty", 1}});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.content.find("id"), std::string::npos) << out.content;
    EXPECT_TRUE(f.cp.requests().empty());
}

TEST(ApiConnectorTools, ACallGoesOnlyToTheControlPlaneWithoutRationale) {
    ManagerFixture f;
    f.cp.answer = [](const test::LoopbackHttpServer::Request&) { return json_reply(200, "OK", {{"status", 200}, {"body", {{"id", "42"}}}, {"truncated", false}}); };
    f.set_policy(bundle_with(json::array({orders_connector()})));
    f.start().apply();
    const auto out = f.call("api:orders-api/getOrder", {{"id", "42"}, {"rationale", "to answer the user"}});
    EXPECT_TRUE(out.ok) << out.content;
    EXPECT_NE(out.content.find("\"id\":\"42\""), std::string::npos);
    ASSERT_TRUE(out.provenance.has_value());
    EXPECT_EQ(out.provenance->source_kind, "api");
    EXPECT_EQ(out.provenance->source_id, "orders-api/getOrder");
    EXPECT_FALSE(out.provenance->trusted);
    const auto reqs = f.cp.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].method, "POST");
    EXPECT_EQ(reqs[0].path, "/api/v1/connectors/orders-api/operations/getOrder/call");
    const json body = parse_json(reqs[0].body).value_or(json());
    EXPECT_EQ(body, json({{"arguments", {{"id", "42"}}}}));
}

TEST(ApiConnectorTools, ABusyAnswerIsReportedAndNotRetried) {
    ManagerFixture f;
    f.cp.answer = [](const test::LoopbackHttpServer::Request&) {
        return json_reply(429, "Too Many Requests",
                          {{"error", {{"code", "BUSY"}, {"message", "The connector is busy with other calls; the call was not made. Ask again later."}, {"reason", "busy"}}}});
    };
    f.set_policy(bundle_with(json::array({orders_connector()})));
    f.start().apply();
    const auto out = f.call("api:orders-api/getOrder", {{"id", "42"}});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.content.find("busy"), std::string::npos);
    EXPECT_EQ(f.cp.requests().size(), 1u);
}

TEST(ApiConnectorTools, AnUnreachableControlPlaneIsAFailureWithoutRetry) {
    Registry reg;
    std::shared_ptr<const policy::EffectivePolicy> current;
    {
        policy::PolicyBundle b;
        b.ver = 1;
        b.exp = 4102444800;
        b.policy = bundle_with(json::array({orders_connector()}));
        current = std::make_shared<policy::EffectivePolicy>(b);
    }
    int calls = 0;
    ManagerEnv env;
    env.policy = [&] { return current; };
    env.request = [&](const std::string&, const std::string&, const std::string&) -> Result<HttpResponse> {
        ++calls;
        return make_error("connect", "refused");
    };
    env.add_tool = [&](ToolDef d) { return reg.add(std::move(d)); };
    env.remove_tool = [&](const std::string& id) { return reg.remove(id); };
    ConnectorManager m(std::move(env));
    m.apply();
    const auto d = reg.get("api:orders-api/getOrder");
    ASSERT_TRUE(d.has_value());
    ToolContext ctx;
    const auto out = d->run(ctx, json{{"id", "42"}});
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.content.find("not be reached"), std::string::npos) << out.content;
    EXPECT_EQ(calls, 1);
}

TEST(ApiConnectorTools, PolicyDenyAndRemovalWithdrawTheTools) {
    ManagerFixture f;
    f.set_policy(bundle_with(json::array({orders_connector()}), {{"api:orders-api/createOrder", "deny"}}));
    auto& m = f.start();
    m.apply();
    EXPECT_TRUE(f.reg.has("api:orders-api/getOrder"));
    EXPECT_FALSE(f.reg.has("api:orders-api/createOrder"));
    EXPECT_EQ(m.status()[0]["usable_tools"], 1);

    // The policy no longer has the connector: its tools are withdrawn before apply returns.
    f.set_policy(bundle_with(json::array()));
    m.apply();
    EXPECT_EQ(f.reg.size(), 0u);
    EXPECT_EQ(m.status().size(), 0u);
}

TEST(ApiConnectorTools, AConnectorWithoutATokenOffersNoTools) {
    ManagerFixture f;
    f.set_policy(bundle_with(json::array({orders_connector("needs-token", json::array())})));
    auto& m = f.start();
    m.apply();
    EXPECT_EQ(f.reg.size(), 0u);
    const json st = m.status();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0]["state"], std::string("needs-token"));
    EXPECT_EQ(st[0]["reason"], std::string("needs-token"));
    EXPECT_EQ(st[0]["usable_tools"], 0);
    EXPECT_EQ(st[0]["warning"], std::string("no usable tools"));
}

TEST(ApiConnectorTools, AConnectorWithNoSelectedOperationSaysSo) {
    ManagerFixture f;
    f.set_policy(bundle_with(json::array({orders_connector("ready", json::array())})));
    auto& m = f.start();
    m.apply();
    EXPECT_EQ(m.status()[0]["reason"], std::string("none-selected"));
}

// ---- personal access tokens (6.4) ---------------------------------------------------------------------------------------

TEST(ApiConnectorTokens, TheTokenIsSentOnceAndNeverKeptOrReturned) {
    const fs::path log_dir = fs::temp_directory_path() / "wb-api-connectors-log-test";
    fs::remove_all(log_dir);
    log::init(log::Options{log_dir, 1024 * 1024, 256 * 1024, false});

    FakeControlPlane cp;
    cp.answer = [](const test::LoopbackHttpServer::Request& r) {
        if (r.method == "PUT")
            return json_reply(200, "OK", {{"id", "orders-api"}, {"state", "ready"}, {"token", {{"set", true}, {"set_at", "2026-11-10T10:00:00.000Z"}}}});
        if (r.method == "DELETE") return json_reply(200, "OK", {{"id", "orders-api"}, {"state", "needs-token"}, {"token", nullptr}});
        return json_reply(200, "OK",
                          {{"connectors", json::array({{{"id", "orders-api"}, {"name", "Orders"}, {"kind", "api"}, {"auth", "user-pat"}, {"state", "ready"},
                                                       {"offered_tools", 2}, {"help_text", "Create a token in your Jira settings."},
                                                       {"token", {{"set", true}, {"set_at", "2026-11-10T10:00:00.000Z"}}}}})}});
    };
    const Request request = cp.request();

    // The text goes in the body of one PUT; the answer does not echo it.
    const auto set = set_token(request, "orders-api", kToken);
    ASSERT_TRUE(set.ok()) << set.error().message;
    EXPECT_EQ(set->dump().find(kToken), std::string::npos);
    EXPECT_EQ((*set)["token"]["set"], true);
    const auto reqs = cp.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].method, "PUT");
    EXPECT_EQ(reqs[0].path, "/api/v1/connectors/orders-api/token");
    EXPECT_EQ(parse_json(reqs[0].body).value_or(json())["token"], std::string(kToken));

    // The listing carries whether a token is set and when, never the token itself.
    const auto list = list_connectors(request);
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list->dump().find(kToken), std::string::npos);
    EXPECT_EQ((*list)["connectors"][0]["token"]["set"], true);

    // The tools of the connector carry no token either.
    ManagerFixture f;
    f.set_policy(bundle_with(json::array({orders_connector()})));
    f.start().apply();
    EXPECT_EQ(f.mgr->status().dump().find(kToken), std::string::npos);
    EXPECT_EQ(f.reg.get("api:orders-api/getOrder")->description.find(kToken), std::string::npos);

    const auto removed = remove_token(request, "orders-api");
    ASSERT_TRUE(removed.ok());
    EXPECT_TRUE((*removed)["token"].is_null());
    EXPECT_EQ(cp.requests().back().method, "DELETE");

    log::shutdown();
    for (const auto& e : fs::recursive_directory_iterator(log_dir))
        if (e.is_regular_file()) EXPECT_EQ(read_all(e.path()).find(kToken), std::string::npos) << e.path();
    fs::remove_all(log_dir);
}

TEST(ApiConnectorTokens, BadIdsAndEmptyTokensAreRefusedWithoutARequest) {
    FakeControlPlane cp;
    const Request request = cp.request();
    EXPECT_FALSE(set_token(request, "../orders", kToken).ok());
    EXPECT_FALSE(set_token(request, "orders-api", "").ok());
    EXPECT_FALSE(remove_token(request, "..").ok());
    EXPECT_FALSE(set_token(Request{}, "orders-api", kToken).ok());
    EXPECT_TRUE(cp.requests().empty());
}

TEST(ApiConnectorTokens, AControlPlaneRefusalIsReturnedAsItsCode) {
    FakeControlPlane cp;
    cp.answer = [](const test::LoopbackHttpServer::Request&) {
        return json_reply(400, "Bad Request", {{"error", {{"code", "VALIDATION_FAILED"}, {"message", "A token is at most 4096 characters."}, {"field", "token"}}}});
    };
    const auto r = set_token(cp.request(), "orders-api", kToken);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "VALIDATION_FAILED");
    EXPECT_EQ(r.error().message.find(kToken), std::string::npos);
}

TEST(ApiConnectorTokens, TheDraftIsClearedWhenTaken) {
    TokenDraft d;
    d.set(u"typed-token");
    EXPECT_EQ(d.text(), u"typed-token");
    EXPECT_EQ(d.take(), u"typed-token");
    EXPECT_TRUE(d.text().empty());
    EXPECT_TRUE(d.take().empty());
}
