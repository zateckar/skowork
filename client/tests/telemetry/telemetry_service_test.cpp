// client/telemetry service (add-telemetry-and-admin tasks 3.1, 3.4): the level from policy, the encrypted offline buffer
// with its cap and discard counter, ordered flush with acknowledgement, retry-after, and the viewer's data.
#include "support/runtime_harness.h"
#include "policy/enforcer.h"
#include "policy/store.h"
#include "storage/app_store.h"
#include "telemetry/collector.h"
#include "telemetry/service.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::telemetry;

namespace {

std::shared_ptr<const policy::EffectivePolicy> policy_with(json telemetry, long long version = 1, const std::string& group = "pilot") {
    policy::PolicyBundle b;
    b.sub = "user";
    b.ver = version;
    b.exp = 4102444800;
    b.policy = {{"telemetry", std::move(telemetry)}};
    for (const auto& [k, v] : b.policy["telemetry"].items()) b.src["telemetry." + k] = group;
    return std::make_shared<const policy::EffectivePolicy>(b);
}

struct Rig {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / L"skowork-test" / widen("tel-" + random_urlsafe(6));
    db_key key{};
    FakeClock clock;
    std::shared_ptr<const policy::EffectivePolicy> pol;
    bool online = false;
    int status = 200;
    std::string retry_after;
    std::vector<std::string> bodies;
    std::unique_ptr<TelemetryService> svc;

    Rig() {
        std::filesystem::create_directories(dir);
        for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);
        open();
    }
    ~Rig() {
        svc.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void open() {
        svc.reset();
        TelemetryEnv env;
        env.policy = [this] { return pol; };
        env.send = [this](const std::string& body) -> Result<HttpResponse> {
            if (!online) return make_error("network", "offline");
            HttpResponse r;
            r.status = status;
            if (!retry_after.empty()) r.headers["retry-after"] = retry_after;
            if (status >= 200 && status < 300) bodies.push_back(body);
            return r;
        };
        env.installation_id = [] { return std::string("inst-1"); };
        env.version = "0.1.0";
        env.channel = "dev";
        env.os_build = "22631";
        env.clock = &clock;
        auto s = TelemetryService::open(dir / L"telemetry-buffer.db", key, std::move(env));
        ASSERT_TRUE(s) << s.error().message;
        svc = std::move(*s);
    }
    std::vector<std::string> sent_run_ids() const {
        std::vector<std::string> out;
        for (const auto& b : bodies) {
            const auto d = decode_logs(b);
            if (!d) continue;
            for (const auto& l : d->logs)
                for (const auto& [k, v] : l.attrs)
                    if (k == "run.id") out.push_back(std::get<std::string>(v));
        }
        return out;
    }
};

}  // namespace

TEST(TelemetryService, DefaultLevelIsMetadataAndOffEmitsNothing) {
    Rig rig;
    EXPECT_EQ(rig.svc->level(), Level::metadata);
    EXPECT_EQ(rig.svc->level_source(), "default");
    rig.svc->emit(Event::run_finished, {{Attr::run_id, "r1"}, {Attr::content_prompt, "secret plan"}});
    EXPECT_EQ(rig.svc->pending(), 1);
    EXPECT_EQ(rig.svc->view()["records"][0].dump().find("secret plan"), std::string::npos);

    rig.pol = policy_with({{"level", "off"}});
    EXPECT_EQ(rig.svc->level(), Level::off);
    rig.svc->emit(Event::run_finished, {{Attr::run_id, "r2"}});
    EXPECT_EQ(rig.svc->pending(), 1);
    // Nothing is sent at off, not even what was buffered before.
    rig.online = true;
    ASSERT_TRUE(rig.svc->flush());
    EXPECT_TRUE(rig.bodies.empty());
}

TEST(TelemetryService, ContentLevelKeepsContentAndReportsTheIndicatorState) {
    Rig rig;
    rig.pol = policy_with({{"level", "content"}});
    EXPECT_TRUE(rig.svc->content_active());
    EXPECT_EQ(rig.svc->level_source(), "pilot");
    rig.svc->emit(Event::run_finished, {{Attr::run_id, "r1"}, {Attr::content_prompt, "hello"}});
    const json v = rig.svc->view();
    EXPECT_EQ(jstr(v, "level"), "content");
    EXPECT_TRUE(jbool(v, "content_active"));
    EXPECT_EQ(jstr(v["records"][0]["attrs"], "content.prompt"), "hello");
    // The level drops to metadata before the batch goes: the stored content is not sent.
    rig.pol = policy_with({{"level", "metadata"}}, 2);
    rig.online = true;
    ASSERT_TRUE(rig.svc->flush());
    ASSERT_EQ(rig.bodies.size(), 1u);
    EXPECT_EQ(rig.bodies[0].find("hello"), std::string::npos);
    EXPECT_EQ(rig.sent_run_ids(), std::vector<std::string>{"r1"});
}

TEST(TelemetryService, PolicyRedactionPatternsApplyBeforeBuffering) {
    Rig rig;
    rig.pol = policy_with({{"level", "content"}, {"redaction_patterns", {R"(PN-\d{6})"}}});
    rig.svc->emit(Event::client_error, {{Attr::error_code, "HR"}, {Attr::error_message, "PN-123456 not found"}});
    const json v = rig.svc->view();
    EXPECT_EQ(jstr(v["records"][0]["attrs"], "error.message"), "[redacted] not found");
}

TEST(TelemetryService, BufferedOfflineThenSentInOrderAndAcknowledged) {
    Rig rig;
    for (int i = 0; i < 1200; ++i) rig.svc->emit(Event::run_finished, {{Attr::run_id, "r" + std::to_string(i)}});
    EXPECT_FALSE(rig.svc->flush());  // offline
    EXPECT_EQ(rig.svc->pending(), 1200);
    // Survives a restart of the client.
    rig.open();
    EXPECT_EQ(rig.svc->pending(), 1200);
    rig.online = true;
    while (rig.svc->pending() > 0) ASSERT_TRUE(rig.svc->flush());
    ASSERT_EQ(rig.bodies.size(), 3u);  // batches of at most 500
    const auto ids = rig.sent_run_ids();
    ASSERT_EQ(ids.size(), 1200u);
    for (int i = 0; i < 1200; ++i) ASSERT_EQ(ids[static_cast<size_t>(i)], "r" + std::to_string(i));
    const auto d = decode_logs(rig.bodies[0]);
    ASSERT_TRUE(d);
    bool inst = false;
    for (const auto& [k, v] : d->resource) inst |= k == "skowork.installation_id" && std::get<std::string>(v) == "inst-1";
    EXPECT_TRUE(inst);
    // Sent records stay visible in the viewer for 30 days, then go.
    EXPECT_EQ(jstr(rig.svc->view(5)["records"][0], "state"), "sent");
    rig.clock.advance(std::chrono::hours(24 * 31));
    rig.svc->emit(Event::run_finished, {{Attr::run_id, "late"}});
    ASSERT_TRUE(rig.svc->flush());
    EXPECT_EQ(rig.svc->view()["records"].size(), 1u);
}

TEST(TelemetryService, FullBufferDropsTheOldestAndReportsTheCountInTheNextBatch) {
    Rig rig;
    rig.pol = policy_with({{"level", "metadata"}, {"buffer_max_mb", 1}});
    const std::string pad(200, 'x');
    int n = 0;
    while (rig.svc->discarded() == 0 && n < 20000) rig.svc->emit(Event::client_error, {{Attr::error_code, pad + std::to_string(n++)}});
    ASSERT_GT(rig.svc->discarded(), 0);
    const long long dropped = rig.svc->discarded();
    rig.online = true;
    ASSERT_TRUE(rig.svc->flush());
    const auto d = decode_logs(rig.bodies.at(0));
    ASSERT_TRUE(d);
    // The oldest went: the first record sent is not error 0. The count rides in the batch.
    std::string first_code;
    long long reported = -1;
    for (const auto& l : d->logs) {
        for (const auto& [k, v] : l.attrs) {
            if (k == "error.code" && first_code.empty()) first_code = std::get<std::string>(v);
            if (k == "telemetry.discarded") reported = std::get<long long>(v);
        }
    }
    EXPECT_NE(first_code, pad + "0");
    EXPECT_EQ(reported, dropped);
    EXPECT_EQ(rig.svc->discarded(), 0);
}

TEST(TelemetryService, RetryAfterFromTheIngestDelaysTheNextBatch) {
    Rig rig;
    rig.online = true;
    rig.status = 429;
    rig.retry_after = "120";
    rig.svc->emit(Event::run_finished, {{Attr::run_id, "r1"}});
    EXPECT_FALSE(rig.svc->flush());
    rig.status = 200;
    rig.retry_after.clear();
    rig.clock.advance(std::chrono::seconds(61));
    EXPECT_FALSE(rig.svc->flush_due());
    EXPECT_FALSE(rig.svc->flush());
    EXPECT_TRUE(rig.bodies.empty());
    rig.clock.advance(std::chrono::seconds(60));
    EXPECT_TRUE(rig.svc->flush_due());
    ASSERT_TRUE(rig.svc->flush());
    EXPECT_EQ(rig.sent_run_ids(), std::vector<std::string>{"r1"});
}

TEST(TelemetryService, TheBufferIsEncrypted) {
    Rig rig;
    rig.svc->emit(Event::tool_call, {{Attr::tool_id, "plainly.visible.tool"}});
    rig.svc.reset();
    std::ifstream f(rig.dir / L"telemetry-buffer.db", std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(raw.empty());
    EXPECT_EQ(raw.find("plainly.visible.tool"), std::string::npos);
    EXPECT_EQ(raw.find("SQLite format 3"), std::string::npos);
}

// ---- 3.3: policy decision and enforcement records ------------------------------------------------------------------

namespace {

std::vector<json> records_of(const TelemetryService& svc, const std::string& event) {
    std::vector<json> out;
    const json v = svc.view();
    for (const auto& r : v["records"])
        if (jstr(r, "event") == event) out.push_back(r);
    return out;
}

}  // namespace

TEST(TelemetryCollector, ADeniedCallIsRecordedWithRuleAndVersionButNoPath) {
    Rig rig;
    auto key = rig.key;
    MigrationOutcome o;
    auto db = open_and_migrate(rig.dir / L"app.db", key, app_migrations(), o);
    ASSERT_TRUE(db) << db.error().message;
    AppStore store(std::move(*db));
    policy::DecisionLog log(store.db());
    log.set_observer([&rig](const policy::DecisionRecord& r) { record_decision(*rig.svc, r); });

    const std::string secret_dir = (rig.dir / L"Users" / L"jan.novak" / L"HR").string();
    std::filesystem::create_directories(secret_dir);
    auto pol = std::make_shared<const policy::EffectivePolicy>(policy::PolicyBundle{
        "u", "", 12, 0, 4102444800, 1, {"developers"},
        {{"filesystem", {{"deny", {secret_dir + "\\**"}}}}},
        {{"filesystem.deny", "security-team"}}});
    policy::EnforcerEnv env;
    env.policy = [pol] { return pol; };
    env.workspace = (rig.dir / L"ws").string();
    env.data_root = (rig.dir / L"root").string();
    env.grants = [] { return std::vector<policy::FolderGrant>{}; };
    env.log = &log;
    policy::PolicyEnforcer enf(env);

    runtime::ToolCallInfo call;
    call.run.run_id = "run-7";
    call.call_id = "call-1";
    call.tool = "files.read";
    call.risk = runtime::Risk::read;
    call.resources = {{runtime::ToolResource::Kind::read_path, secret_dir + "\\salaries.xlsx"}};
    ASSERT_EQ(enf.before_tool_call(call).kind, runtime::ToolDecision::Kind::deny);

    const auto recs = records_of(*rig.svc, "policy.decision");
    ASSERT_EQ(recs.size(), 1u);
    const json& a = recs[0]["attrs"];
    EXPECT_EQ(jstr(a, "tool.id"), "files.read");
    EXPECT_EQ(jstr(a, "tool.risk"), "read");
    EXPECT_EQ(jstr(a, "policy.rule"), "filesystem.deny");
    EXPECT_EQ(jint(a, "policy.version"), 12);
    EXPECT_EQ(jstr(a, "policy.outcome"), "deny");
    const std::string all = rig.svc->view().dump();
    EXPECT_EQ(all.find("jan.novak"), std::string::npos) << all;
    EXPECT_EQ(all.find("salaries"), std::string::npos) << all;

    // A resolved Approval becomes approval.resolved.
    enf.record_resolution("run-7", "call-2", "files.write", "reject");
    const auto res = records_of(*rig.svc, "approval.resolved");
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(jstr(res[0]["attrs"], "approval.decision"), "reject");
    EXPECT_EQ(rule_id("grant:C:\\Users\\jan.novak\\Docs"), "grant");
}

TEST(TelemetryCollector, RunsAndToolCallsWithoutContentAtMetadataLevel) {
    Rig rig;
    RunCollector col([&rig] { return rig.svc.get(); });
    runtime::RunInfo run;
    run.run_id = "run-1";
    col.run_start(run);
    runtime::ToolCallInfo call;
    call.run = run;
    call.call_id = "c1";
    call.tool = "web.fetch";
    call.risk = runtime::Risk::read;
    call.args = {{"url", "https://wiki.corp.local/hr/salaries?employee=jan"}};
    (void)col.before_tool_call(call);
    col.after_tool_call(call, runtime::ToolResultInfo{true, "page text with secrets", {}});
    model::ChatRequest req;
    req.role = model::Role::small;
    req.messages.push_back({"user", "summarise my salary"});
    model::ChatResponse resp;
    resp.text = "Your salary is high.";
    resp.usage.prompt = 100;
    resp.usage.completion = 7;
    runtime::ModelCallInfo mc;
    mc.run = run;
    mc.request = &req;
    mc.response = &resp;
    col.after_model_call(mc);
    col.run_end(run, runtime::RunState::failed, "timeout: the model did not answer");

    const auto tools = records_of(*rig.svc, "tool.call");
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(jstr(tools[0]["attrs"], "url.domain"), "wiki.corp.local");
    EXPECT_EQ(jstr(tools[0]["attrs"], "tool.outcome"), "ok");
    const auto usage = records_of(*rig.svc, "model.usage");
    ASSERT_EQ(usage.size(), 1u);
    EXPECT_EQ(jstr(usage[0]["attrs"], "model.role"), "small");
    EXPECT_EQ(jint(usage[0]["attrs"], "tokens.prompt"), 100);
    const auto runs = records_of(*rig.svc, "run.finished");
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(jstr(runs[0]["attrs"], "run.outcome"), "failed");
    EXPECT_EQ(jstr(runs[0]["attrs"], "error.code"), "timeout");
    EXPECT_EQ(jint(runs[0]["attrs"], "run.tool_calls"), 1);
    EXPECT_EQ(jstr(runs[0]["attrs"], "run.origin"), "user");
    const std::string all = rig.svc->view().dump();
    for (const char* s : {"salar", "employee", "secrets", "high"}) EXPECT_EQ(all.find(s), std::string::npos) << s;

    // At content level the prompt, the answer and the arguments are included.
    rig.pol = policy_with({{"level", "content"}});
    run.run_id = "run-2";
    col.run_start(run);
    mc.run = run;
    col.after_model_call(mc);
    col.run_end(run, runtime::RunState::succeeded, "");
    const auto runs2 = records_of(*rig.svc, "run.finished");
    ASSERT_EQ(runs2.size(), 2u);
    EXPECT_EQ(jstr(runs2[0]["attrs"], "content.prompt"), "summarise my salary");
    EXPECT_EQ(jstr(runs2[0]["attrs"], "content.response"), "Your salary is high.");
}