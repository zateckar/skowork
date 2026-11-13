// The apps.* tools of client/local-apps (App Tools): apps.create (write-local), apps.list and apps.logs (read),
// apps.open and apps.stop (write-local), apps.delete (destructive). They are offered while apps can run (the runtime
// package is installed and policy.apps.mode is on). apps.create gives the Run write access to the app's folder.
#include "apps/apps.h"

#include "base/strings.h"

namespace wb::apps {

namespace {

using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;

json schema(json props, std::vector<std::string> required = {}) {
    json s = {{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}

const json kAppRef = {{"type", "string"}, {"description", "The app id from apps.create or apps.list"}};

constexpr const char* kHowTo =
    "The app's server is the entry file (server.js) run by the bundled Node.js runtime with built-in modules only "
    "(node:http, node:fs, node:path, node:sqlite and the like): no npm packages, no network, no child processes, files "
    "of its own folder only. It must call http.createServer(handler).listen(process.env.SKOWORK_APP_PIPE). Keep static "
    "files in public/ and data in data.db (node:sqlite). The app is served under /<app-id>/, so pages use relative URLs. "
    "Pages read data of the MCP tools in mcp_tools with fetch('_skowork/mcp/<server>/<tool>', {method: 'POST', headers: "
    "{'content-type': 'application/json'}, body: JSON.stringify(args)}), which returns {result} or {error}. Write the files "
    "with files.write into the folder, then call apps.open.";

std::string list_text(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

class Tools {
public:
    explicit Tools(AppToolsEnv env) : env_(std::move(env)) {}

    ToolOutput create(ToolContext& ctx, const json& a) {
        std::vector<std::string> tools;
        if (const json* t = jobj(a, "mcp_tools"); t && t->is_array())
            for (const auto& x : *t)
                if (x.is_string()) tools.push_back(x.get<std::string>());
        const std::string existing = trim(jstr(a, "app"));
        Result<AppInfo> r = make_error("x", "");
        if (!existing.empty()) {
            json changes = json::object();
            for (const char* k : {"name", "description", "mcp_tools"})
                if (const json* v = jobj(a, k)) changes[k] = *v;
            r = env_.apps->update(existing, changes, ctx.run_id);
        } else {
            Manifest m;
            m.name = trim(jstr(a, "name"));
            m.description = trim(jstr(a, "description"));
            m.mcp_tools = tools;
            r = env_.apps->create(m, ctx.run_id);
        }
        if (!r) return ToolOutput::failure(r.error().message);
        json out = {{"app", r->id}, {"folder", r->folder}, {"manifest", r->manifest.to_json()}, {"how_to", kHowTo}};
        if (existing.empty()) out["files"] = {"app.json", r->manifest.entry, "public\\index.html"};
        return ToolOutput::success(out.dump(2));
    }

    ToolOutput list(ToolContext&, const json&) {
        json out = json::array();
        for (const auto& a : env_.apps->list()) out.push_back(a.to_json());
        json body = {{"apps", out}};
        if (out.empty()) body["note"] = "The user has no Local apps yet.";
        return ToolOutput::success(body.dump(2));
    }

    ToolOutput logs(ToolContext&, const json& a) {
        const std::string id = trim(jstr(a, "app"));
        if (!env_.apps->get(id)) return ToolOutput::failure("No app with the id " + id + ".");
        const auto max = static_cast<size_t>(std::clamp<long long>(jint(a, "max_bytes", 16000), 1000, 64000));
        const std::string l = env_.apps->logs(id, max);
        auto out = ToolOutput::success(l.empty() ? "The app has written nothing to its log." : l);
        out.provenance = runtime::Provenance{"app", id, "", false};  // the app's own output: data, not instructions
        return out;
    }

    ToolOutput open(ToolContext&, const json& a) {
        const std::string id = trim(jstr(a, "app"));
        const auto app = env_.apps->get(id);
        if (!app) return ToolOutput::failure("No app with the id " + id + ".");
        if (app->needs_confirmation) {
            runtime::QuestionRequest q;
            q.prompt = "The app \"" + app->manifest.name + "\" wants to read data through these MCP tools of yours: " +
                       list_text(app->manifest.mcp_tools) + ". Allow it?";
            q.choices = {"Allow", "Don't allow"};
            q.state = {{"app", id}, {"hash", tools_hash(app->manifest.mcp_tools)}};
            return ToolOutput::ask(std::move(q));
        }
        return launch(id);
    }

    ToolOutput open_resumed(ToolContext&, const json&, const json& state, const std::string& answer) {
        const std::string id = jstr(state, "app");
        const auto app = env_.apps->get(id);
        if (!app) return ToolOutput::failure("The app no longer exists.");
        if (answer != "Allow") return ToolOutput::failure("The user did not allow the app's MCP tools; it was not opened.");
        if (tools_hash(app->manifest.mcp_tools) != jstr(state, "hash"))
            return ToolOutput::failure("The app's MCP tool list changed while the user was asked; call apps.open again.");
        if (Status s = env_.apps->confirm(id); !s) return ToolOutput::failure(s.error().message);
        return launch(id);
    }

    ToolOutput launch(const std::string& id) {
        if (Status s = env_.apps->start(id); !s) return ToolOutput::failure(s.error().message);
        auto url = env_.server->launch_url(id);
        if (!url) return ToolOutput::failure(url.error().message);
        if (env_.open_url) env_.open_url(*url);
        return ToolOutput::success("The app " + id + " is running and opened in the user's browser.");
    }

    ToolOutput stop(ToolContext&, const json& a) {
        const std::string id = trim(jstr(a, "app"));
        if (!env_.apps->get(id)) return ToolOutput::failure("No app with the id " + id + ".");
        if (Status s = env_.apps->stop(id); !s) return ToolOutput::failure(s.error().message);
        return ToolOutput::success("The app " + id + " is stopped.");
    }

    ToolOutput remove(ToolContext&, const json& a) {
        const std::string id = trim(jstr(a, "app"));
        if (Status s = env_.apps->remove(id); !s) return ToolOutput::failure(s.error().message);
        if (env_.server) env_.server->forget(id);
        return ToolOutput::success("The app " + id + " and its folder are deleted.");
    }

private:
    AppToolsEnv env_;
};

}  // namespace

std::vector<ToolDef> app_tools(AppToolsEnv env) {
    AppManager* apps = env.apps;
    auto t = std::make_shared<Tools>(std::move(env));
    const auto offered = [apps] { return apps && !apps->unavailable(); };
    std::vector<ToolDef> out;
    {
        ToolDef d;
        d.id = "apps.create";
        d.description = std::string("Create a Local app: a small web application for the user alone, in its own folder, or change the "
                                    "manifest of an existing one (give app). Returns the folder, which this Run may then write with "
                                    "files.write. ") + kHowTo;
        d.parameters = schema({{"name", {{"type", "string"}, {"description", "A short name; the app id is made from it"}}},
                               {"description", {{"type", "string"}}},
                               {"mcp_tools", {{"type", "array"}, {"items", {{"type", "string"}}},
                                              {"description", "Read-risk MCP tools the app's pages call, as mcp:<server>/<tool>; the user confirms them"}}},
                               {"app", kAppRef}});
        d.risk = Risk::write_local;
        d.core = true;
        d.offered = offered;
        d.run = [t](ToolContext& c, const json& a) { return t->create(c, a); };
        out.push_back(std::move(d));
    }
    {
        ToolDef d;
        d.id = "apps.list";
        d.description = "The user's Local apps with id, name, description, folder, size, last use and whether they run.";
        d.parameters = schema(json::object());
        d.risk = Risk::read;
        d.offered = offered;
        d.run = [t](ToolContext& c, const json& a) { return t->list(c, a); };
        out.push_back(std::move(d));
    }
    {
        ToolDef d;
        d.id = "apps.logs";
        d.description = "The end of a Local app's log: what its server wrote and why it stopped. Use it to fix errors.";
        d.parameters = schema({{"app", kAppRef}, {"max_bytes", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 64000}}}}, {"app"});
        d.risk = Risk::read;
        d.offered = offered;
        d.run = [t](ToolContext& c, const json& a) { return t->logs(c, a); };
        out.push_back(std::move(d));
    }
    {
        ToolDef d;
        d.id = "apps.open";
        d.description = "Start a Local app and open it in the user's browser. The user confirms the app's MCP tools first.";
        d.parameters = schema({{"app", kAppRef}}, {"app"});
        d.risk = Risk::write_local;
        d.core = true;
        d.offered = offered;
        d.run = [t](ToolContext& c, const json& a) { return t->open(c, a); };
        d.resume = [t](ToolContext& c, const json& a, const json& s, const std::string& ans) { return t->open_resumed(c, a, s, ans); };
        out.push_back(std::move(d));
    }
    {
        ToolDef d;
        d.id = "apps.stop";
        d.description = "Stop a running Local app.";
        d.parameters = schema({{"app", kAppRef}}, {"app"});
        d.risk = Risk::write_local;
        d.offered = offered;
        d.run = [t](ToolContext& c, const json& a) { return t->stop(c, a); };
        out.push_back(std::move(d));
    }
    {
        ToolDef d;
        d.id = "apps.delete";
        d.description = "Delete a Local app with its folder and data.";
        d.parameters = schema({{"app", kAppRef}}, {"app"});
        d.risk = Risk::destructive;
        d.offered = offered;
        d.run = [t](ToolContext& c, const json& a) { return t->remove(c, a); };
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace wb::apps
