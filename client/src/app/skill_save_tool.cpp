#include "app/skill_save_tool.h"

namespace wb {

namespace {

// The spike's description and schema (dev/scripts/skills-save-spike.mjs; design Spike Results). No argument names a path.
constexpr const char* kDescription =
    "Save a Skill you drafted as one of the user's private Skills, so it can be used in later Runs. Give name "
    "(kebab-case), description and instructions. A Skill needs no file path: this tool stores it, and the user approves "
    "the text first. Set update to true with a higher version to change a Skill you created earlier. Never ask the user "
    "where to save a Skill.";

constexpr const char* kParameters = R"({"type":"object","properties":{
    "name":{"type":"string","description":"Kebab-case Skill name, for example weekly-status-email"},
    "description":{"type":"string","description":"When to use the Skill, one or two sentences"},
    "instructions":{"type":"string","description":"The Skill instructions in Markdown"},
    "version":{"type":"string","description":"Semantic version, default 1.0.0"},
    "tools":{"type":"array","items":{"type":"string"},"description":"Tool ids the Skill declares"},
    "risk":{"type":"string","enum":["read","write-local","external","destructive"]},
    "network":{"type":"string","enum":["none","proxy"]},
    "files":{"type":"array","items":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}},
    "update":{"type":"boolean","description":"True to replace a Skill you created earlier"}},
    "required":["name","description","instructions"]})";

}  // namespace

runtime::ToolDef skill_save_tool(std::function<skills::AgentSkillEnv()> env_of) {
    runtime::ToolDef t;
    t.id = "skills.save";
    t.description = kDescription;
    t.parameters = json::parse(kParameters);
    t.risk = runtime::Risk::write_local;
    t.attended_only = true;

    // Refuses a save that cannot succeed before the approval is asked (runtime precheck: no Action item then).
    t.precheck = [env_of](const std::string&, const json& args) -> std::optional<std::string> {
        auto a = skills::parse_agent_args(args);
        if (!a) return a.error().message;
        auto p = skills::prepare_agent_save(env_of(), *a);
        if (!p) return p.error().message;
        return std::nullopt;
    };

    // The approval shows what the save changes, next to the arguments (which carry the full text): findings, files,
    // and for an update the changed lines (design D4, D8).
    t.approval_preview = [env_of](const json& args) -> json {
        auto a = skills::parse_agent_args(args);
        if (!a) return {{"error", a.error().message}};
        auto p = skills::prepare_agent_save(env_of(), *a);
        if (!p) return {{"error", p.error().message}};
        return skills::agent_save_preview(*p);
    };

    t.run = [env_of](runtime::ToolContext& ctx, const json& args) -> runtime::ToolOutput {
        if (ctx.unattended) return runtime::ToolOutput::failure("The tool 'skills.save' is not available in an unattended Run.");
        auto a = skills::parse_agent_args(args);
        if (!a) return runtime::ToolOutput::failure(a.error().message);
        const auto env = env_of();
        auto p = skills::prepare_agent_save(env, *a);
        if (!p) return runtime::ToolOutput::failure(p.error().message);
        auto r = skills::commit_agent_save(env, *p, ctx.thread_id);
        if (!r) return runtime::ToolOutput::failure(r.error().message);
        const std::string& name = p->args.name;
        const std::string& version = p->args.version;
        if (p->exists)
            return runtime::ToolOutput::success("Updated the private Skill \"" + name + "\" to version " + version +
                                                ". The replaced version is kept for the user to restore. It is usable from the next Run; "
                                                "a Run already in progress keeps the Skills it started with.");
        return runtime::ToolOutput::success("Saved the Skill \"" + name + "\" as a private Skill (created, version " + version +
                                            "). It is usable from the next Run; a Run already in progress keeps the Skills it started "
                                            "with. You may offer to share it. Only the user can share it, from the Skill's Publish dialog.");
    };
    return t;
}

}  // namespace wb
