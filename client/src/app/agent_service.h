// The agent runtime in the core process (add-agent-runtime design D11): the runtime on app.db, the interim policy
// handler, and the Local Run API (client/agent-runtime) on the host RPC registry.
#pragma once

#include "host/rpc.h"
#include "runtime/runtime.h"

#include <functional>
#include <memory>

namespace wb {

// A fixed policy handler for tests and tools that run without the app (the eval runner): read and write_local calls
// run, external and destructive calls need approval, every registered tool is permitted. The app uses
// policy::PolicyEnforcer (client/policy-enforcement).
class InterimPolicy final : public runtime::PolicyHook {
public:
    runtime::ToolDecision before_tool_call(const runtime::ToolCallInfo& call) override;
    [[nodiscard]] bool tool_permitted(const std::string& tool_id, runtime::Risk declared_risk) const override;
    [[nodiscard]] bool may_require_approval(const std::string& tool_id, runtime::Risk declared_risk) const override;
};

class AgentService {
public:
    // `policy` null: InterimPolicy. `hooks` run after the policy handler.
    static Result<std::unique_ptr<AgentService>> create(Database& db, model::IChatModel& model, runtime::RuntimeConfig config,
                                                        std::shared_ptr<runtime::PolicyHook> policy = nullptr,
                                                        std::vector<std::shared_ptr<runtime::Hook>> hooks = {});

    // Receives every Run event as JSON ({kind, run_id, thread_id, seq, at, data}); called on runtime threads.
    void set_event_sink(std::function<void(const json&)> sink);
    // Told about every Approval the user resolves through the API (client/policy-enforcement Decision Audit).
    void set_resolution_listener(std::function<void(const runtime::ActionItem&, const std::string& decision)> fn) {
        on_resolution_ = std::move(fn);
    }

    Status start();
    void shutdown();

    // Registers the Local Run API. `expose_on_pipe`: development builds only (design D11); release builds keep every
    // method UI-only.
    void register_rpc(RpcRegistry& rpc, bool expose_on_pipe);

    runtime::AgentRuntime& runtime() { return *rt_; }
    [[nodiscard]] size_t executing_runs() { return rt_->executing_runs(); }

private:
    explicit AgentService(std::unique_ptr<runtime::AgentRuntime> rt) : rt_(std::move(rt)) {}
    std::unique_ptr<runtime::AgentRuntime> rt_;
    std::mutex sink_mx_;
    std::function<void(const json&)> sink_;
    std::function<void(const runtime::ActionItem&, const std::string&)> on_resolution_;
};

// JSON views of runtime records for the API.
json to_json(const runtime::RunRecord& r);
json to_json(const runtime::ActionItem& a);
json to_json(const model::ChatMessage& m);

}  // namespace wb
