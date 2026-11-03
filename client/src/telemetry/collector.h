// What client/telemetry records about Runs, tool calls, model calls, policy decisions and Approvals: a runtime hook and a
// decision-log observer that turn those into catalogue events (client/telemetry Metadata Level Contents, Enforcement
// And Decision Metadata, Content Level).
#pragma once

#include "policy/store.h"
#include "runtime/hooks.h"
#include "telemetry/service.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

namespace wb::telemetry {

class RunCollector final : public runtime::Hook {
public:
    explicit RunCollector(std::function<TelemetryService*()> service) : service_(std::move(service)) {}

    void run_start(const runtime::RunInfo& run) override;
    void after_model_call(const runtime::ModelCallInfo& call) override;
    runtime::ToolDecision before_tool_call(const runtime::ToolCallInfo& call) override;
    void after_tool_call(const runtime::ToolCallInfo& call, const runtime::ToolResultInfo& result) override;
    void run_end(const runtime::RunInfo& run, runtime::RunState state, const std::string& reason) override;

private:
    struct RunState {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        int tool_calls = 0;
        std::string prompt, response;  // content level only
    };
    std::function<TelemetryService*()> service_;
    std::mutex mx_;
    std::map<std::string, RunState> runs_;
    std::map<std::string, std::chrono::steady_clock::time_point> calls_;
};

// policy.decision for each decision record, approval.resolved for each resolution (policy::DecisionLog observer).
void record_decision(TelemetryService& svc, const policy::DecisionRecord& r);

// The rule id as telemetry may carry it: filesystem rules and grants without their paths.
std::string rule_id(const std::string& rule);

// The Run origin as the catalogue spells it: user, subagent, trigger or remote.
std::string origin_name(const runtime::RunInfo& run);

}  // namespace wb::telemetry
