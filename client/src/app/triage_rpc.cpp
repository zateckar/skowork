#include "app/triage_rpc.h"

namespace wb {

void register_triage_rpc(RpcRegistry& rpc, std::function<triage::TriageService*()> svc) {
    const auto get = [svc]() -> Result<triage::TriageService*> {
        triage::TriageService* t = svc ? svc() : nullptr;
        if (!t) return make_error("unavailable", "Triage is not available.");
        return t;
    };
    const auto done = [](const Status& s) -> Result<json> {
        if (!s) return s.error();
        return json::object();
    };
    rpc.add("triage.status", RpcExposure::ui, [get](const json&) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return (*t)->status();
    });
    rpc.add("triage.configure", RpcExposure::ui, [get, done](const json& p) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return done((*t)->configure(p));
    });
    rpc.add("triage.ack_gap", RpcExposure::ui, [get, done](const json&) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return done((*t)->acknowledge_gap());
    });
    rpc.add("triage.delete", RpcExposure::ui, [get, done](const json&) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return done((*t)->delete_data());
    });
    rpc.add("triage.suggestion", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return (*t)->suggestion(jstr(p, "thread_id"));
    });
    rpc.add("triage.dismiss", RpcExposure::ui, [get, done](const json& p) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return done((*t)->dismiss(jstr(p, "thread_id")));
    });
    rpc.add("triage.no_action", RpcExposure::ui, [get, done](const json& p) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        return done((*t)->no_action(jstr(p, "thread_id")));
    });
    rpc.add("triage.start", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto t = get();
        if (!t) return t.error();
        auto r = (*t)->start_work(jstr(p, "thread_id"));
        if (!r) return r.error();
        return json{{"run_id", *r}};
    });
}

}  // namespace wb