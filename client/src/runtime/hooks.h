// Lifecycle hooks (client/agent-runtime Lifecycle Hooks and Mandatory Policy Hook; add-agent-runtime design D2).
#pragma once

#include "model/types.h"
#include "runtime/types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wb::runtime {

struct RunInfo {
    std::string run_id, thread_id, parent_run_id;
    bool unattended = false;
    Origin origin;
};

struct ModelCallInfo {
    RunInfo run;
    model::ChatRequest* request = nullptr;          // BeforeModelCall may edit the request
    const model::ChatResponse* response = nullptr;  // AfterModelCall
};

struct Provenance {
    std::string source_kind;  // files | web | mail | transcript | mcp | browser | user | runtime ...
    std::string source_id;
    std::string at;
    bool trusted = true;
};

// A file or URL a call uses, declared by the tool from its arguments; client/policy-enforcement checks each against
// the filesystem and egress rules (design D3 step 2).
struct ToolResource {
    enum class Kind { read_path, write_path, url };
    Kind kind = Kind::read_path;
    std::string value;
};

// What the active Skill declares for its subagent's calls (client/skills interrupt_on and risk).
struct SkillDeclaration {
    std::string skill;
    std::vector<std::string> interrupt_on;
    std::string max_risk;  // read | write-local | external | destructive
};

struct ToolCallInfo {
    RunInfo run;
    std::string call_id;
    std::string tool;  // Tool id (files.read, mcp:server/tool)
    json args = json::object();
    std::string rationale;
    Risk risk = Risk::destructive;   // the Risk class: declared, computed from the arguments, or destructive when undeclared
    bool risk_declared = false;
    // The tool touches only the client's own data (Threads, attachments): the only calls allowed without a valid policy.
    bool own_data = false;
    std::vector<ToolResource> resources;
    std::optional<SkillDeclaration> skill;
    // Untrusted content present in the Run's context (an Approval item shows it).
    std::vector<Provenance> untrusted_context;
};

struct ToolResultInfo {
    bool ok = true;
    std::string content;
    Provenance provenance;
};

struct ToolDecision {
    enum class Kind { allow, deny, rewrite, require_approval };
    Kind kind = Kind::allow;
    std::string reason;
    json args;  // for rewrite

    static ToolDecision allow() { return {}; }
    static ToolDecision deny(std::string reason) { return {Kind::deny, std::move(reason), {}}; }
    static ToolDecision rewrite(json args, std::string reason = {}) { return {Kind::rewrite, std::move(reason), std::move(args)}; }
    static ToolDecision approval(std::string reason) { return {Kind::require_approval, std::move(reason), {}}; }
};

// A hook handler. Every method has a neutral default so that a handler overrides only what it needs. Handlers run in
// registration order; BeforeToolCall handlers may allow, deny, rewrite or require approval.
class Hook {
public:
    virtual ~Hook() = default;
    virtual void session_start(const RunInfo&) {}
    virtual void run_start(const RunInfo&) {}
    virtual void before_model_call(ModelCallInfo&) {}
    virtual void after_model_call(const ModelCallInfo&) {}
    virtual ToolDecision before_tool_call(const ToolCallInfo&) { return ToolDecision::allow(); }
    virtual void after_tool_call(const ToolCallInfo&, const ToolResultInfo&) {}
    virtual void before_compaction(const RunInfo&) {}
    virtual void run_end(const RunInfo&, RunState, const std::string& /*reason*/) {}
};

// The policy decision of client/policy-enforcement, plugged in as the first BeforeToolCall handler. The runtime refuses
// to start without one, and no later handler can remove it or override its denial.
class PolicyHook : public Hook {
public:
    ToolDecision before_tool_call(const ToolCallInfo& call) override = 0;
    // Whether the user may see and call the tool at all. find_tools never returns a tool for which this is false.
    [[nodiscard]] virtual bool tool_permitted(const std::string& tool_id, Risk declared_risk) const = 0;
    // Whether calls of the tool can be decided "approve": its schema then gets the required `rationale` argument.
    [[nodiscard]] virtual bool may_require_approval(const std::string& tool_id, Risk declared_risk) const = 0;
    // Why policy withholds the tool, naming the capability and the policy key (Withheld Tool Explanation); nullopt when
    // policy does not withhold it. find_tools reports this for matching tools instead of loading them.
    [[nodiscard]] virtual std::optional<std::string> withheld_reason(const std::string& /*tool_id*/, Risk /*declared_risk*/) const {
        return std::nullopt;
    }
};

class HookRegistry {
public:
    // `policy` may be null only to build a registry that the runtime then refuses to start with.
    explicit HookRegistry(std::shared_ptr<PolicyHook> policy);

    // Registers a handler after the policy handler (and after the handlers registered before it).
    void add(std::shared_ptr<Hook> hook);
    [[nodiscard]] bool has_policy() const { return policy_ != nullptr; }
    [[nodiscard]] const PolicyHook& policy() const { return *policy_; }

    // Runs the BeforeToolCall handlers and combines their decisions; fails closed:
    //  - a handler that throws denies the call;
    //  - a policy handler that cannot decide (throws) denies the call;
    //  - deny wins over everything, require_approval over allow and rewrite;
    //  - after rewrites the policy handler decides again on the final arguments.
    // `call.args` holds the final arguments afterwards.
    ToolDecision decide(ToolCallInfo& call) const;

    // The other hook points. A handler that throws is skipped and the failure reported through `on_error`.
    void session_start(const RunInfo& run) const;
    void run_start(const RunInfo& run) const;
    void before_model_call(ModelCallInfo& info) const;
    void after_model_call(const ModelCallInfo& info) const;
    void after_tool_call(const ToolCallInfo& call, const ToolResultInfo& result) const;
    void before_compaction(const RunInfo& run) const;
    void run_end(const RunInfo& run, RunState state, const std::string& reason) const;

    std::function<void(const std::string& what)> on_error;

private:
    template <typename F>
    void each(F&& fn) const;

    std::shared_ptr<PolicyHook> policy_;
    std::vector<std::shared_ptr<Hook>> hooks_;  // [0] is the policy handler
};

}  // namespace wb::runtime
