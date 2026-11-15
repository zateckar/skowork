// Opt-in integration test: a local control plane forwards synthetic prompts to the configured organization model.
// The gateway token is a short-lived test token; the only mail Tool is a local mock.
#include "model/model_client.h"

namespace {
class LiveGatewayAuth final : public model::IGatewayAuth {
public:
    explicit LiveGatewayAuth(std::string token) : token_(std::move(token)) {}
    Result<std::string> access_token() override { return token_; }
    Result<std::string> refresh_after_unauthorized(const std::string&) override { return token_; }
    void session_rejected(const std::string&) override {}
private:
    std::string token_;
};

const json* live_component(const json& components, const std::string& type) {
    if (!components.is_array()) return nullptr;
    for (const auto& component : components) {
        if (jstr(component, "type") == type) return &component;
        if ((jstr(component, "type") == "section" || jstr(component, "type") == "card") && component.contains("components"))
            if (const auto* found = live_component(component["components"], type)) return found;
    }
    return nullptr;
}
}  // namespace

TEST(ShellLiveGeneratedSurface, RealGatewayInteraction) {
#ifndef SKOWORK_DEV_ENDPOINTS
    GTEST_SKIP() << "the isolated loopback gateway requires a development build";
#endif
    const char* gateway = std::getenv("SKOWORK_TEST_GATEWAY");
    const char* token = std::getenv("SKOWORK_TEST_GATEWAY_TOKEN");
    if (!gateway || !token) GTEST_SKIP() << "run gateway-acceptance.mjs to configure the isolated gateway";
    const std::string base = gateway;
    ASSERT_TRUE(base.starts_with("http://127.0.0.1:"));
    LiveGatewayAuth auth(token);
    model::WinHttpTransport transport;
    model::ModelClientConfig mc;
    mc.gateway_url = [base] { return base; };
    model::ModelClient model(mc, auth, transport);
    RuntimeConfig cfg;
    cfg.generated_surfaces = true;
    Env env(cfg);
    env.rt.reset();
    env.policy->approvals.insert("mail.send");
    auto created = AgentService::create(*env.db, model, cfg, env.policy);
    ASSERT_TRUE(created);
    auto agent = std::move(*created);
    ASSERT_TRUE(agent->runtime().add_tool(mail_tool(env)));
    ASSERT_TRUE(agent->start());

    std::atomic<ShellThread*> window{nullptr};
    InboxEnv ie;
    ie.runtime = &agent->runtime();
    ie.clock = &env.clock;
    ie.emit = [&](const std::string& name, const json& data) {
        if (auto* w = window.load()) w->post([w, name, data] { w->main().emit(name, data); });
    };
    InboxService inbox(*env.db, std::move(ie));
    RpcRegistry rpc;
    agent->register_rpc(rpc, false);
    inbox.register_rpc(rpc, false);
    rpc.add("app.status", RpcExposure::ui, [](const json&) -> Result<json> {
        return json{{"auth", {{"state", "signed_in"}}}, {"control_plane", {{"configured", true}, {"connected", true}}},
                    {"background", {{"paused_until", 0}}}, {"storage", {{"read_only", false}, {"degraded", false}}}};
    });
    ShellThread w(rpc, 1300, [] { return shell::now_millis(); });
    window = &w;
    agent->set_event_sink([&](const json& event) {
        inbox.on_event(event);
        if (auto* target = window.load()) target->post([target, event] { target->main().emit("run.event", event); });
    });
    struct Stop {
        AgentService& agent;
        InboxService& inbox;
        std::atomic<ShellThread*>& window;
        ~Stop() { agent.set_event_sink({}); agent.shutdown(); inbox.wait_idle(); window = nullptr; }
    } stop{*agent, inbox, window};

    const auto thread = agent->runtime().create_thread("Synthetic visual acceptance");
    ASSERT_TRUE(thread);
    w.run([&] { w.main().open("#/threads/" + *thread); });
    const auto first = agent->runtime().start_run(*thread,
        "Compare Atlas at 12 hours and Boreal at 18 hours. Include a sortable table and one button labeled Select Atlas "
        "in a valid skowork-ui surface. Do not call tools yet.");
    ASSERT_TRUE(first);
    const bool first_completed = agent->runtime().wait_for_state(*first, RunState::succeeded, 90s);
    if (!first_completed) {
        const auto record = agent->runtime().run(*first);
        if (record) std::printf("[gateway-acceptance] first Run state=%s reason=%s\n", to_string(record->state), record->reason.c_str());
        for (const auto& event : agent->runtime().events(*first))
            if (event.kind == "error") std::printf("[gateway-acceptance] error code=%s message=%s\n",
                                                    jstr(event.payload, "code").c_str(), jstr(event.payload, "message").c_str());
    }
    ASSERT_TRUE(first_completed);
    json surface;
    for (const auto& event : agent->runtime().events(*first)) {
        if (event.kind != "message" || !jbool(event.payload, "surface_complete")) continue;
        const auto blocks = ui::discover_generated_surfaces(jstr(event.payload, "content"));
        if (!blocks.empty()) surface = blocks.front().surface;
    }
    ASSERT_TRUE(surface.is_object()) << "the real model did not produce a valid surface";
    const auto* table = live_component(surface["components"], "table");
    const auto* button = live_component(surface["components"], "button");
    ASSERT_NE(table, nullptr);
    ASSERT_NE(button, nullptr);
    const auto displayed = table->dump();
    EXPECT_NE(displayed.find("Atlas"), std::string::npos);
    EXPECT_NE(displayed.find("Boreal"), std::string::npos);
    EXPECT_NE(displayed.find("12"), std::string::npos);
    EXPECT_NE(displayed.find("18"), std::string::npos);
    const auto table_id = jstr(*table, "id"), button_id = jstr(*button, "id");
    ASSERT_TRUE(w.until([&] { return by_id(w.main().window()->tree().root(), table_id + "-sort-0") != nullptr; }, 15000));
    const int before_sort = model.requests_sent();
    ASSERT_TRUE(w.invoke(table_id + "-sort-0"));
    EXPECT_EQ(model.requests_sent(), before_sort);
    ASSERT_TRUE(w.invoke(button_id + "-review"));
    ASSERT_TRUE(w.invoke(button_id + "-confirm"));
    ASSERT_TRUE(env.wait_for([&] { return agent->runtime().runs(*thread).size() >= 2u; }, 30s));
    const auto followup = agent->runtime().runs(*thread).back().id;
    ASSERT_TRUE(agent->runtime().wait_for_state(followup, RunState::succeeded, 90s));

    const auto question_run = agent->runtime().start_run(*thread,
        "Use the ask_user tool now with a structured form: a select labeled Frequency with options Weekly and Monthly, "
        "and exactly one Continue button. Wait for my answer before proceeding.");
    ASSERT_TRUE(question_run);
    ASSERT_TRUE(agent->runtime().wait_for_state(*question_run, RunState::interrupted, 90s));
    const auto questions = agent->runtime().pending_actions(*thread);
    ASSERT_EQ(questions.size(), 1u);
    ASSERT_EQ(questions[0].kind, "question");
    const auto* form = jobj(questions[0].payload, "surface");
    ASSERT_NE(form, nullptr) << "the real model did not request a structured Question";
    const auto* select = live_component((*form)["components"], "select");
    const auto* submit = live_component((*form)["components"], "button");
    ASSERT_NE(select, nullptr);
    ASSERT_NE(submit, nullptr);
    const auto select_id = jstr(*select, "id"), submit_id = jstr(*submit, "id");
    ASSERT_TRUE(w.until([&] { return by_id(w.main().window()->tree().root(), submit_id + "-review") != nullptr; }, 15000));
    ASSERT_TRUE(w.with(select_id + "-option-0", [](ui::Node& node) {
        auto* checkbox = dynamic_cast<ui::Checkbox*>(&node);
        if (!checkbox) return false;
        checkbox->set_checked(true);
        return true;
    }));
    ASSERT_TRUE(w.invoke(submit_id + "-review"));
    ASSERT_TRUE(w.invoke(submit_id + "-confirm"));
    ASSERT_TRUE(agent->runtime().wait_for_state(*question_run, RunState::succeeded, 90s));

    const auto mail_run = agent->runtime().start_run(*thread,
        "Use find_tools to find mail.send, then use mail.send to send the synthetic message 'Hello' to test@example.test. "
        "This is a mock tool used for acceptance testing.");
    ASSERT_TRUE(mail_run);
    ASSERT_TRUE(agent->runtime().wait_for_state(*mail_run, RunState::interrupted, 90s));
    const auto approvals = agent->runtime().pending_actions(*thread);
    ASSERT_EQ(approvals.size(), 1u);
    EXPECT_EQ(approvals[0].kind, "approval");
    ASSERT_TRUE(w.until([&] { return by_id(w.main().window()->tree().root(), "approval-" + approvals[0].id + "-title") != nullptr; }, 15000));
    EXPECT_EQ(env.calls.count("mail.send"), 0);
    ASSERT_TRUE(agent->runtime().resolve_approval(approvals[0].id, ApprovalResolution::reject()));
    ASSERT_TRUE(agent->runtime().wait_for_state(*mail_run, RunState::succeeded, 90s));
    EXPECT_EQ(env.calls.count("mail.send"), 0);
    std::printf("[gateway-acceptance] native table, local sorting, reviewed follow-up, structured Question and policy approval observed\n");
}
