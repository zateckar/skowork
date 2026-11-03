#include "skills/authoring.h"

#include "skills/catalog.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace wb::skills {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Status write_file_atomic(const fs::path& p, std::string_view bytes) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return make_error("io", "cannot write " + path_utf8(p));
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return make_error("io", "cannot write " + path_utf8(p));
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return make_error("io", "cannot write " + path_utf8(p));
    }
    return {};
}

std::string_view trim_line_end(std::string_view l) {
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.remove_suffix(1);
    return l;
}

bool is_skill_folder(std::string_view s) {
    return std::any_of(std::begin(kSkillFolders), std::end(kSkillFolders), [&](const char* f) { return s == f; });
}

// Top-level files a package may carry besides SKILL.md.
bool package_top_file(const std::string& name) {
    std::string u = name;
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u.starts_with("README") || u.starts_with("LICENSE") || u.starts_with("LICENCE");
}

bool word_char(unsigned char c) { return std::isalnum(c) || c >= 0x80 || c == '_'; }

std::string yaml_quoted(const std::string& s) {
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

const char* kind_text(const std::string& kind) {
    return kind == "email" ? "an email address" : kind == "path" ? "a file or folder path" : "a person's name";
}

}  // namespace

// ---- SKILL.md in two parts ---------------------------------------------------------------------------------------

SkillSource split_skill_md(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    SkillSource s;
    const size_t first_end = text.find('\n');
    if (first_end == std::string_view::npos || trim_line_end(text.substr(0, first_end)) != "---") {
        s.body = std::string(text);
        return s;
    }
    size_t pos = first_end + 1;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        if (trim_line_end(text.substr(pos, end - pos)) == "---") {
            s.frontmatter = std::string(text.substr(first_end + 1, pos - first_end - 1));
            std::string_view rest = end < text.size() ? text.substr(end + 1) : std::string_view{};
            if (rest.starts_with("\r\n")) rest.remove_prefix(2);
            else if (rest.starts_with("\n")) rest.remove_prefix(1);
            s.body = std::string(rest);
            return s;
        }
        if (end == text.size()) break;
        pos = end + 1;
    }
    s.body = std::string(text);  // no closing line: not a frontmatter block
    return s;
}

std::string join_skill_md(const SkillSource& s) {
    std::string fm = s.frontmatter;
    if (!fm.empty() && fm.back() != '\n') fm += '\n';
    return "---\n" + fm + "---\n\n" + s.body;
}

// ---- private Skills on disk --------------------------------------------------------------------------------------

Result<std::string> save_private(const fs::path& private_dir, const std::string& original, const SkillSource& s) {
    const std::string text = join_skill_md(s);
    if (text.size() > kMaxSkillMdBytes) return make_error("too_large", "SKILL.md is larger than 256 KB.");
    auto parsed = parse_skill_md(text, false);
    if (const auto* e = std::get_if<ParseError>(&parsed)) return make_error("invalid", e->field + ": " + e->message);
    const std::string name = std::get<Parsed>(parsed).manifest.name;
    if (!original.empty() && !valid_name(original)) return make_error("invalid_params", "the original name is not valid");
    std::error_code ec;
    const fs::path target = private_dir / utf8_path(name);
    if (original != name) {
        if (fs::exists(target, ec)) return make_error("exists", "A private Skill named " + name + " already exists.");
        fs::create_directories(private_dir, ec);
        const fs::path old = private_dir / utf8_path(original);
        if (!original.empty() && fs::is_directory(old, ec)) {
            fs::rename(old, target, ec);
            if (ec) return make_error("io", "The Skill folder could not be renamed: " + ec.message());
        }
    }
    fs::create_directories(target, ec);
    if (ec) return make_error("io", "The Skill folder could not be created: " + ec.message());
    if (auto w = write_file_atomic(target / "SKILL.md", text); !w) return w.error();
    return name;
}

Status delete_private(const fs::path& private_dir, const std::string& name) {
    if (!valid_name(name)) return make_error("invalid_params", "not a Skill name");
    std::error_code ec;
    const fs::path folder = private_dir / utf8_path(name);
    if (!fs::is_directory(folder, ec)) return make_error("not_found", "No private Skill named " + name + ".");
    fs::remove_all(folder, ec);
    if (ec) return make_error("io", "The Skill could not be deleted: " + ec.message());
    return {};
}

std::vector<SkillFile> list_files(const fs::path& folder) {
    std::vector<SkillFile> out;
    std::error_code ec;
    for (const char* sub : kSkillFolders) {
        const fs::path dir = folder / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            out.push_back({path_utf8(it->path().lexically_relative(folder)), it->file_size(ec)});
        }
    }
    std::sort(out.begin(), out.end(), [](const SkillFile& a, const SkillFile& b) { return a.path < b.path; });
    return out;
}

Status add_files(const fs::path& folder, std::string_view sub, const std::vector<fs::path>& files, const PackageLimits& limits) {
    if (!is_skill_folder(sub)) return make_error("invalid_params", "files go to scripts, references or assets");
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return make_error("not_found", "Save the Skill before adding files.");
    const auto existing = list_files(folder);
    std::uintmax_t total = 0;
    std::set<std::string> present;
    for (const auto& f : existing) {
        total += f.size;
        present.insert(f.path);
    }
    std::size_t count = existing.size() + 1;  // SKILL.md
    std::vector<std::pair<fs::path, std::string>> plan;
    for (const auto& src : files) {
        if (!fs::is_regular_file(src, ec)) return make_error("not_found", path_utf8(src) + " is not a file.");
        const std::string rel = std::string(sub) + "/" + path_utf8(src.filename());
        if (!safe_relative_path(rel)) return make_error("unsafe_path", "The file name " + path_utf8(src.filename()) + " cannot be used.");
        const auto size = fs::file_size(src, ec);
        total += size;
        if (!present.count(rel)) ++count;
        if (total > limits.max_unpacked) return make_error("too_large", "The Skill would be larger than the package limit.");
        if (count > limits.max_entries) return make_error("too_large", "The Skill would have more files than the package limit.");
        plan.emplace_back(src, rel);
    }
    for (const auto& [src, rel] : plan) {
        const fs::path dst = folder / utf8_path(rel);
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return make_error("io", "Copying " + path_utf8(src.filename()) + " failed: " + ec.message());
    }
    return {};
}

Status remove_file(const fs::path& folder, std::string_view relative) {
    if (!safe_relative_path(relative) || !is_skill_folder(relative.substr(0, relative.find('/'))) ||
        relative.find('/') == std::string_view::npos)
        return make_error("invalid_params", "not a file of the Skill");
    std::error_code ec;
    const fs::path p = folder / utf8_path(relative);
    if (!fs::is_regular_file(p, ec)) return make_error("not_found", "No such file.");
    fs::remove(p, ec);
    if (ec) return make_error("io", "The file could not be removed: " + ec.message());
    return {};
}

Result<std::string> zip_folder(const fs::path& folder, const PackageLimits& limits) {
    std::error_code ec;
    std::vector<std::string> rels;
    if (!fs::is_regular_file(folder / "SKILL.md", ec)) return make_error("no_skill_md", "The Skill has no SKILL.md.");
    rels.push_back("SKILL.md");
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = path_utf8(it->path().filename());
        if (it->is_regular_file(ec) && package_top_file(n)) rels.push_back(n);
    }
    for (const auto& f : list_files(folder)) rels.push_back(f.path);
    if (rels.size() > limits.max_entries) return make_error("too_large", "The Skill has more files than the package limit.");
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return make_error("io", "the package cannot be created");
    std::size_t total = 0;
    for (const auto& rel : rels) {
        auto bytes = read_file(folder / utf8_path(rel));
        if (!bytes) {
            mz_zip_writer_end(&zip);
            return make_error("io", "cannot read " + rel);
        }
        total += bytes->size();
        if (total > limits.max_unpacked) {
            mz_zip_writer_end(&zip);
            return make_error("too_large", "The Skill is larger than the package limit.");
        }
        if (!mz_zip_writer_add_mem(&zip, rel.c_str(), bytes->data(), bytes->size(), MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return make_error("io", "cannot add " + rel);
        }
    }
    void* buf = nullptr;
    size_t size = 0;
    const bool ok = mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    std::string out = ok ? std::string(static_cast<const char*>(buf), size) : std::string();
    if (buf) mz_free(buf);
    mz_zip_writer_end(&zip);
    if (!ok) return make_error("io", "the package cannot be finished");
    if (out.size() > limits.max_bytes) return make_error("too_large", "The package is larger than the limit.");
    return out;
}

// ---- Save as skill --------------------------------------------------------------------------------------------------

namespace {

std::string param_for(std::vector<Replacement>& known, const std::string& kind, const std::string& value) {
    for (const auto& r : known)
        if (r.kind == kind && r.original == value) return r.parameter;
    const auto n = std::count_if(known.begin(), known.end(), [&](const Replacement& r) { return r.kind == kind; });
    Replacement r{kind, value, kind + "_" + std::to_string(n + 1)};
    known.push_back(r);
    return r.parameter;
}

std::string replace_regex(const std::string& text, const std::regex& re, const std::string& kind, std::vector<Replacement>& known,
                          const char* trailing = "") {
    std::string out;
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        std::string value = it->str();
        size_t pos = static_cast<size_t>(it->position());
        while (!value.empty() && std::string_view(trailing).find(value.back()) != std::string_view::npos) value.pop_back();
        if (value.empty()) continue;
        out += text.substr(last, pos - last);
        out += "{{" + param_for(known, kind, value) + "}}";
        last = pos + value.size();
    }
    out += text.substr(last);
    return out;
}

}  // namespace

std::vector<std::string> names_in(std::string_view text) {
    // Each <address>: the quoted display name before it, or up to four capitalised words.
    std::vector<std::string> out;
    const std::string_view s = text.substr(0, 256 * 1024);
    const auto capitalised = [](std::string_view w) {
        const auto c = static_cast<unsigned char>(w.front());
        return std::isupper(c) || c >= 0xC0;
    };
    for (size_t open = s.find('<'); open != std::string_view::npos; open = s.find('<', open + 1)) {
        const size_t close = s.find('>', open);
        if (close == std::string_view::npos) break;
        const std::string_view addr = s.substr(open + 1, close - open - 1);
        if (addr.find('@') == std::string_view::npos || addr.find_first_of(" \t\n<") != std::string_view::npos) continue;
        size_t e = open;
        while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
        std::string name;
        if (e > 1 && s[e - 1] == '"') {
            const size_t q = s.rfind('"', e - 2);
            if (q != std::string_view::npos && e - 2 - q >= 2 && e - 2 - q <= 60) name = std::string(s.substr(q + 1, e - 2 - q));
        } else {
            size_t first = e;
            for (int words = 0; words < 4; ++words) {
                size_t end = first;
                while (end > 0 && s[end - 1] == ' ') --end;
                size_t start = end;
                while (start > 0 && std::string_view(" \t\r\n,;:<>\"()[]{}").find(s[start - 1]) == std::string_view::npos) --start;
                if (start == end || !capitalised(s.substr(start, end - start))) break;
                first = start;
            }
            if (first < e) name = std::string(s.substr(first, e - first));
        }
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        if (name.size() < 2 || name.size() > 60 || name.find('@') != std::string::npos) continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

Parameterised parameterise(std::string_view input, const std::vector<std::string>& names, std::vector<Replacement> known) {
    static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})");
    // UNC (\\server\share\...) and drive (C:\...) paths, in text order.
    static const std::regex path(R"((\\\\[A-Za-z0-9._$-]+(\\[^\s"'<>|*?\\]+)+\\?)|([A-Za-z]:\\([^\s"'<>|*?\\]+\\?)*))");
    std::string text(input);
    text = replace_regex(text, email, "email", known);
    text = replace_regex(text, path, "path", known, ".,;:)");
    // Names: longest first, as whole words.
    std::vector<std::string> sorted;
    for (const auto& n : names)
        if (n.size() >= 2 && std::find(sorted.begin(), sorted.end(), n) == sorted.end()) sorted.push_back(n);
    std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& n : sorted) {
        std::string out;
        size_t pos = 0, last = 0;
        while ((pos = text.find(n, pos)) != std::string::npos) {
            const bool before_ok = pos == 0 || !word_char(static_cast<unsigned char>(text[pos - 1]));
            const size_t end = pos + n.size();
            const bool after_ok = end >= text.size() || !word_char(static_cast<unsigned char>(text[end]));
            if (before_ok && after_ok) {
                out += text.substr(last, pos - last);
                out += "{{" + param_for(known, "person", n) + "}}";
                last = end;
            }
            pos = end;
        }
        out += text.substr(last);
        text = std::move(out);
    }
    return {std::move(text), std::move(known)};
}

std::string kebab_name(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) && c < 0x80) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 64) {
        out.resize(64);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    return out.empty() ? "my-skill" : out;
}

Result<Draft> draft_skill(const ThreadDigest& thread, const DraftModel& model) {
    static const char* kSystem =
        "You turn a finished conversation between a user and an AI agent into a reusable Skill: instructions another agent "
        "follows to do the same kind of task again. Generalise: describe the steps, the tools and the inputs, not this "
        "one case. Replace people, email addresses, file paths, dates and other values that change between uses with "
        "{{parameter}} placeholders and list them as inputs. Do not include personal data. Answer with JSON only: "
        "{\"name\": \"kebab-case-name\", \"description\": \"one sentence saying what the Skill does and when to use it\", "
        "\"instructions\": \"Markdown steps\", \"inputs\": [{\"name\": \"parameter\", \"description\": \"what it is\"}]}";
    std::string user = "Title: " + thread.title + "\n";
    if (!thread.tools.empty()) {
        user += "Tools used:";
        for (const auto& t : thread.tools) user += " " + t;
        user += "\n";
    }
    user += "\nConversation:\n";
    constexpr size_t kPerMessage = 4000, kTotal = 60000;
    for (const auto& [role, content] : thread.messages) {
        std::string c = content.substr(0, kPerMessage);
        if (content.size() > kPerMessage) c += " [...]";
        if (user.size() + c.size() > kTotal) break;
        user += "[" + role + "] " + c + "\n\n";
    }
    auto answer = model ? model(kSystem, user) : Result<std::string>(make_error("unavailable", "no model"));
    if (!answer) return answer.error();
    const size_t open = answer->find('{'), close = answer->rfind('}');
    json j = json::parse(open != std::string::npos && close != std::string::npos && close > open ? answer->substr(open, close - open + 1) : std::string(),
                         nullptr, false);
    if (!j.is_object()) return make_error("draft_failed", "The model's draft could not be read. Try again.");
    const auto str = [&j](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };

    std::vector<std::string> names = thread.names;
    for (const auto& m : thread.messages)
        for (auto& n : names_in(m.second))
            if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(std::move(n));
    auto desc = parameterise(str("description"), names);
    auto body = parameterise(str("instructions"), names, desc.replaced);

    Draft d;
    d.replaced = body.replaced;
    std::string fm = "name: " + kebab_name(str("name").empty() ? thread.title : str("name")) + "\n";
    fm += "description: " + yaml_quoted(desc.text) + "\n";
    fm += "version: 0.1.0\n";
    fm += "risk: " + (risk_rank(thread.risk) >= 0 ? thread.risk : std::string("read")) + "\n";
    if (!thread.tools.empty()) {
        fm += "tools:\n";
        for (const auto& t : thread.tools) fm += "  - " + t + "\n";
    }
    d.source.frontmatter = fm;
    std::string text = body.text;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    std::string inputs;
    if (j.contains("inputs") && j["inputs"].is_array())
        for (const auto& in : j["inputs"]) {
            if (!in.is_object()) continue;
            std::string n = in.contains("name") && in["name"].is_string() ? in["name"].get<std::string>() : "";
            n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == '{' || c == '}' || c == '`'; }), n.end());
            if (n.empty() || n.size() > 64) continue;
            const std::string what = in.contains("description") && in["description"].is_string() ? in["description"].get<std::string>() : "";
            auto described = parameterise(what, names, d.replaced);
            d.replaced = described.replaced;
            inputs += "- `{{" + n + "}}`" + (what.empty() ? "" : ": " + described.text) + "\n";
        }
    for (const auto& r : d.replaced) inputs += "- `{{" + r.parameter + "}}`: " + kind_text(r.kind) + "\n";
    d.source.body = text + "\n" + (inputs.empty() ? "" : "\n## Inputs\n\n" + inputs);
    return d;
}

SkillSource keep_original(const SkillSource& s, const Replacement& r) {
    const std::string placeholder = "{{" + r.parameter + "}}";
    SkillSource out;
    // Body: drop the parameter's line from the Inputs list, then put the value back.
    std::istringstream body(s.body);
    std::string line, kept;
    while (std::getline(body, line)) {
        if (line.starts_with("- `" + placeholder + "`") || line.starts_with("- " + placeholder)) continue;
        kept += line + "\n";
    }
    if (!s.body.empty() && s.body.back() != '\n' && !kept.empty()) kept.pop_back();
    const auto replace_all = [](std::string text, const std::string& from, const std::string& to) {
        for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size()) text.replace(pos, from.size(), to);
        return text;
    };
    out.body = replace_all(kept, placeholder, r.original);
    // Frontmatter: inside a double-quoted scalar the value needs escaping.
    std::istringstream fm(s.frontmatter);
    std::string escaped = yaml_quoted(r.original);
    escaped = escaped.substr(1, escaped.size() - 2);
    while (std::getline(fm, line)) {
        const size_t at = line.find(placeholder);
        if (at != std::string::npos) line = replace_all(line, placeholder, line.substr(0, at).find('"') != std::string::npos ? escaped : r.original);
        out.frontmatter += line + "\n";
    }
    if (!s.frontmatter.empty() && s.frontmatter.back() != '\n' && !out.frontmatter.empty()) out.frontmatter.pop_back();
    return out;
}

}  // namespace wb::skills
