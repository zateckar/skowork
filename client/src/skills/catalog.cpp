#include "skills/catalog.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace wb::skills {
namespace fs = std::filesystem;

std::string path_utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path utf8_path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

namespace {

std::optional<std::string> read_small(const fs::path& file, std::size_t limit, std::string& why) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        why = "SKILL.md is missing.";
        return std::nullopt;
    }
    if (size > limit) {
        why = "SKILL.md is larger than " + std::to_string(limit / 1024) + " KB.";
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        why = "SKILL.md cannot be read.";
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<fs::path> subfolders(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

std::string trimmed_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string s;
    if (in) std::getline(in, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

Loaded load_folder(const fs::path& folder, Tier tier, const std::string& folder_name) {
    Loaded l;
    l.tier = tier;
    l.name = folder_name;
    l.folder = path_utf8(folder);
    std::string why;
    const auto text = read_small(folder / "SKILL.md", kMaxSkillMdBytes, why);
    if (!text) {
        l.error = ParseError{"SKILL.md", why};
        return l;
    }
    // The registry requires a version; private and built-in Skills may omit it (Agent Skills Compatibility).
    auto r = parse_skill_md(*text, tier == Tier::org || tier == Tier::group || tier == Tier::shared);
    if (auto* e = std::get_if<ParseError>(&r)) {
        l.error = std::move(*e);
        return l;
    }
    Parsed p = std::get<Parsed>(std::move(r));
    if (p.manifest.name != folder_name) {
        l.error = ParseError{"name", "The name \"" + p.manifest.name + "\" must match the folder name \"" + folder_name + "\"."};
        return l;
    }
    l.skill = std::move(p);
    return l;
}

Catalog::Catalog(CatalogEnv env) : env_(std::move(env)) { reload(); }

void Catalog::reload() {
    auto all = std::make_shared<std::vector<Loaded>>();
    const auto plain_tier = [&](const fs::path& dir, Tier tier) {
        for (const auto& f : subfolders(dir)) all->push_back(load_folder(f, tier, path_utf8(f.filename())));
    };
    if (!env_.builtin_dir.empty()) plain_tier(env_.builtin_dir, Tier::builtin);
    if (!env_.data_dir.empty()) {
        plain_tier(env_.data_dir / "private", Tier::priv);
        for (const Tier tier : {Tier::org, Tier::group, Tier::shared}) {
            for (const auto& named : subfolders(env_.data_dir / to_string(tier))) {
                const std::string name = path_utf8(named.filename());
                // The pinned version, else the highest installed one (Skill Installation And Pinning).
                std::string version = trimmed_file(named / "pinned");
                std::error_code ec;
                if (version.empty() || !fs::is_directory(named / utf8_path(version), ec)) {
                    version.clear();
                    for (const auto& v : subfolders(named)) {
                        const std::string vs = path_utf8(v.filename());
                        if (valid_version(vs) && (version.empty() || compare_versions(vs, version) > 0)) version = vs;
                    }
                }
                if (version.empty()) continue;
                const fs::path dir = named / utf8_path(version);
                Loaded l = load_folder(dir, tier, name);
                if (l.skill && l.skill->manifest.version != version)
                    l = Loaded{tier, name, path_utf8(dir), std::nullopt,
                               ParseError{"version", "The version in SKILL.md does not match the installed version " + version + "."}, ""};
                if (l.skill && env_.verify) {
                    std::optional<ParseError> bad;
                    try {
                        bad = env_.verify(tier, dir);
                    } catch (...) {
                        bad = ParseError{"signature", "The Skill could not be checked."};
                    }
                    if (bad) {
                        l.skill.reset();
                        l.error = std::move(bad);
                    }
                }
                {
                    std::ifstream in(dir / "install.json", std::ios::binary);
                    std::ostringstream ss;
                    if (in) {
                        ss << in.rdbuf();
                        if (auto j = parse_json(ss.str()); j && j->is_object()) l.source = jstr(*j, "source");
                    }
                }
                all->push_back(std::move(l));
            }
        }
    }
    const std::lock_guard lock(mx_);
    loaded_ = std::move(all);
}

std::vector<std::string> Catalog::locked() const {
    if (!env_.locked) return {};
    try {
        return env_.locked();
    } catch (...) {
        return {};
    }
}

std::optional<std::string> Catalog::unavailable(const Manifest& m) const {
    std::vector<std::string> missing;
    for (const auto& server : m.requires_mcp) {
        std::optional<std::string> why = std::string("the MCP client is not running");
        if (env_.mcp_unavailable) {
            try {
                why = env_.mcp_unavailable(server);
            } catch (...) {
            }
        }
        if (why) missing.push_back(server);
    }
    if (missing.empty()) return std::nullopt;
    std::string s = "requires";
    for (size_t i = 0; i < missing.size(); ++i) s += (i ? ", " : " ") + missing[i];
    return s;
}

json Catalog::list() const {
    std::shared_ptr<const std::vector<Loaded>> loaded;
    {
        const std::lock_guard lock(mx_);
        loaded = loaded_;
    }
    std::vector<ToolFacts> tools;
    if (env_.tools) {
        try {
            tools = env_.tools();
        } catch (...) {
        }
    }
    json out = json::array();
    const auto copy_json = [](const Loaded& l, const std::string& state) {
        json j = {{"tier", to_string(l.tier)}, {"folder", l.folder}, {"state", state}, {"source", l.source}};
        j["version"] = l.skill ? json(l.skill->manifest.version) : json(nullptr);
        j["error"] = l.error ? json{{"field", l.error->field}, {"message", l.error->message}} : json(nullptr);
        return j;
    };
    for (const auto& [name, entry] : resolve_tiers(*loaded, locked())) {
        json item = {{"name", name}};
        const Loaded* shown = entry.active ? entry.active : (entry.others.empty() ? nullptr : entry.others.front().skill);
        if (!shown) continue;
        item.update(copy_json(*shown, entry.active ? "active" : "invalid"));
        if (entry.active) {
            const Manifest& m = entry.active->skill->manifest;
            item["description"] = m.description;
            item["risk"] = m.risk;
            item["network"] = m.network;
            item["requires_mcp"] = m.requires_mcp;
            item["triggers"] = m.triggers;
            const Scope scope = tool_scope(m, tools);
            item["tools"] = scope.tools;
            item["unavailable_tools"] = scope.unavailable;
            const auto why = unavailable(m);
            item["unavailable"] = why ? json(*why) : json(nullptr);
            if (why) item["state"] = "unavailable";
        }
        json others = json::array();
        for (const auto& o : entry.others)
            if (o.skill != shown) others.push_back(copy_json(*o.skill, o.state));
        item["others"] = std::move(others);
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<Usable> Catalog::usable() const {
    std::shared_ptr<const std::vector<Loaded>> loaded;
    {
        const std::lock_guard lock(mx_);
        loaded = loaded_;
    }
    std::vector<Usable> out;
    for (const auto& [name, entry] : resolve_tiers(*loaded, locked())) {
        if (!entry.active || unavailable(entry.active->skill->manifest)) continue;
        out.push_back({*entry.active->skill, entry.active->tier, entry.active->folder});
    }
    return out;
}

std::optional<Usable> Catalog::find(const std::string& name) const {
    for (auto& u : usable())
        if (u.skill.manifest.name == name) return std::move(u);
    return std::nullopt;
}

}  // namespace wb::skills
