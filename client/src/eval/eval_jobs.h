// The evaluation service of the client (client/evaluation): one export or one replay at a time, each on its own
// background thread, polled with status and stopped with cancel (add-evaluation-export design D1, D4).
#pragma once

#include "eval/eval_service.h"
#include "eval/exporter.h"
#include "eval/replay.h"

#include <memory>
#include <mutex>
#include <thread>

namespace wb::eval {

// Parses eval.export parameters: {out_dir, from, to (ISO dates or date-times, UTC), sources[], audio}.
Result<ExportRequest> export_request_from_json(const json& params);

class EvalJobs : public EvalService {
public:
    EvalJobs(ExportEnv exp, ReplayEnv rep);
    ~EvalJobs() override;

    Result<json> start_export(const json& params) override;
    Result<json> start_run(const json& params) override;
    Result<json> status(const std::string& job) const override;
    Status cancel(const std::string& job) override;
    void wait();  // for tests: the running export finishes
    // Cancels whatever runs (policy.evaluation.export was turned off).
    void cancel_all() override;

private:
    void export_job(ExportRequest r, std::filesystem::path folder);
    Exporter exporter_;
    Replayer replayer_;
    mutable std::mutex mx_;
    std::string export_id_;
    json export_status_ = json::object();
    std::shared_ptr<CancelToken> export_cancel_;
    std::thread export_thread_;
    bool exporting_ = false;
    std::string run_id_;
};

}  // namespace wb::eval
