#include "app/messaging_rpc.h"

namespace wb {

namespace {

std::vector<std::string> names(const json& p, const char* key) {
    std::vector<std::string> out;
    if (const json* a = jobj(p, key); a && a->is_array())
        for (const auto& x : *a)
            if (x.is_string() && !x.get<std::string>().empty()) out.push_back(x.get<std::string>());
    return out;
}

}  // namespace

void register_messaging_rpc(RpcRegistry& rpc, std::function<messaging::MessagingService*()> svc) {
    const auto get = [svc]() -> Result<messaging::MessagingService*> {
        messaging::MessagingService* m = svc ? svc() : nullptr;
        if (!m) return make_error("unavailable", "Messaging is not available yet.");
        return m;
    };
    // Every method answers with the listing the view shows, so one call is enough to redraw it.
    const auto listing = [](messaging::MessagingService& m) {
        json s = m.status();
        s["chats"] = m.chats();
        return s;
    };
    rpc.add("messaging.status", RpcExposure::ui, [get](const json&) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        return (*m)->status();
    });
    rpc.add("messaging.chats", RpcExposure::ui, [get, listing](const json&) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        return listing(**m);
    });
    rpc.add("messaging.messages", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        const std::string id = jstr(p, "chat");
        if (Status s = (*m)->open(id); !s) return s.error();
        return json{{"chat", (*m)->chat(id)}, {"messages", (*m)->messages(id)}};
    });
    rpc.add("messaging.send", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        const std::string id = jstr(p, "chat");
        auto r = (*m)->send(id, jstr(p, "text"));
        if (!r) return r.error();
        return json{{"sent", *r}, {"chat", (*m)->chat(id)}, {"messages", (*m)->messages(id)}};
    });
    rpc.add("messaging.directory", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        return (*m)->directory(jstr(p, "q"));
    });
    rpc.add("messaging.create", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        if (jstr(p, "kind") == "team") return (*m)->create_team(jstr(p, "name"), names(p, "members"));
        return (*m)->create_direct(jstr(p, "user"));
    });
    rpc.add("messaging.rename", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        return (*m)->rename(jstr(p, "chat"), jstr(p, "name"));
    });
    rpc.add("messaging.add_members", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        return (*m)->add_members(jstr(p, "chat"), names(p, "users"));
    });
    rpc.add("messaging.leave", RpcExposure::ui, [get, listing](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        if (Status s = (*m)->leave(jstr(p, "chat")); !s) return s.error();
        return listing(**m);
    });
    rpc.add("messaging.busy", RpcExposure::ui, [get](const json& p) -> Result<json> {
        auto m = get();
        if (!m) return m.error();
        if (Status s = (*m)->set_busy(jbool(p, "busy")); !s) return s.error();
        return (*m)->status();
    });
}

}  // namespace wb
