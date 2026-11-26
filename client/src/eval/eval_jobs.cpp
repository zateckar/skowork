#include "eval/eval_jobs.h"
#include "meeting/audio_file.h"

#include "base/encoding.h"
#include "base/time.h"
#include "platform/crypto.h"

namespace wb::eval {

Result<ExportRequest> export_request_from_json(const json& params) {
    ExportRequest r;
    const std::string out = jstr(params, "out_dir");
    if (out.empty()) return make_error("invalid_params", "out_dir is the absolute folder to export into.");
    r.out_dir = meeting::path_from_utf8(out);
    const auto from = parse_iso8601_ms(jstr(params, "from"));
    const auto to = parse_iso8601_ms(jstr(params, "to"));
    if (!from || !to) return make_error("invalid_params", "from and to are ISO dates, for example 2026-09-01.");
    r.from_ms = *from;
    r.to_ms = *to;
    if (const json* s = jobj(params, "sources"); s && s->is_array())
        for (const auto& x : *s)
            if (x.is_string()) r.sources.insert(x.get<std::string>());
    r.audio = jbool(params, "audio");
    if (Status st = check_request(r); !st) return st.error();
    return r;
}

EvalJobs::EvalJobs(ExportEnv exp, ReplayEnv rep) : exporter_(std::move(exp)), replayer_(std::move(rep)) {}

EvalJobs::~EvalJobs() {
    {
        const std::lock_guard lock(mx_);
        if (export_cancel_) export_cancel_->cancel();
    }
    if (export_thread_.joinable()) export_thread_.join();
}

Result<json> EvalJobs::start_export(const json& params) {
    auto r = export_request_from_json(params);
    if (!r) return r.error();
    const long long audio_bytes = exporter_.audio_size(*r);
    if (jbool(params, "dry_run")) return json{{"audio_bytes", audio_bytes}};
    const std::lock_guard lock(mx_);
    if (exporting_ || jstr(replayer_.status(), "state") == "running" || jstr(replayer_.status(), "state") == "paused")
        return make_error("busy", "Another evaluation job is running.");
    auto folder = exporter_.begin(*r);
    if (!folder) return folder.error();
    if (export_thread_.joinable()) export_thread_.join();
    export_id_ = "exp_" + hex_encode(random_bytes(6));
    export_cancel_ = std::make_shared<CancelToken>();
    exporting_ = true;
    export_status_ = {{"job", export_id_}, {"kind", "export"}, {"state", "running"}, {"folder", meeting::path_to_utf8(*folder)},
                      {"done", 0}, {"total", static_cast<long long>(r->sources.size())}};
    export_thread_ = std::thread([this, req = *r, f = *folder] { export_job(req, f); });
    return json{{"job", export_id_}, {"folder", meeting::path_to_utf8(*folder)}, {"audio_bytes", audio_bytes}};
}

void EvalJobs::export_job(ExportRequest r, std::filesystem::path folder) {
    std::map<std::string, ExportCounts> counts;
    std::string error;
    std::shared_ptr<CancelToken> cancel;
    {
        const std::lock_guard lock(mx_);
        cancel = export_cancel_;
    }
    (void)exporter_.write_manifest(folder, r, counts, false);
    long long done = 0;
    for (const std::string source : {"mail", "teams", "calendar", "meetings"}) {
        if (!r.sources.count(source) || !error.empty() || cancel->cancelled()) continue;
        Result<ExportCounts> n = source == "mail"       ? exporter_.export_mail(folder, r, *cancel)
                                 : source == "teams"    ? exporter_.export_teams(folder, r, *cancel)
                                 : source == "calendar" ? exporter_.export_calendar(folder, r, *cancel)
                                                        : exporter_.export_meetings(folder, r, *cancel);
        if (!n) {
            if (n.error().code != "cancelled") error = n.error().message;
            continue;
        }
        counts[source] = *n;
        ++done;
        const std::lock_guard lock(mx_);
        export_status_["done"] = done;
    }
    const bool complete = error.empty() && !cancel->cancelled();
    (void)exporter_.write_manifest(folder, r, counts, complete);
    const std::lock_guard lock(mx_);
    export_status_["state"] = !error.empty() ? "failed" : cancel->cancelled() ? "cancelled" : "done";
    if (!error.empty()) export_status_["error"] = error;
    json c = json::object();
    for (const auto& [s, n] : counts) c[s] = {{"written", n.written}, {"withheld", n.withheld}};
    export_status_["counts"] = c;
    exporting_ = false;
}

Result<json> EvalJobs::start_run(const json& params) {
    auto r = replay_request_from_json(params);
    if (!r) return r.error();
    {
        const std::lock_guard lock(mx_);
        if (exporting_) return make_error("busy", "An export is running.");
    }
    auto id = replayer_.start(*r);
    if (!id) return id.error();
    const std::lock_guard lock(mx_);
    run_id_ = *id;
    return json{{"job", *id}};
}

Result<json> EvalJobs::status(const std::string& job) const {
    const std::lock_guard lock(mx_);
    if (!job.empty() && job == export_id_) return export_status_;
    if (!job.empty() && job == run_id_) {
        json s = replayer_.status();
        s["job"] = job;
        s["kind"] = "run";
        return s;
    }
    return make_error("not_found", "No such evaluation job.");
}

Status EvalJobs::cancel(const std::string& job) {
    const std::lock_guard lock(mx_);
    if (!job.empty() && job == export_id_) {
        if (export_cancel_) export_cancel_->cancel();
        return {};
    }
    if (!job.empty() && job == run_id_) {
        replayer_.cancel();
        return {};
    }
    return make_error("not_found", "No such evaluation job.");
}

void EvalJobs::wait() {
    if (export_thread_.joinable()) export_thread_.join();
    replayer_.wait();
}

}  // namespace wb::eval
