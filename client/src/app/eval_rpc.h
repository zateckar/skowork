// The evaluation methods of the local API (client/evaluation): eval.export, eval.run, eval.status and eval.cancel.
// Pipe only (add-evaluation-export design D1); every call is refused with not_enabled while policy.evaluation.export
// is off.
#pragma once

#include "host/rpc.h"

#include <functional>

namespace wb {
namespace eval {
class EvalService;
}

void register_eval_rpc(RpcRegistry& rpc, std::function<eval::EvalService*()> svc, std::function<bool()> enabled);

}  // namespace wb
