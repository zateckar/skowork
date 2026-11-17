#include "app/agenda_rpc.h"

namespace wb {

void register_agenda_rpc(RpcRegistry& rpc, std::function<agenda::Service*()> svc) {
    const auto get = [svc]() -> Result<agenda::Service*> {
        agenda::Service* a = svc ? svc() : nullptr;
        if (!a) return make_error("unavailable", "The Agenda is not available yet.");
        return a;
    };
    rpc.add("agenda.status", RpcExposure::ui, [get](const json&) -> Result<json> {
        auto a = get();
        if (!a) return a.error();
        return (*a)->status();
    });
    rpc.add("agenda.set", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto a = get();
        if (!a) return a.error();
        return (*a)->set(p);
    });
    rpc.add("agenda.list", RpcExposure::ui, [get](const json&) -> Result<json> {
        auto a = get();
        if (!a) return a.error();
        json out = (*a)->list();
        out["status"] = (*a)->status();
        return out;
    });
    const auto item_method = [&rpc, get](const char* name, Result<json> (agenda::Service::*fn)(const std::string&)) {
        rpc.add(name, RpcExposure::ui, [get, fn](const json& p) -> Result<json> {
            auto a = get();
            if (!a) return a.error();
            const std::string id = jstr(p, "id");
            if (id.empty()) return make_error("invalid_params", "id is required");
            return ((**a).*fn)(id);
        });
    };
    item_method("agenda.done", &agenda::Service::mark_done);
    item_method("agenda.dismiss", &agenda::Service::dismiss);
    item_method("agenda.reopen", &agenda::Service::reopen);
    item_method("agenda.prepare", &agenda::Service::prepare);
    rpc.add("agenda.edit", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto a = get();
        if (!a) return a.error();
        const std::string id = jstr(p, "id");
        if (id.empty()) return make_error("invalid_params", "id is required");
        return (*a)->edit(id, p.value("patch", json::object()));
    });
    rpc.add("agenda.delete_all", RpcExposure::ui, [get](const json&) -> Result<json> {
        auto a = get();
        if (!a) return a.error();
        if (Status s = (*a)->delete_all(); !s) return s.error();
        return (*a)->status();
    });
}

}  // namespace wb
