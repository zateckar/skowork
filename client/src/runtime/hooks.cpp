#include "runtime/hooks.h"

namespace wb::runtime {

HookRegistry::HookRegistry(std::shared_ptr<PolicyHook> policy) : policy_(std::move(policy)) {
    if (policy_) hooks_.push_back(policy_);
}

void HookRegistry::add(std::shared_ptr<Hook> hook) {
    if (hook) hooks_.push_back(std::move(hook));
}

template <typename F>
void HookRegistry::each(F&& fn) const {
    for (const auto& h : hooks_) {
        try {
            fn(*h);
        } catch (const std::exception& e) {
            if (on_error) on_error(std::string("hook failed: ") + e.what());
        } catch (...) {
            if (on_error) on_error("hook failed");
        }
    }
}

ToolDecision HookRegistry::decide(ToolCallInfo& call) const {
    if (!policy_) return ToolDecision::deny("no policy handler is registered");

    bool approval = false;
    std::string approval_reason;
    bool rewritten = false;

    // The policy handler is first, so a denial stops the chain before any other handler sees the call.
    for (const auto& h : hooks_) {
        ToolDecision d;
        try {
            d = h->before_tool_call(call);
        } catch (const std::exception& e) {
            if (on_error) on_error(std::string("BeforeToolCall handler failed: ") + e.what());
            return ToolDecision::deny(h == policy_ ? "the policy decision could not be evaluated"
                                                   : "a hook failed while checking the call");
        } catch (...) {
            if (on_error) on_error("BeforeToolCall handler failed");
            return ToolDecision::deny(h == policy_ ? "the policy decision could not be evaluated"
                                                   : "a hook failed while checking the call");
        }
        switch (d.kind) {
        case ToolDecision::Kind::deny: return d;
        case ToolDecision::Kind::rewrite:
            if (!d.args.is_object()) return ToolDecision::deny("a hook returned invalid rewritten arguments");
            call.args = std::move(d.args);
            rewritten = true;
            break;
        case ToolDecision::Kind::require_approval:
            approval = true;
            if (approval_reason.empty()) approval_reason = d.reason;
            break;
        case ToolDecision::Kind::allow: break;
        }
    }

    // A rewrite after the policy handler decided could turn an allowed call into a forbidden one: decide again.
    if (rewritten) {
        try {
            ToolDecision d = policy_->before_tool_call(call);
            if (d.kind == ToolDecision::Kind::deny) return d;
            if (d.kind == ToolDecision::Kind::require_approval) {
                approval = true;
                if (approval_reason.empty()) approval_reason = d.reason;
            }
        } catch (...) {
            return ToolDecision::deny("the policy decision could not be evaluated");
        }
    }
    if (approval) return ToolDecision::approval(approval_reason);
    return ToolDecision::allow();
}

void HookRegistry::session_start(const RunInfo& run) const {
    each([&](Hook& h) { h.session_start(run); });
}
void HookRegistry::run_start(const RunInfo& run) const {
    each([&](Hook& h) { h.run_start(run); });
}
void HookRegistry::before_model_call(ModelCallInfo& info) const {
    each([&](Hook& h) { h.before_model_call(info); });
}
void HookRegistry::after_model_call(const ModelCallInfo& info) const {
    each([&](Hook& h) { h.after_model_call(info); });
}
void HookRegistry::after_tool_call(const ToolCallInfo& call, const ToolResultInfo& result) const {
    each([&](Hook& h) { h.after_tool_call(call, result); });
}
void HookRegistry::before_compaction(const RunInfo& run) const {
    each([&](Hook& h) { h.before_compaction(run); });
}
void HookRegistry::run_end(const RunInfo& run, RunState state, const std::string& reason) const {
    each([&](Hook& h) { h.run_end(run, state, reason); });
}

}  // namespace wb::runtime
