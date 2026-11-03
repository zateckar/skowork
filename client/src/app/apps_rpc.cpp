#include "app/apps_rpc.h"

#include "base/strings.h"

namespace wb {

namespace {

json listing(apps::AppManager& m) {
    json out = json::array();
    for (const auto& a : m.list()) out.push_back(a.to_json());
    const auto why = m.unavailable();
    return {{"apps", out}, {"unavailable", why ? json(*why) : json(nullptr)}, {"mcp", m.mcp_mode()}};
}

}  // namespace

void register_apps_rpc(RpcRegistry& rpc, AppsRpcEnv env) {
    auto e = std::make_shared<AppsRpcEnv>(std::move(env));
    const auto manager = [e]() -> Result<apps::AppManager*> {
        apps::AppManager* m = e->apps ? e->apps() : nullptr;
        if (!m) return make_error("unavailable", "Local apps are not available yet.");
        return m;
    };
    rpc.add("apps.list", RpcExposure::ui, [manager](const json&) -> Result<json> {
        auto m = manager();
        if (!m) return m.error();
        return listing(**m);
    });
    // Opens the app in the user's browser. An app whose MCP tool list is not confirmed answers needs_confirmation with
    // the list; the view asks the user and calls apps.confirm first.
    rpc.add("apps.open", RpcExposure::ui, [manager, e](const json& p) -> Result<json> {
        auto m = manager();
        if (!m) return m.error();
        const std::string id = jstr(p, "id");
        const auto app = (*m)->get(id);
        if (!app) return make_error("not_found", "No app with the id " + id + ".");
        if (app->needs_confirmation) return json{{"needs_confirmation", true}, {"mcp_tools", app->manifest.mcp_tools}};
        apps::AppServer* s = e->server ? e->server() : nullptr;
        if (!s) return make_error("unavailable", "Local apps are not available yet.");
        if (Status st = (*m)->start(id); !st) return st.error();
        auto url = s->launch_url(id);
        if (!url) return url.error();
        if (e->open_url) e->open_url(*url);
        return json{{"opened", true}};
    });
    rpc.add("apps.confirm", RpcExposure::ui, [manager](const json& p) -> Result<json> {
        auto m = manager();
        if (!m) return m.error();
        if (Status s = (*m)->confirm(jstr(p, "id")); !s) return s.error();
        return json{{"ok", true}};
    });
    rpc.add("apps.stop", RpcExposure::ui, [manager](const json& p) -> Result<json> {
        auto m = manager();
        if (!m) return m.error();
        if (Status s = (*m)->stop(jstr(p, "id")); !s) return s.error();
        return listing(**m);
    });
    rpc.add("apps.delete", RpcExposure::ui, [manager, e](const json& p) -> Result<json> {
        auto m = manager();
        if (!m) return m.error();
        const std::string id = jstr(p, "id");
        if (Status s = (*m)->remove(id); !s) return s.error();
        if (apps::AppServer* s = e->server ? e->server() : nullptr) s->forget(id);
        return listing(**m);
    });
    rpc.add("apps.reveal", RpcExposure::ui, [manager, e](const json& p) -> Result<json> {
        auto m = manager();
        if (!m) return m.error();
        const auto app = (*m)->get(jstr(p, "id"));
        if (!app) return make_error("not_found", "No app with the id " + jstr(p, "id") + ".");
        if (!e->reveal) return make_error("unavailable", "The folder cannot be shown.");
        if (Status s = e->reveal(app->folder); !s) return s.error();
        return json{{"ok", true}};
    });
}

}  // namespace wb
