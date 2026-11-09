// The scripted agent-runtime harness shared by the runtime and inbox tests: a fake clock, a scripted model, a test
// policy handler and a runtime on a real encrypted app.db. Each test file gets its own copy (anonymous namespace).
#pragma once

#include "platform/crypto.h"
#include "platform/win.h"
#include "app/agent_service.h"
#include "policy/enforcer.h"
#include "runtime/context.h"
#include "runtime/run_context.h"
#include "runtime/runtime.h"
#include "storage/app_store.h"
#include "storage/data_root.h"
#include "storage/database.h"
#include "storage/keystore.h"
#include "support/junction.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <deque>
#include <fstream>
#include <set>
#include <thread>
#include <tuple>

using namespace wb;
using namespace wb::runtime;
using namespace std::chrono_literals;

namespace {

// ---- Helpers --------------------------------------------------------------------------------------------------------

class FakeClock final : public Clock {
public:
    [[nodiscard]] sys_clock::time_point now() const override {
        return sys_clock::time_point(std::chrono::duration_cast<sys_clock::duration>(std::chrono::milliseconds(base_ + ms_.load())));
    }
    void advance(std::chrono::milliseconds d) { ms_ += d.count(); }

private:
    static constexpr long long base_ = 1'790'000'000'000LL;
    std::atomic<long long> ms_{0};
};

using Step = std::function<model::ModelResult<model::ChatResponse>(const model::ChatRequest&, const model::ChatSink&)>;

struct CallSpec {
    std::string id, tool;
    json args = json::object();
};

model::ChatResponse text_response(const std::string& text) {
    model::ChatResponse r;
    r.text = text;
    r.finish_reason = "stop";
    r.usage.requests = 1;
    r.usage.prompt = 10;
    r.usage.completion = 5;
    return r;
}

model::ChatResponse calls_response(const std::vector<CallSpec>& calls, const std::string& text = {}) {
    model::ChatResponse r;
    r.text = text;
    r.finish_reason = "tool_calls";
    r.usage.requests = 1;
    r.usage.prompt = 10;
    r.usage.completion = 5;
    for (const auto& c : calls) {
        model::ToolCall t;
        t.id = c.id;
        t.tool = c.tool;
        t.arguments = c.args;
        t.raw_arguments = c.args.dump();
        t.valid = true;
        r.tool_calls.push_back(std::move(t));
    }
    return r;
}

model::ModelFailure failure(const std::string& code, const std::string& reason = {}) {
    model::ModelFailure f;
    f.code = code;
    f.message = code;
    f.reason = reason;
    return f;
}

struct Gate {
    std::atomic<bool> open{false};
    void release() { open = true; }
    bool wait(std::chrono::milliseconds max = 10s) const {
        const auto end = std::chrono::steady_clock::now() + max;
        while (!open && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
        return open;
    }
};

// Answers requests it recognises (summaries, subagents) outside the script; returns nothing for the others.
using Responder = std::function<std::optional<model::ModelResult<model::ChatResponse>>(const model::ChatRequest&)>;

class ScriptedModel final : public model::IChatModel {
public:
    model::ModelResult<model::ChatResponse> chat(const model::ChatRequest& req, const model::ChatSink& sink) override {
        Responder responder;
        {
            const std::lock_guard lock(mx_);
            responder = responder_;
        }
        if (responder) {
            if (auto r = responder(req)) {
                const std::lock_guard lock(mx_);
                side_requests_.push_back(req);
                return std::move(*r);
            }
        }
        Step step;
        {
            const std::lock_guard lock(mx_);
            requests_.push_back(req);
            if (steps_.empty()) return failure(model::code::model_unavailable, "no scripted response");
            step = std::move(steps_.front());
            steps_.pop_front();
        }
        return step(req, sink);
    }
    [[nodiscard]] std::optional<model::RoleInfo> role_info(model::Role role) override {
        const std::lock_guard lock(mx_);
        const auto it = roles_.find(role);
        return it == roles_.end() ? std::nullopt : std::optional<model::RoleInfo>(it->second);
    }

    void set_role_info(model::RoleInfo info) {
        const std::lock_guard lock(mx_);
        roles_[info.role] = std::move(info);
    }

    void push(Step s) {
        const std::lock_guard lock(mx_);
        steps_.push_back(std::move(s));
    }
    void push_response(model::ChatResponse r) {
        push([r = std::move(r)](const model::ChatRequest&, const model::ChatSink&) { return model::ModelResult<model::ChatResponse>(r); });
    }
    void push_text(const std::string& t) { push_response(text_response(t)); }
    void push_calls(const std::vector<CallSpec>& c) { push_response(calls_response(c)); }
    void push_failure(model::ModelFailure f) {
        push([f = std::move(f)](const model::ChatRequest&, const model::ChatSink&) { return model::ModelResult<model::ChatResponse>(f); });
    }
    // Blocks until the Run is cancelled or the gate opens, then answers with `after`.
    void push_blocking(Gate& gate, model::ChatResponse after = text_response("done")) {
        push([&gate, after = std::move(after)](const model::ChatRequest& req, const model::ChatSink&) {
            const auto end = std::chrono::steady_clock::now() + 20s;
            while (!gate.open && !(req.cancel && req.cancel->cancelled()) && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(2ms);
            if (req.cancel && req.cancel->cancelled()) return model::ModelResult<model::ChatResponse>(failure(model::code::cancelled));
            return model::ModelResult<model::ChatResponse>(after);
        });
    }

    [[nodiscard]] std::vector<model::ChatRequest> requests() {
        const std::lock_guard lock(mx_);
        return requests_;
    }
    [[nodiscard]] size_t request_count() {
        const std::lock_guard lock(mx_);
        return requests_.size();
    }
    void set_responder(Responder r) {
        const std::lock_guard lock(mx_);
        responder_ = std::move(r);
    }
    [[nodiscard]] std::vector<model::ChatRequest> side_requests() {
        const std::lock_guard lock(mx_);
        return side_requests_;
    }

private:
    std::mutex mx_;
    std::deque<Step> steps_;
    std::vector<model::ChatRequest> requests_;
    Responder responder_;
    std::vector<model::ChatRequest> side_requests_;
    std::map<model::Role, model::RoleInfo> roles_;
};

class TestPolicy final : public PolicyHook {
public:
    ToolDecision before_tool_call(const ToolCallInfo& c) override {
        {
            const std::lock_guard lock(mx);
            seen.push_back(c);
        }
        if (decider) return decider(c);
        if (denied.count(c.tool)) return ToolDecision::deny("denied by policy");
        if (approvals.count(c.tool)) return ToolDecision::approval("needs approval");
        return ToolDecision::allow();
    }
    [[nodiscard]] bool tool_permitted(const std::string& id, Risk) const override { return !denied.count(id); }
    [[nodiscard]] bool may_require_approval(const std::string& id, Risk) const override { return approvals.count(id) > 0; }

    std::function<ToolDecision(const ToolCallInfo&)> decider;
    std::set<std::string> denied, approvals;
    std::mutex mx;
    std::vector<ToolCallInfo> seen;
};

class Collector {
public:
    void operator()(const RunEvent& e) {
        const std::lock_guard lock(mx_);
        events_.push_back(e);
    }
    [[nodiscard]] std::vector<RunEvent> all() {
        const std::lock_guard lock(mx_);
        return events_;
    }
    [[nodiscard]] std::vector<RunEvent> of(const std::string& kind) {
        std::vector<RunEvent> out;
        for (auto& e : all())
            if (e.kind == kind) out.push_back(std::move(e));
        return out;
    }
    [[nodiscard]] std::vector<std::string> states(const std::string& run_id) {
        std::vector<std::string> out;
        for (const auto& e : of("run.state"))
            if (e.run_id == run_id) out.push_back(jstr(e.data, "state"));
        return out;
    }

private:
    std::mutex mx_;
    std::vector<RunEvent> events_;
};

// Which tools ran, with which arguments.
struct Calls {
    std::mutex mx;
    std::vector<std::pair<std::string, json>> list;
    void add(const std::string& id, const json& args) {
        const std::lock_guard lock(mx);
        list.emplace_back(id, args);
    }
    [[nodiscard]] int count(const std::string& id) {
        const std::lock_guard lock(mx);
        int n = 0;
        for (const auto& c : list)
            if (c.first == id) ++n;
        return n;
    }
    [[nodiscard]] std::vector<json> args_of(const std::string& id) {
        const std::lock_guard lock(mx);
        std::vector<json> out;
        for (const auto& c : list)
            if (c.first == id) out.push_back(c.second);
        return out;
    }
};

struct TempRoot {
    std::filesystem::path path = std::filesystem::path(local_appdata()) / L"workbench-test" / widen(random_urlsafe(6));
    ~TempRoot() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A runtime on a real encrypted app.db with scripted model and policy. restart() reopens the database like a new process.
struct Env {
    explicit Env(RuntimeConfig c = {}) : cfg(std::move(c)) {
        auto root = DataRoot::open(tmp.path);
        EXPECT_TRUE(root);
        KeyStore keys(root->keys());
        EXPECT_TRUE(keys.load());
        auto k = keys.get_or_create("app.db");
        EXPECT_TRUE(k);
        key = std::make_unique<db_key>(*k);
        file = root->file("app.db");
        policy = std::make_shared<TestPolicy>();
        make_runtime();
    }
    ~Env() { rt.reset(); }

    void make_runtime() {
        rt.reset();
        db.reset();
        MigrationOutcome o;
        auto opened = open_and_migrate(file, *key, app_migrations(), o);
        EXPECT_TRUE(opened);
        db = std::move(*opened);
        HookRegistry hooks(policy_hook ? policy_hook : std::shared_ptr<PolicyHook>(policy));
        for (const auto& h : extra_hooks) hooks.add(h);
        auto r = AgentRuntime::create(cfg, *db, model, clock, std::move(hooks));
        EXPECT_TRUE(r);
        rt = std::move(*r);
        rt->set_listener([this](const RunEvent& e) { events(e); });
        for (const auto& t : tool_defs) EXPECT_TRUE(rt->add_tool(t));
    }
    void add_tool(ToolDef t) {
        tool_defs.push_back(t);
        EXPECT_TRUE(rt->add_tool(std::move(t)));
    }
    // Simulates a restart of the application.
    void restart() {
        rt->shutdown();
        make_runtime();
        auto r = rt->start();
        EXPECT_TRUE(r);
        if (r) report = *r;
    }
    void start() {
        auto r = rt->start();
        ASSERT_TRUE(r);
        report = *r;
    }
    std::string thread() {
        auto t = rt->create_thread("t");
        EXPECT_TRUE(t);
        return *t;
    }
    std::string run(const std::string& thread_id, const std::string& text, bool unattended = false) {
        StartOptions o;
        o.unattended = unattended;
        auto r = rt->start_run(thread_id, text, o);
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? *r : std::string();
    }
    bool wait(const std::string& run_id, RunState s, std::chrono::milliseconds t = 10s) { return rt->wait_for_state(run_id, s, t); }
    RunRecord rec(const std::string& run_id) { return *rt->run(run_id); }
    std::string tool_content(const std::string& run_id, const std::string& call_id) {
        for (const auto& e : rt->events(run_id))
            if (e.kind == "tool_result" && jstr(e.payload, "call_id") == call_id) return jstr(e.payload, "content");
        return "<no result>";
    }
    bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds t = 10s) {
        const auto end = std::chrono::steady_clock::now() + t;
        while (!pred() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
        return pred();
    }

    RuntimeConfig cfg;
    TempRoot tmp;
    std::unique_ptr<db_key> key;
    std::filesystem::path file;
    std::unique_ptr<Database> db;
    FakeClock clock;
    ScriptedModel model;
    std::shared_ptr<TestPolicy> policy;
    std::shared_ptr<PolicyHook> policy_hook;  // replaces `policy` when set (before make_runtime)
    std::vector<std::shared_ptr<Hook>> extra_hooks;  // registered after the policy handler by make_runtime()
    Collector events;
    Calls calls;
    std::vector<ToolDef> tool_defs;
    std::unique_ptr<AgentRuntime> rt;
    RecoveryReport report;
};

ToolDef make_tool(Env& env, const std::string& id, std::optional<Risk> risk, const std::string& description = "A test tool") {
    ToolDef t;
    t.id = id;
    t.description = description;
    t.risk = risk;
    t.run = [&env, id](ToolContext&, const json& args) {
        env.calls.add(id, args);
        return ToolOutput::success("ran " + id);
    };
    return t;
}

ToolDef mail_tool(Env& env) {
    ToolDef t = make_tool(env, "mail.send", Risk::external, "Send an e-mail");
    t.parameters = json::parse(R"({"type":"object","properties":{"to":{"type":"string"},"body":{"type":"string"}},"required":["to"],"additionalProperties":false})");
    return t;
}

json user_message_event(const std::string& text) {
    model::ChatMessage m;
    m.role = "user";
    m.content = text;
    return message_to_event(m);
}

}  // namespace
