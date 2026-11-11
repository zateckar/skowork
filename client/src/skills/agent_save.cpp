#include "skills/agent_save.h"

#include "base/strings.h"
#include "skills/catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace wb::skills {

namespace fs = std::filesystem;

namespace {

constexpr const char* kDefaultVersion = "1.0.0";
constexpr std::size_t kMaxChangedLines = 400;

std::optional<std::string> read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Status write_text(const fs::path& p, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return make_error("io", "cannot create " + path_utf8(p.parent_path()));
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return make_error("io", "cannot write " + path_utf8(p));
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return make_error("io", "cannot write " + path_utf8(p));
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return make_error("io", "cannot write " + path_utf8(p));
    }
    return {};
}

// A YAML double-quoted scalar, as the Skill editor's draft writes them.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        if (c < 0x80) {
            if (c == 0) return false;  // NUL marks binary content
            i += 1;
            continue;
        }
        if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

bool is_folder_path(const std::string& rel) {
    const auto slash = rel.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rel.size()) return false;
    const std::string top = rel.substr(0, slash);
    return std::any_of(std::begin(kSkillFolders), std::end(kSkillFolders), [&](const char* f) { return top == f; });
}

// The version of a stored SKILL.md, 0.0.0 when it cannot be read (design D5: the stored version must be lower).
std::string stored_version(const std::string& text) {
    auto parsed = parse_skill_md(text, false);
    if (const auto* p = std::get_if<Parsed>(&parsed)) return p->manifest.version;
    return "0.0.0";
}

// A run of characters that match `ok` from `pos`.
std::string_view run_from(std::string_view text, std::size_t pos, bool (*ok)(unsigned char)) {
    std::size_t e = pos;
    while (e < text.size() && ok(static_cast<unsigned char>(text[e]))) ++e;
    return text.substr(pos, e - pos);
}

bool token_char(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '+' || c == '/' || c == '=' || c == '.' || c == '~'; }
bool b64url_char(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }
bool opaque_char(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '+' || c == '/' || c == '='; }

bool has_digit_and_letter(std::string_view s) {
    const bool digit = std::any_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    const bool letter = std::any_of(s.begin(), s.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
    return digit && letter;
}

}  // namespace

// ---- arguments -----------------------------------------------------------------------------------------------------

Result<AgentSkillArgs> parse_agent_args(const json& args) {
    if (!args.is_object()) return make_error("invalid_params", "The arguments must be an object.");
    for (const char* key : {"path", "folder", "directory", "file_name", "location", "save_to", "destination"})
        if (args.contains(key))
            return make_error("invalid_params", std::string("The argument ") + key +
                                                    " is not allowed: a Skill needs no file path, the tool stores it.");
    AgentSkillArgs a;
    const auto text = [&](const char* key) -> Result<std::string> {
        if (!args.contains(key) || !args[key].is_string()) return make_error("invalid_params", std::string(key) + " must be text.");
        return args[key].get<std::string>();
    };
    auto name = text("name");
    if (!name) return name.error();
    auto description = text("description");
    if (!description) return description.error();
    auto instructions = text("instructions");
    if (!instructions) return instructions.error();
    a.name = trim(*name);
    a.description = trim(*description);
    a.instructions = *instructions;
    if (args.contains("version")) {
        if (!args["version"].is_string()) return make_error("invalid_params", "version must be text.");
        a.version = trim(args["version"].get<std::string>());
    }
    if (a.version.empty()) a.version = kDefaultVersion;
    if (args.contains("tools") && !args["tools"].is_null()) {
        if (!args["tools"].is_array()) return make_error("invalid_params", "tools must be a list of tool ids.");
        std::vector<std::string> tools;
        for (const auto& t : args["tools"]) {
            if (!t.is_string()) return make_error("invalid_params", "tools must be a list of tool ids.");
            tools.push_back(trim(t.get<std::string>()));
        }
        a.tools = std::move(tools);
    }
    if (args.contains("risk") && args["risk"].is_string()) a.risk = args["risk"].get<std::string>();
    if (args.contains("network") && args["network"].is_string()) a.network = args["network"].get<std::string>();
    if (args.contains("files") && !args["files"].is_null()) {
        if (!args["files"].is_array()) return make_error("invalid_params", "files must be a list of {path, content}.");
        for (const auto& f : args["files"]) {
            if (!f.is_object() || !f.contains("path") || !f["path"].is_string() || !f.contains("content") || !f["content"].is_string())
                return make_error("invalid_params", "each file needs a text path and a text content.");
            a.files.emplace_back(trim(f["path"].get<std::string>()), f["content"].get<std::string>());
        }
    }
    if (args.contains("update") && args["update"].is_boolean()) a.update = args["update"].get<bool>();
    return a;
}

// ---- secrets -------------------------------------------------------------------------------------------------------

bool has_secret(std::string_view text) {
    const std::string lo = to_lower(text);
    // 1. A keyed value: "api_key: ...", "password=...", "token": "...".
    constexpr std::array<std::string_view, 9> keys = {"api_key", "apikey", "api-key", "secret", "password", "passwd",
                                                      "access_token", "client_secret", "private_key"};
    for (const auto key : keys) {
        for (std::size_t p = lo.find(key); p != std::string::npos; p = lo.find(key, p + 1)) {
            std::size_t j = p + key.size();
            while (j < lo.size() && (lo[j] == '"' || lo[j] == '\'' || lo[j] == ' ')) ++j;
            if (j >= lo.size() || (lo[j] != ':' && lo[j] != '=')) continue;
            ++j;
            while (j < lo.size() && (lo[j] == '"' || lo[j] == '\'' || lo[j] == ' ')) ++j;
            const std::string_view value = run_from(text, j, token_char);
            if (value.size() >= 16 || (value.size() >= 8 && has_digit_and_letter(value))) return true;
        }
    }
    // 2. Well-known key shapes.
    const auto prefixed = [&](std::string_view prefix, std::size_t min_tail) {
        for (std::size_t p = lo.find(prefix); p != std::string::npos; p = lo.find(prefix, p + 1)) {
            const std::size_t start = p + prefix.size();
            std::size_t e = start;
            while (e < text.size() && b64url_char(static_cast<unsigned char>(text[e]))) ++e;
            if (e - start >= min_tail) return true;
        }
        return false;
    };
    if (prefixed("sk-", 20) || prefixed("ghp_", 30) || prefixed("github_pat_", 20) || prefixed("xoxb-", 10) ||
        prefixed("xoxp-", 10) || prefixed("xoxa-", 10) || prefixed("xoxs-", 10) || prefixed("xoxr-", 10) ||
        prefixed("aiza", 30))
        return true;
    if (text.find("AKIA") != std::string_view::npos) {
        for (std::size_t p = text.find("AKIA"); p != std::string_view::npos; p = text.find("AKIA", p + 1))
            if (run_from(text, p + 4, [](unsigned char c) { return std::isupper(c) || std::isdigit(c); }).size() >= 16) return true;
    }
    if (lo.find("private key-----") != std::string::npos && lo.find("begin") != std::string::npos) return true;
    // 3. A JSON Web Token: three base64url segments, the first two starting "eyJ" and the second with a dot.
    for (std::size_t p = text.find("eyJ"); p != std::string_view::npos; p = text.find("eyJ", p + 1)) {
        const auto seg1 = run_from(text, p, b64url_char);
        if (seg1.size() < 10 || p + seg1.size() >= text.size() || text[p + seg1.size()] != '.') continue;
        const auto seg2 = run_from(text, p + seg1.size() + 1, b64url_char);
        if (seg2.size() >= 10) return true;
    }
    // 4. An opaque run of 40 or more key characters with digits and letters.
    for (std::size_t p = 0; p < text.size();) {
        if (!opaque_char(static_cast<unsigned char>(text[p]))) {
            ++p;
            continue;
        }
        const auto run = run_from(text, p, opaque_char);
        if (run.size() >= 40 && has_digit_and_letter(run)) return true;
        p += run.size();
    }
    return false;
}

std::optional<std::string> secret_field(const AgentSkillArgs& a) {
    if (has_secret(a.description)) return "description";
    if (has_secret(a.instructions)) return "instructions";
    for (const auto& [path, content] : a.files)
        if (has_secret(content)) return "file " + path;
    return std::nullopt;
}

// ---- diff ----------------------------------------------------------------------------------------------------------

std::vector<std::string> changed_lines(std::string_view before, std::string_view after) {
    const auto split = [](std::string_view t) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= t.size()) {
            const std::size_t e = t.find('\n', pos);
            std::string line(t.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos));
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(std::move(line));
            if (e == std::string_view::npos) break;
            pos = e + 1;
        }
        return out;
    };
    const auto a = split(before), b = split(after);
    const std::size_t n = a.size(), m = b.size();
    // Longest common subsequence table; the Skill texts are small (at most 256 KB).
    std::vector<std::vector<std::uint32_t>> lcs(n + 1, std::vector<std::uint32_t>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    std::vector<std::string> out;
    std::size_t i = 0, j = 0;
    while ((i < n || j < m) && out.size() < kMaxChangedLines) {
        if (i < n && j < m && a[i] == b[j]) {
            ++i;
            ++j;
        } else if (i < n && (j >= m || lcs[i + 1][j] >= lcs[i][j + 1])) {
            out.push_back("- " + a[i++]);  // removals first, as a diff reads
        } else {
            out.push_back("+ " + b[j++]);
        }
    }
    return out;
}

// ---- prepare and commit --------------------------------------------------------------------------------------------

namespace {

// Builds SKILL.md from the arguments: the declared fields, then the instructions.
std::string skill_text(const AgentSkillArgs& a) {
    std::string fm = "name: " + quoted(a.name) + "\ndescription: " + quoted(a.description) + "\nversion: " + quoted(a.version) + "\n";
    if (a.tools) {
        fm += "tools:\n";
        for (const auto& t : *a.tools) fm += "  - " + quoted(t) + "\n";
    }
    if (a.risk) fm += "risk: " + quoted(*a.risk) + "\n";
    if (a.network) fm += "network: " + quoted(*a.network) + "\n";
    return join_skill_md({fm, a.instructions});
}

}  // namespace

Result<PreparedSave> prepare_agent_save(const AgentSkillEnv& env, const AgentSkillArgs& args) {
    if (!valid_name(args.name)) return make_error("invalid", "name: must be lower-case kebab-case, at most 64 characters.");
    if (auto field = secret_field(args))
        return make_error("secret", *field + " contains a key, token or password. Remove it and save again; the value is not shown.");

    // Resource files: text only, under scripts/, references/ or assets/, within the package limits (design D9).
    const PackageLimits limits;
    std::set<std::string> seen;
    std::uintmax_t total = 0;
    for (const auto& [path, content] : args.files) {
        if (!safe_relative_path(path) || !is_folder_path(path))
            return make_error("unsafe_path", "file " + path + ": the path must be under scripts/, references/ or assets/.");
        if (!seen.insert(path).second) return make_error("invalid", "file " + path + " is listed twice.");
        if (!valid_utf8(content)) return make_error("binary", "file " + path + " is not text. Only text files can be saved.");
        total += content.size();
    }
    if (args.files.size() + 1 > limits.max_entries) return make_error("too_large", "The Skill would have more files than the package limit.");

    PreparedSave p;
    p.args = args;
    if (p.args.version.empty()) p.args.version = kDefaultVersion;
    p.text = skill_text(p.args);
    if (p.text.size() > kMaxSkillMdBytes) return make_error("too_large", "SKILL.md is larger than 256 KB.");
    auto parsed = parse_skill_md(p.text, false);
    if (const auto* e = std::get_if<ParseError>(&parsed)) return make_error("invalid", e->field + ": " + e->message);
    total += p.text.size();
    if (total > limits.max_unpacked) return make_error("too_large", "The Skill would be larger than the package limit.");

    // Name collisions (design D5 and the spec's Name Collisions requirement).
    if (env.other_tier)
        if (auto tier = env.other_tier(args.name))
            return make_error("name_in_use", "The name " + args.name + " is already used by a Skill of the " + *tier +
                                                 " tier. Choose another name for the Skill.");
    std::error_code ec;
    const fs::path folder = env.private_dir / utf8_path(args.name);
    if (fs::is_directory(folder, ec)) {
        if (!env.created_by_agent || !env.created_by_agent(args.name))
            return make_error("name_in_use", "You wrote a private Skill named " + args.name +
                                                 ", so the agent cannot replace it. Choose another name for the Skill.");
        if (!args.update)
            return make_error("exists", "A Skill named " + args.name + " already exists. To change it, call again with update set to true and a higher version.");
        const auto stored = read_text(folder / "SKILL.md");
        p.exists = true;
        p.stored_text = stored.value_or(std::string());
        if (compare_versions(p.args.version, stored_version(p.stored_text)) <= 0)
            return make_error("version", "The version must be higher than the stored version " + stored_version(p.stored_text) + " of " + args.name + ".");
    }

    // Personal data (design D8): email addresses, Windows and UNC paths and the user's names, in the description, the
    // instructions and the files. Each distinct value is one finding; nothing is replaced here.
    const std::vector<std::string> names = env.user_names ? env.user_names() : std::vector<std::string>{};
    std::vector<std::string> texts = {p.args.description, p.args.instructions};
    for (const auto& f : args.files) texts.push_back(f.second);
    for (const auto& text : texts) {
        const auto found = parameterise(text, names, p.findings);
        for (const auto& r : found.replaced) {
            const bool known = std::any_of(p.findings.begin(), p.findings.end(),
                                           [&](const Replacement& k) { return k.kind == r.kind && k.original == r.original; });
            if (!known) p.findings.push_back(r);
        }
    }
    return p;
}

json agent_save_preview(const PreparedSave& p) {
    json files = json::array();
    for (const auto& [path, content] : p.args.files) files.push_back({{"path", path}, {"size", content.size()}});
    json findings = json::array();
    for (const auto& r : p.findings) findings.push_back({{"kind", r.kind}, {"original", r.original}});
    json out = {{"name", p.args.name},
                {"version", p.args.version},
                {"description", p.args.description},
                {"instructions", p.args.instructions},
                {"tools", p.args.tools ? json(*p.args.tools) : json(nullptr)},
                {"risk", p.args.risk.value_or("read")},
                {"network", p.args.network.value_or("")},
                {"files", files},
                {"findings", findings},
                {"update", p.exists}};
    if (p.exists) out["changes"] = changed_lines(p.stored_text, p.text);
    return out;
}

Result<json> commit_agent_save(const AgentSkillEnv& env, const PreparedSave& p, const std::string& thread_id) {
    std::error_code ec;
    const std::string& name = p.args.name;
    const fs::path folder = env.private_dir / utf8_path(name);
    const fs::path kept = env.previous_dir / utf8_path(name);
    if (p.exists) {
        // The replaced version becomes the one kept version (design D12); an older kept version is dropped.
        fs::remove_all(kept, ec);
        fs::create_directories(env.previous_dir, ec);
        fs::copy(folder, kept, fs::copy_options::recursive, ec);
        if (ec) return make_error("io", "The previous version could not be kept: " + ec.message());
        for (const char* sub : kSkillFolders) fs::remove_all(folder / sub, ec);
    } else {
        fs::remove_all(kept, ec);  // a kept version of an earlier Skill with this name
    }
    fs::create_directories(folder, ec);
    if (ec) return make_error("io", "The Skill folder could not be created: " + ec.message());
    if (auto w = write_text(folder / "SKILL.md", p.text); !w) return w.error();
    for (const auto& [path, content] : p.args.files)
        if (auto w = write_text(folder / utf8_path(path), content); !w) return w.error();
    if (env.record) env.record(name, thread_id, !p.exists);
    if (env.reload) env.reload();
    return json{{"name", name}, {"status", p.exists ? "updated" : "created"}, {"version", p.args.version}, {"tier", "private"},
                {"previous_kept", p.exists}};
}

Status restore_previous(const AgentSkillEnv& env, const std::string& name) {
    if (!valid_name(name)) return make_error("invalid_params", "not a Skill name");
    std::error_code ec;
    const fs::path folder = env.private_dir / utf8_path(name);
    const fs::path kept = env.previous_dir / utf8_path(name);
    const fs::path swap = env.previous_dir / (utf8_path(name) += ".swap");
    if (!fs::is_directory(kept, ec)) return make_error("not_found", "No previous version of " + name + " is kept.");
    if (!fs::is_directory(folder, ec)) return make_error("not_found", "No private Skill named " + name + ".");
    fs::remove_all(swap, ec);
    fs::rename(folder, swap, ec);
    if (ec) return make_error("io", "The current version could not be moved: " + ec.message());
    fs::rename(kept, folder, ec);
    if (ec) {
        fs::rename(swap, folder, ec);  // put the current version back
        return make_error("io", "The previous version could not be restored.");
    }
    fs::rename(swap, kept, ec);
    if (ec) return make_error("io", "The replaced version could not be kept: " + ec.message());
    if (env.reload) env.reload();
    return {};
}

bool has_previous(const AgentSkillEnv& env, const std::string& name) {
    std::error_code ec;
    return valid_name(name) && fs::is_directory(env.previous_dir / utf8_path(name), ec);
}

Status forget_previous(const AgentSkillEnv& env, const std::string& name) {
    if (!valid_name(name)) return make_error("invalid_params", "not a Skill name");
    std::error_code ec;
    fs::remove_all(env.previous_dir / utf8_path(name), ec);
    if (ec) return make_error("io", "The previous version could not be deleted: " + ec.message());
    return {};
}

}  // namespace wb::skills
