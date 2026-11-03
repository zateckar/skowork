#include "app/skill_authoring.h"

#include "base/strings.h"
#include "base/url.h"
#include "skills/catalog.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace wb {

namespace fs = std::filesystem;

namespace {

skills::SkillSource source_of(const json& p) { return {jstr(p, "frontmatter"), jstr(p, "body")}; }

std::vector<std::string> strings_of(const json& p, const char* key) {
    std::vector<std::string> out;
    if (const json* a = jobj(p, key); a && a->is_array())
        for (const auto& x : *a)
            if (x.is_string() && !trim(x.get<std::string>()).empty()) out.push_back(trim(x.get<std::string>()));
    return out;
}

json replacement_json(const skills::Replacement& r) { return {{"kind", r.kind}, {"original", r.original}, {"parameter", r.parameter}}; }

// Runtime tools (use_skill, delegate, find_tools...) have neither a dot nor an MCP prefix; a Skill does not declare them.
bool declarable_tool(const std::string& id) { return id.find('.') != std::string::npos || id.find(':') != std::string::npos; }

}  // namespace

SkillAuthoring::SkillAuthoring(SkillAuthoringEnv env) : env_(std::move(env)) {}

fs::path SkillAuthoring::folder(const std::string& name) const { return env_.private_dir / skills::utf8_path(name); }

json SkillAuthoring::files_json(const std::string& name) const {
    json files = json::array();
    for (const auto& f : skills::list_files(folder(name))) files.push_back({{"path", f.path}, {"size", f.size}});
    return files;
}

Result<json> SkillAuthoring::source(const std::string& name) {
    if (!skills::valid_name(name)) return make_error("invalid_params", "not a Skill name");
    std::ifstream in(folder(name) / "SKILL.md", std::ios::binary);
    if (!in) return make_error("not_found", "No private Skill named " + name + ".");
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto s = skills::split_skill_md(ss.str());
    return json{{"name", name}, {"frontmatter", s.frontmatter}, {"body", s.body}, {"files", files_json(name)}};
}

json SkillAuthoring::validate(const skills::SkillSource& s) {
    const std::string text = skills::join_skill_md(s);
    if (text.size() > skills::kMaxSkillMdBytes) return {{"valid", false}, {"error", {{"field", "SKILL.md"}, {"message", "SKILL.md is larger than 256 KB."}}}};
    auto parsed = skills::parse_skill_md(text, false);
    if (const auto* e = std::get_if<skills::ParseError>(&parsed))
        return {{"valid", false}, {"error", {{"field", e->field}, {"message", e->message}}}};
    const auto& m = std::get<skills::Parsed>(parsed).manifest;
    return {{"valid", true}, {"name", m.name}, {"version", m.version}};
}

Result<json> SkillAuthoring::save(const std::string& original, const skills::SkillSource& s) {
    auto name = skills::save_private(env_.private_dir, original, s);
    if (!name) return name.error();
    if (env_.reload) env_.reload();
    return json{{"name", *name}, {"files", files_json(*name)}};
}

Result<json> SkillAuthoring::remove(const std::string& name) {
    if (Status st = skills::delete_private(env_.private_dir, name); !st) return st.error();
    if (env_.reload) env_.reload();
    return json{{"ok", true}};
}

Result<json> SkillAuthoring::add_files(const std::string& name, const std::string& sub, const std::vector<std::string>& paths) {
    if (!skills::valid_name(name)) return make_error("invalid_params", "not a Skill name");
    std::vector<fs::path> files;
    for (const auto& p : paths) files.push_back(skills::utf8_path(p));
    if (Status st = skills::add_files(folder(name), sub, files); !st) return st.error();
    return json{{"files", files_json(name)}};
}

Result<json> SkillAuthoring::remove_file(const std::string& name, const std::string& path) {
    if (!skills::valid_name(name)) return make_error("invalid_params", "not a Skill name");
    if (Status st = skills::remove_file(folder(name), path); !st) return st.error();
    return json{{"files", files_json(name)}};
}

Result<json> SkillAuthoring::draft(const std::string& thread) {
    if (!env_.conversation || !env_.model) return make_error("unavailable", "the agent runtime is not running");
    skills::ThreadDigest d;
    d.title = env_.title ? env_.title(thread) : std::string();
    d.names = env_.user_names ? env_.user_names() : std::vector<std::string>{};
    for (const auto& m : env_.conversation(thread)) {
        if ((m.role == "user" || m.role == "assistant") && !trim(m.content).empty()) d.messages.emplace_back(m.role, m.content);
        for (const auto& c : m.tool_calls) {
            if (!declarable_tool(c.tool) || std::find(d.tools.begin(), d.tools.end(), c.tool) != d.tools.end()) continue;
            d.tools.push_back(c.tool);
            const std::string r = env_.tool_risk ? env_.tool_risk(c.tool) : std::string();
            if (skills::risk_rank(r) > skills::risk_rank(d.risk)) d.risk = r;
        }
    }
    if (d.messages.empty()) return make_error("empty", "The Thread has no messages to draft a Skill from.");
    auto draft = skills::draft_skill(d, [&](const std::string& system, const std::string& user) { return env_.model(thread, system, user); });
    if (!draft) return draft.error();
    json replaced = json::array();
    for (const auto& r : draft->replaced) replaced.push_back(replacement_json(r));
    return json{{"frontmatter", draft->source.frontmatter}, {"body", draft->source.body}, {"replaced", replaced}};
}

Result<json> SkillAuthoring::test(const skills::SkillSource& s, const std::string& input) {
    auto parsed = skills::parse_skill_md(skills::join_skill_md(s), false);
    if (const auto* e = std::get_if<skills::ParseError>(&parsed)) return make_error("invalid", e->field + ": " + e->message);
    if (!env_.create_thread || !env_.send) return make_error("unavailable", "the agent runtime is not running");
    const auto& p = std::get<skills::Parsed>(parsed);
    const std::string name = p.manifest.name;
    auto thread = env_.create_thread("Skill test: " + name, json{{"skill_test", name}});
    if (!thread) return thread.error();
    std::error_code ec;
    const fs::path dir = folder(name);
    runtime::SkillRun run{p.manifest, p.body, "draft", fs::is_directory(dir, ec) ? skills::path_utf8(dir) : std::string()};
    {
        const std::lock_guard lock(mx_);
        tests_[*thread] = std::move(run);
    }
    const std::string text = "/" + name + (trim(input).empty() ? std::string() : " " + trim(input));
    if (Status st = env_.send(*thread, text); !st) return st.error();
    return json{{"thread_id", *thread}};
}

std::optional<runtime::SkillRun> SkillAuthoring::test_skill(const std::string& thread, const std::string& name) const {
    const std::lock_guard lock(mx_);
    const auto it = tests_.find(thread);
    if (it == tests_.end() || it->second.manifest.name != name) return std::nullopt;
    return it->second;
}

std::optional<runtime::SkillRun> SkillAuthoring::test_draft(const std::string& thread) const {
    const std::lock_guard lock(mx_);
    const auto it = tests_.find(thread);
    if (it == tests_.end()) return std::nullopt;
    return it->second;
}

bool SkillAuthoring::is_test_thread(const std::string& thread) const {
    const std::lock_guard lock(mx_);
    return tests_.count(thread) > 0;
}

Result<json> SkillAuthoring::publish(const std::string& name, const std::string& scope, const std::vector<std::string>& users,
                                     const std::vector<std::string>& groups) {
    const auto allowed = env_.publish_scopes ? env_.publish_scopes() : std::vector<std::string>{"shared"};
    if (std::find(allowed.begin(), allowed.end(), scope) == allowed.end())
        return make_error("scope_not_permitted", "Your policy does not allow publishing to the " + scope + " scope.");
    if (scope == "shared" && users.empty()) return make_error("invalid_params", "Name at least one colleague to share the Skill with.");
    if (scope == "group") {
        if (groups.empty()) return make_error("invalid_params", "Choose at least one Group.");
        const auto mine = env_.groups ? env_.groups() : std::vector<std::string>{};
        for (const auto& g : groups)
            if (std::find(mine.begin(), mine.end(), g) == mine.end()) return make_error("not_a_member", "You are not a member of the Group " + g + ".");
    }
    if (!skills::valid_name(name)) return make_error("invalid_params", "not a Skill name");
    auto zip = skills::zip_folder(folder(name));
    if (!zip) return zip.error();
    if (!env_.publish) return make_error("unavailable", "not registered with the control plane");
    std::string query = "scope=" + url_encode(scope);
    const auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
        return s;
    };
    if (scope == "shared") query += "&users=" + url_encode(list(users));
    if (scope == "group") query += "&groups=" + url_encode(list(groups));
    return env_.publish(query, *zip);
}

Result<json> SkillAuthoring::published() {
    if (!env_.mine) return make_error("unavailable", "not registered with the control plane");
    return env_.mine();
}

Result<json> SkillAuthoring::withdraw(const std::string& name, const std::string& version) {
    if (!skills::valid_name(name)) return make_error("invalid_params", "not a Skill name");
    if (trim(version).empty() || version.find_first_of("/?#%") != std::string::npos)
        return make_error("invalid_params", "not a Skill version");
    if (!env_.withdraw) return make_error("unavailable", "not registered with the control plane");
    return env_.withdraw(name, version);
}

void SkillAuthoring::register_rpc(RpcRegistry& rpc, RpcExposure ex) {
    rpc.add("skills.source", ex, [this](const json& p) { return source(jstr(p, "name")); });
    rpc.add("skills.validate", ex, [](const json& p) -> Result<json> { return validate(source_of(p)); });
    rpc.add("skills.save", ex, [this](const json& p) { return save(jstr(p, "original"), source_of(p)); });
    rpc.add("skills.delete", ex, [this](const json& p) { return remove(jstr(p, "name")); });
    rpc.add("skills.files.add", ex, [this](const json& p) { return add_files(jstr(p, "name"), jstr(p, "folder"), strings_of(p, "paths")); });
    rpc.add("skills.files.remove", ex, [this](const json& p) { return remove_file(jstr(p, "name"), jstr(p, "path")); });
    rpc.add("skills.draft", ex, [this](const json& p) { return draft(jstr(p, "thread_id")); });
    rpc.add("skills.keep", ex, [](const json& p) -> Result<json> {
        const json* r = jobj(p, "replacement");
        if (!r) return make_error("invalid_params", "replacement is required");
        const auto s = skills::keep_original(source_of(p), {jstr(*r, "kind"), jstr(*r, "original"), jstr(*r, "parameter")});
        return json{{"frontmatter", s.frontmatter}, {"body", s.body}};
    });
    rpc.add("skills.test", ex, [this](const json& p) { return test(source_of(p), jstr(p, "input")); });
    rpc.add("skills.publish_options", ex, [this](const json&) -> Result<json> {
        return json{{"scopes", env_.publish_scopes ? env_.publish_scopes() : std::vector<std::string>{"shared"}},
                    {"groups", env_.groups ? env_.groups() : std::vector<std::string>{}}};
    });
    rpc.add("skills.publish", ex, [this](const json& p) {
        return publish(jstr(p, "name"), jstr(p, "scope"), strings_of(p, "users"), strings_of(p, "groups"));
    });
    rpc.add("skills.published", ex, [this](const json&) { return published(); });
    rpc.add("skills.withdraw", ex, [this](const json& p) { return withdraw(jstr(p, "name"), jstr(p, "version")); });
}

}  // namespace wb
