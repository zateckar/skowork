#include "app/eval_rpc.h"

#include "eval/eval_service.h"

namespace wb {

void register_eval_rpc(RpcRegistry& rpc, std::function<eval::EvalService*()> svc, std::function<bool()> enabled) {
    // client/evaluation Evaluation Setting: pipe only, refused while policy.evaluation.export is off.
    auto guarded = [svc, enabled](auto call) {
        return [svc, enabled, call](const json& p) -> Result<json> {
            if (!enabled()) return make_error("not_enabled", "Evaluation export is off (policy.evaluation.export).");
            eval::EvalService* s = svc();
            if (!s) return make_error("unavailable", "Evaluation is not available.");
            return call(*s, p);
        };
    };
    rpc.add("eval.export", RpcExposure::pipe, guarded([](eval::EvalService& s, const json& p) { return s.start_export(p); }));
    rpc.add("eval.run", RpcExposure::pipe, guarded([](eval::EvalService& s, const json& p) { return s.start_run(p); }));
    rpc.add("eval.status", RpcExposure::pipe,
            guarded([](eval::EvalService& s, const json& p) -> Result<json> { return s.status(jstr(p, "job")); }));
    rpc.add("eval.cancel", RpcExposure::pipe, guarded([](eval::EvalService& s, const json& p) -> Result<json> {
                if (Status st = s.cancel(jstr(p, "job")); !st) return st.error();
                return json{{"cancelled", true}};
            }));
}

}  // namespace wb
