// Evaluation jobs (client/evaluation): one export or replay at a time, run in the background and polled with status.
// The RPC layer (app/eval_rpc) checks policy.evaluation.export before any call reaches this service.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <string>

namespace wb::eval {

class EvalService {
public:
    virtual ~EvalService() = default;
    // {out_dir, from, to (ISO), sources[], audio, dry_run} -> {job, folder?, audio_bytes}
    virtual Result<json> start_export(const json& params) = 0;
    // {folder, step, variant, max_items} -> {job}
    virtual Result<json> start_run(const json& params) = 0;
    // {state: running|paused|done|failed|cancelled, kind, step?, done, total, error?, folder?}
    virtual Result<json> status(const std::string& job) const = 0;
    virtual Status cancel(const std::string& job) = 0;
    virtual void cancel_all() = 0;  // the setting was turned off
};

}  // namespace wb::eval
