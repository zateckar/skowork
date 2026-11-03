// Fuzz target for SKILL.md and its frontmatter (client/skills Skill Package Format; add-skills task 2.1): Skills come
// from colleagues and the registry, so any text must give a Skill or a validation error naming a field, never a crash,
// hang or unbounded output.
#include "fuzz_main.h"
#include "skills/skill.h"
#include "skills/yaml.h"

using namespace wb::skills;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    const auto y = parse_yaml(text);
    if (y.ok() && !y->is_object()) __builtin_trap();
    if (!y.ok() && y.error().message.empty()) __builtin_trap();

    // As a whole SKILL.md, and with a frontmatter wrapper around the input.
    for (const std::string& doc : {std::string(text), "---\nname: fuzz\ndescription: d\n" + std::string(text) + "\n---\nbody"}) {
        for (bool require_version : {false, true}) {
            const auto r = parse_skill_md(doc, require_version);
            if (const auto* e = std::get_if<ParseError>(&r)) {
                if (e->field.empty() || e->message.empty()) __builtin_trap();
                continue;
            }
            const Parsed& p = std::get<Parsed>(r);
            const Manifest& m = p.manifest;
            if (!valid_name(m.name) || m.description.empty() || !valid_version(m.version)) __builtin_trap();
            if (require_version && !m.version_declared) __builtin_trap();
            if (risk_rank(m.risk) < 0 || (m.network != "none" && m.network != "proxy")) __builtin_trap();
            if (p.body.size() > doc.size()) __builtin_trap();
            for (const auto& rule : m.interrupt_on) {
                if (rule.tool.empty()) __builtin_trap();
                for (const auto& res : allowed_resolutions(m, rule.tool))
                    if (res != "approve" && res != "approve_edited" && res != "reject" && res != "respond_instead") __builtin_trap();
            }
            std::vector<ToolFacts> tools = {{"files.read", "read", true}, {"shell.run", "external", false}, {m.name, m.risk, true}};
            const Scope s = tool_scope(m, tools);
            for (const auto& t : s.tools)
                if (t == "shell.run") __builtin_trap();  // never a tool policy does not permit
        }
    }
    // The glob over arbitrary pattern and text halves.
    (void)tool_matches(text.substr(0, size / 2), text.substr(size / 2));
    return 0;
}
