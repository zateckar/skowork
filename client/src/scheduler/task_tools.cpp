#include "scheduler/task_tools.h"

namespace wb::scheduler {

namespace {

constexpr const char* kConditions =
    "Conditions by kind. schedule: {\"cron\": \"m h dom mon dow\"} or {\"interval_minutes\": n}, with an optional IANA "
    "\"time_zone\" (default: the computer's). file: {\"folder\": path, \"pattern\": \"*.pdf\"}. session: {\"event\": "
    "\"logon\"|\"unlock\"}. mail, calendar and meeting depend on their sources being available.";

json definition_schema(bool require) {
    json props = {
        {"name", {{"type", "string"}, {"description", "A short name shown in Automations"}}},
        {"kind", {{"type", "string"}, {"enum", {"schedule", "file", "mail", "calendar", "meeting", "session"}}}},
        {"conditions", {{"type", "object"}, {"description", kConditions}}},
        {"prompt", {{"type", "string"}, {"description", "What the agent does each time the Trigger fires"}}},
        {"skill", {{"type", "string"}, {"description", "Optional Skill to run the prompt with"}}},
        {"thread_mode", {{"type", "string"}, {"enum", {"new", "append"}}, {"description", "A new Thread each time, or one Thread for all"}}},
        {"enabled", {{"type", "boolean"}}},
        {"notify", {{"type", "boolean"}, {"description", "Notify on completion"}}},
    };
    json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
    if (require) s["required"] = {"name", "kind", "conditions", "prompt"};
    return s;
}

runtime::ToolOutput result(const Result<json>& r) {
    if (!r) return runtime::ToolOutput::failure(r.error().message.empty() ? r.error().code : r.error().message);
    return runtime::ToolOutput::success(r->dump());
}

}  // namespace

std::vector<runtime::ToolDef> task_tools(std::function<Scheduler*()> get) {
    std::vector<runtime::ToolDef> out;
    const auto with = [get](auto fn) {
        return [get, fn](runtime::ToolContext&, const json& args) -> runtime::ToolOutput {
            Scheduler* s = get();
            if (!s) return runtime::ToolOutput::failure("The scheduler is not running.");
            return fn(*s, args);
        };
    };
    const json id_schema = {{"type", "object"},
                            {"properties", {{"id", {{"type", "string"}, {"description", "The Trigger id from tasks.list"}}}}},
                            {"required", {"id"}},
                            {"additionalProperties", false}};

    runtime::ToolDef list;
    list.id = "tasks.list";
    list.description = "List the user's automations (Triggers) with their next and last runs.";
    list.risk = runtime::Risk::read;
    list.run = with([](Scheduler& s, const json&) { return runtime::ToolOutput::success(json{{"triggers", s.list()}}.dump()); });
    out.push_back(std::move(list));

    runtime::ToolDef create;
    create.id = "tasks.create";
    create.description = "Create an automation (Trigger) that starts a Run with the prompt when it fires. It is labelled as created by the agent.";
    create.risk = runtime::Risk::write_local;
    create.parameters = definition_schema(true);
    create.run = with([](Scheduler& s, const json& a) { return result(s.create(a, "agent")); });
    out.push_back(std::move(create));

    runtime::ToolDef update;
    update.id = "tasks.update";
    update.description = "Change an automation (Trigger): any of its fields except the kind.";
    update.risk = runtime::Risk::write_local;
    update.parameters = definition_schema(false);
    update.parameters["properties"]["id"] = {{"type", "string"}};
    update.parameters["properties"].erase("kind");
    update.parameters["required"] = {"id"};
    update.run = with([](Scheduler& s, const json& a) {
        json patch = a;
        patch.erase("id");
        return result(s.update(jstr(a, "id"), patch));
    });
    out.push_back(std::move(update));

    runtime::ToolDef remove;
    remove.id = "tasks.delete";
    remove.description = "Delete an automation (Trigger) and its history.";
    remove.risk = runtime::Risk::write_local;
    remove.parameters = id_schema;
    remove.run = with([](Scheduler& s, const json& a) {
        if (Status st = s.remove(jstr(a, "id")); !st) return runtime::ToolOutput::failure(st.error().message);
        return runtime::ToolOutput::success("deleted");
    });
    out.push_back(std::move(remove));

    runtime::ToolDef now;
    now.id = "tasks.run_now";
    now.description = "Run an automation (Trigger) now, as a manual Occurrence.";
    now.risk = runtime::Risk::write_local;
    now.parameters = id_schema;
    now.run = with([](Scheduler& s, const json& a) { return result(s.run_now(jstr(a, "id"))); });
    out.push_back(std::move(now));
    return out;
}

}  // namespace wb::scheduler
