#include "skills/skill.h"

#include "skills/yaml.h"

#include <algorithm>
#include <regex>
#include <set>

namespace wb::skills {
namespace {

ParseError err(std::string field, std::string message) { return {std::move(field), std::move(message)}; }

// A list field: a sequence of strings, or one comma-separated string.
std::optional<std::vector<std::string>> string_list(const json& v) {
    std::vector<std::string> out;
    if (v.is_null()) return out;
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        size_t p = 0;
        while (p <= s.size()) {
            size_t e = s.find(',', p);
            if (e == std::string::npos) e = s.size();
            std::string item = s.substr(p, e - p);
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (!item.empty()) out.push_back(item);
            p = e + 1;
        }
        return out;
    }
    if (!v.is_array()) return std::nullopt;
    for (const auto& item : v) {
        if (!item.is_string() || item.get<std::string>().empty()) return std::nullopt;
        out.push_back(item.get<std::string>());
    }
    return out;
}

std::string canonical_resolution(const std::string& r) {
    if (r == "approve") return "approve";
    if (r == "edit" || r == "approve_edited" || r == "approve-edited") return "approve_edited";
    if (r == "reject") return "reject";
    if (r == "respond" || r == "respond_instead" || r == "respond-instead") return "respond_instead";
    return {};
}

}  // namespace

bool valid_name(std::string_view name) {
    static const std::regex re("^[a-z0-9]+(-[a-z0-9]+)*$");
    return !name.empty() && name.size() <= 64 && std::regex_match(name.begin(), name.end(), re);
}

bool valid_version(std::string_view v) {
    static const std::regex re(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z.-]+)?$)");
    return v.size() <= 64 && std::regex_match(v.begin(), v.end(), re);
}

int compare_versions(std::string_view a, std::string_view b) {
    if (!valid_version(a) || !valid_version(b)) return 0;
    auto split = [](std::string_view v) {
        std::vector<long long> n;
        std::string pre;
        const size_t dash = v.find('-');
        const std::string_view core = v.substr(0, dash);
        if (dash != std::string_view::npos) pre = std::string(v.substr(dash + 1));
        size_t p = 0;
        for (int i = 0; i < 3; ++i) {
            const size_t e = core.find('.', p);
            n.push_back(std::stoll(std::string(core.substr(p, e - p))));
            p = e + 1;
        }
        return std::pair{n, pre};
    };
    const auto [x, xp] = split(a);
    const auto [y, yp] = split(b);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    if (xp == yp) return 0;
    if (xp.empty()) return 1;  // a release sorts after its pre-releases
    if (yp.empty()) return -1;
    return xp < yp ? -1 : 1;
}

int risk_rank(std::string_view r) {
    if (r == "read") return 0;
    if (r == "write-local") return 1;
    if (r == "external") return 2;
    if (r == "destructive") return 3;
    return -1;
}

bool tool_matches(std::string_view pattern, std::string_view tool) {
    // Iterative glob with `*` only.
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < tool.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (p < pattern.size() && pattern[p] == tool[t]) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::variant<Parsed, ParseError> parse_skill_md(std::string_view text, bool require_version) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    auto starts_line = [&](size_t at) { return text.substr(at, 3) == "---" && (at + 3 == text.size() || text[at + 3] == '\n' || text[at + 3] == '\r'); };
    if (!starts_line(0)) return err("frontmatter", "SKILL.md must start with a frontmatter block (---).");
    size_t first_nl = text.find('\n');
    if (first_nl == std::string_view::npos) return err("frontmatter", "The frontmatter block of SKILL.md is not closed with ---.");
    size_t close = std::string_view::npos;
    for (size_t p = first_nl + 1; p < text.size();) {
        if (starts_line(p)) {
            close = p;
            break;
        }
        const size_t nl = text.find('\n', p);
        if (nl == std::string_view::npos) break;
        p = nl + 1;
    }
    if (close == std::string_view::npos) return err("frontmatter", "The frontmatter block of SKILL.md is not closed with ---.");
    const auto fm = parse_yaml(text.substr(first_nl + 1, close - first_nl - 1));
    if (!fm) return err("frontmatter", "The frontmatter is not valid: " + fm.error().message + ".");

    Parsed out;
    size_t body = close + 3;
    if (body < text.size() && text[body] == '\r') ++body;
    if (body < text.size() && text[body] == '\n') ++body;
    out.body = std::string(text.substr(std::min(body, text.size())));
    Manifest& m = out.manifest;
    m.frontmatter = *fm;
    const json& f = m.frontmatter;

    auto scalar = [&](const char* key) -> std::optional<std::string> {
        if (!f.contains(key) || f[key].is_null()) return std::string();
        if (!f[key].is_string()) return std::nullopt;
        std::string s = f[key].get<std::string>();
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    };

    const auto name = scalar("name");
    if (!name || name->empty()) return err("name", "SKILL.md needs a name.");
    if (!valid_name(*name)) return err("name", "The name \"" + *name + "\" must be lower-case kebab-case, at most 64 characters.");
    m.name = *name;
    const auto desc = scalar("description");
    if (!desc || desc->empty()) return err("description", "SKILL.md needs a description.");
    if (desc->size() > 1024) return err("description", "The description is longer than 1024 characters.");
    m.description = *desc;
    const auto version = scalar("version");
    if (!version) return err("version", "The version must be a single value.");
    if (version->empty()) {
        if (require_version) return err("version", "SKILL.md needs a version (semantic, for example 1.0.0).");
    } else {
        if (!valid_version(*version)) return err("version", "The version \"" + *version + "\" is not a semantic version.");
        m.version = *version;
        m.version_declared = true;
    }

    if (f.contains("tools") && !f["tools"].is_null()) {
        auto t = string_list(f["tools"]);
        if (!t) return err("tools", "tools must be a list of tool ids.");
        m.tools = std::move(*t);
    }
    const auto risk = scalar("risk");
    if (!risk) return err("risk", "risk must be a single value.");
    if (!risk->empty()) {
        if (risk_rank(*risk) < 0) return err("risk", "risk must be one of read, write-local, external, destructive.");
        m.risk = *risk;
    }
    const auto network = scalar("network");
    if (!network) return err("network", "network must be a single value.");
    if (!network->empty()) {
        if (*network != "none" && *network != "proxy") return err("network", "network must be none or proxy.");
        m.network = *network;
    }
    if (f.contains("requires_mcp")) {
        auto r = string_list(f["requires_mcp"]);
        if (!r) return err("requires_mcp", "requires_mcp must be a list of MCP server ids.");
        m.requires_mcp = std::move(*r);
    }

    // interrupt_on: a map {tool: true | false | [resolutions] | {resolutions: [...]}} or a list of tool ids.
    if (f.contains("interrupt_on") && !f["interrupt_on"].is_null()) {
        const json& io = f["interrupt_on"];
        auto add = [&](const std::string& tool, const json& v) -> std::optional<ParseError> {
            if (tool.empty()) return err("interrupt_on", "interrupt_on names an empty tool.");
            if (v.is_boolean()) {
                // false would loosen a decision; a Skill cannot, so it is ignored (Skill Interrupt Rules).
                if (v.get<bool>()) m.interrupt_on.push_back({tool, {}});
                return std::nullopt;
            }
            const json* list = &v;
            if (v.is_object()) {
                list = v.contains("resolutions") ? &v["resolutions"] : v.contains("allowed_decisions") ? &v["allowed_decisions"] : nullptr;
                if (!list) return err("interrupt_on", "The rule for " + tool + " needs resolutions.");
            }
            auto names = string_list(*list);
            if (!names || names->empty()) return err("interrupt_on", "The rule for " + tool + " must list resolutions.");
            InterruptRule rule{tool, {}};
            for (const auto& n : *names) {
                const std::string c = canonical_resolution(n);
                if (c.empty()) return err("interrupt_on", "Unknown resolution \"" + n + "\" for " + tool + "; use approve, edit, reject or respond.");
                if (std::find(rule.resolutions.begin(), rule.resolutions.end(), c) == rule.resolutions.end()) rule.resolutions.push_back(c);
            }
            m.interrupt_on.push_back(std::move(rule));
            return std::nullopt;
        };
        if (io.is_object()) {
            for (const auto& [tool, v] : io.items())
                if (auto e = add(tool, v)) return *e;
        } else if (auto list = string_list(io)) {
            for (const auto& tool : *list)
                if (auto e = add(tool, true)) return *e;
        } else {
            return err("interrupt_on", "interrupt_on must map tool ids to true or to a list of resolutions.");
        }
    }

    if (f.contains("triggers") && !f["triggers"].is_null()) {
        const json& t = f["triggers"];
        m.triggers = t.is_array() ? t : json::array({t});
        for (const auto& item : m.triggers)
            if (!item.is_object() || !item.contains("kind") || !item["kind"].is_string())
                return err("triggers", "Each suggested trigger needs a kind (schedule, file, mail, calendar, meeting or session).");
    }
    return out;
}

const char* to_string(Tier t) {
    switch (t) {
    case Tier::builtin: return "builtin";
    case Tier::org: return "org";
    case Tier::group: return "group";
    case Tier::shared: return "shared";
    case Tier::priv: return "private";
    }
    return "private";
}

std::optional<Tier> parse_tier(std::string_view s) {
    for (Tier t : {Tier::builtin, Tier::org, Tier::group, Tier::shared, Tier::priv})
        if (s == to_string(t)) return t;
    return std::nullopt;
}

std::map<std::string, Entry> resolve_tiers(const std::vector<Loaded>& loaded, const std::vector<std::string>& locked) {
    std::map<std::string, std::vector<const Loaded*>> by_name;
    for (const auto& l : loaded) by_name[l.skill ? l.skill->manifest.name : l.name].push_back(&l);
    const std::set<std::string> lock(locked.begin(), locked.end());
    std::map<std::string, Entry> out;
    for (auto& [name, copies] : by_name) {
        // Highest precedence first; within a tier, the higher version first.
        std::stable_sort(copies.begin(), copies.end(), [](const Loaded* a, const Loaded* b) {
            if (a->tier != b->tier) return a->tier > b->tier;
            if (a->skill && b->skill) return compare_versions(a->skill->manifest.version, b->skill->manifest.version) > 0;
            return a->skill.has_value() && !b->skill.has_value();
        });
        Entry e;
        const Loaded* locked_org = nullptr;
        if (lock.count(name))
            for (const Loaded* c : copies)
                if (c->tier == Tier::org && c->skill) {
                    locked_org = c;
                    break;
                }
        e.active = locked_org;
        if (!e.active)
            for (const Loaded* c : copies)
                if (c->skill) {
                    e.active = c;
                    break;
                }
        for (const Loaded* c : copies) {
            if (c == e.active) continue;
            if (!c->skill) e.others.push_back({c, "invalid"});
            else if (locked_org && c->tier > Tier::org) e.others.push_back({c, "conflicting"});
            else e.others.push_back({c, "shadowed"});
        }
        out.emplace(name, std::move(e));
    }
    return out;
}

Scope tool_scope(const Manifest& m, const std::vector<ToolFacts>& tools) {
    Scope s;
    if (!m.tools) {
        for (const auto& t : tools)
            if (t.permitted && t.risk == "read") s.tools.push_back(t.id);
        return s;
    }
    std::set<std::string> granted;
    for (const auto& pattern : *m.tools) {
        bool any_permitted = false;
        bool any_known = false;
        for (const auto& t : tools) {
            if (!tool_matches(pattern, t.id)) continue;
            any_known = true;
            if (t.permitted) {
                any_permitted = true;
                granted.insert(t.id);
            }
        }
        if (!any_permitted || !any_known) s.unavailable.push_back(pattern);
    }
    for (const auto& t : tools)  // keep the registry's order
        if (granted.count(t.id)) s.tools.push_back(t.id);
    return s;
}

std::vector<std::string> allowed_resolutions(const Manifest& m, std::string_view tool) {
    std::optional<std::set<std::string>> allowed;
    for (const auto& r : m.interrupt_on) {
        if (r.resolutions.empty() || !tool_matches(r.tool, tool)) continue;
        std::set<std::string> these(r.resolutions.begin(), r.resolutions.end());
        if (!allowed) {
            allowed = these;
            continue;
        }
        std::set<std::string> both;
        for (const auto& x : *allowed)
            if (these.count(x)) both.insert(x);
        allowed = both;
    }
    std::vector<std::string> out;
    if (!allowed) return out;
    for (const char* r : kResolutions)
        if (allowed->count(r)) out.push_back(r);
    if (out.empty()) out.push_back("reject");  // contradictory rules: the call can only be rejected
    return out;
}

}  // namespace wb::skills
