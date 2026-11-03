// The mandatory BeforeToolCall handler of client/policy-enforcement (design D3), replacing the interim policy of
// add-agent-runtime: decides every call from the Policy bundle in force, checks file and URL arguments, records
// decisions in the approval log, and hides tools the policy switches off.
#pragma once

#include "policy/decide.h"
#include "policy/paths.h"
#include "policy/store.h"
#include "runtime/hooks.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wb::policy {

struct EnforcerEnv {
    std::function<std::shared_ptr<const EffectivePolicy>()> policy;  // null result: fail closed
    std::string workspace;  // agent workspace folder (always readable and writable)
    std::string data_root;  // the client's data root
    std::function<std::vector<FolderGrant>()> grants;
    // Local app folders a Run may read and write (client/local-apps); "" gives those of every active Run. Optional.
    std::function<std::vector<std::string>(const std::string& run_id)> app_folders;
    DecisionLog* log = nullptr;  // optional
    ToolFloors floors = declared_tool_floors();
    // Why a tool cannot run on this computer right now (for example the selected shell is not installed); such a tool
    // is not offered and its calls are denied. Optional.
    std::function<std::optional<std::string>(const std::string& tool_id)> unavailable;
};

class PolicyEnforcer final : public runtime::PolicyHook {
public:
    explicit PolicyEnforcer(EnforcerEnv env) : env_(std::move(env)) {}

    runtime::ToolDecision before_tool_call(const runtime::ToolCallInfo& call) override;
    [[nodiscard]] bool tool_permitted(const std::string& tool_id, runtime::Risk declared_risk) const override;
    [[nodiscard]] bool may_require_approval(const std::string& tool_id, runtime::Risk declared_risk) const override;
    [[nodiscard]] std::optional<std::string> withheld_reason(const std::string& tool_id, runtime::Risk declared_risk) const override;

    // The verdict for a call without recording it (tests, simulation parity).
    [[nodiscard]] Verdict evaluate(const runtime::ToolCallInfo& call) const;
    // The filesystem rules in force (for file tools that open paths through open_for_read / open_for_write).
    [[nodiscard]] PathRules path_rules_now() const;
    // Records the user's resolution of an Approval in the approval log.
    void record_resolution(const std::string& run_id, const std::string& call_id, const std::string& tool, const std::string& resolution);

private:
    EnforcerEnv env_;
};

// Risk class names as policy keys spell them.
const char* risk_key(runtime::Risk r);

// The tool groups a mode setting can switch off: shell.* by tools.shell.mode, computer.* by tools.computer.mode
// (client/tool-sandbox Shell Modes, client/computer-use).
bool tool_switched_off(const EffectivePolicy* policy, const std::string& tool_id);

}  // namespace wb::policy
