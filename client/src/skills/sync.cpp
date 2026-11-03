#include "skills/sync.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace wb::skills {
namespace fs = std::filesystem;

namespace {

constexpr Tier kRegistryTiers[] = {Tier::org, Tier::group, Tier::shared};

std::optional<std::string> read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_digest(const std::string& d) {
    return d.size() == 64 && std::all_of(d.begin(), d.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::vector<std::string> strings_of(const json* a) {
    std::vector<std::string> out;
    if (a && a->is_array())
        for (const auto& x : *a)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

std::vector<std::string> installed_versions(const fs::path& name_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(name_dir, ec)) return out;
    for (fs::directory_iterator it(name_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string v = path_utf8(it->path().filename());
        if (valid_version(v)) out.push_back(v);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return compare_versions(a, b) < 0; });
    return out;
}

std::string pinned_of(const fs::path& name_dir) {
    auto s = read_file(name_dir / "pinned").value_or("");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

json read_json_file(const fs::path& file) {
    const auto text = read_file(file);
    if (!text) return nullptr;
    return parse_json(*text).value_or(json(nullptr));
}

bool write_file_atomic(const fs::path& file, std::string_view data) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

SkillSync::SkillSync(SyncEnv env) : env_(std::move(env)) {}

// ---- policy ---------------------------------------------------------------------------------------------------------

bool SkillSync::tier_allowed(Tier tier, const std::vector<std::string>& groups) const {
    std::vector<std::string> from = {"org", "group", "shared"};
    if (env_.install_from) {
        try {
            from = env_.install_from();
        } catch (...) {
            from.clear();  // fail closed
        }
    }
    const auto has = [&](const std::string& s) { return std::find(from.begin(), from.end(), s) != from.end(); };
    switch (tier) {
    case Tier::org: return has("org");
    case Tier::shared: return has("shared");
    case Tier::group:
        if (has("group")) return true;
        for (const auto& g : groups)
            if (has("group:" + g)) return true;
        return false;
    default: return false;
    }
}

bool SkillSync::allowed(const Listed& l) const {
    const auto t = parse_tier(l.scope);
    return t && tier_allowed(*t, l.groups);
}

// ---- signatures -----------------------------------------------------------------------------------------------------

Result<json> SkillSync::verify_signature(const std::string& jws, bool allow_fetch) {
    if (!env_.verify_jws) return make_error("signature", "no signature check is available");
    const fs::path keys_file = env_.data_dir / "keys.json";
    const json keys = read_json_file(keys_file);
    if (keys.is_object() && keys.contains("keys"))
        if (auto r = env_.verify_jws(jws, keys)) return r;
    if (!allow_fetch || !env_.fetch_keys) return make_error("signature", "the registry signature does not verify");
    // An unknown key id: fetch the published keys once (key rotation).
    auto fetched = env_.fetch_keys();
    if (!fetched) return make_error("keys", "the registry keys are unavailable: " + fetched.error().message);
    if (!fetched->is_object() || !fetched->contains("keys")) return make_error("keys", "the registry key set is malformed");
    write_file_atomic(keys_file, fetched->dump());
    auto r = env_.verify_jws(jws, *fetched);
    if (!r) return make_error("signature", "the registry signature does not verify");
    return r;
}

// ---- installation ---------------------------------------------------------------------------------------------------

Status SkillSync::install(const Listed& l) {
    const Tier tier = *parse_tier(l.scope);
    const auto refuse = [this, &l](const std::string& kind, const std::string& msg) -> Status {
        if (!kind.empty() && env_.security_event) env_.security_event(kind, l.name + " " + l.version + ": " + msg);
        return make_error("refused", msg);
    };
    if (!env_.fetch_content || !env_.sha256_hex) return make_error("unavailable", "the registry cannot be reached");
    auto bytes = env_.fetch_content(l.digest);
    if (!bytes) return bytes.error();
    if (env_.sha256_hex(*bytes) != l.digest) return refuse("skill.digest.mismatch", "the content does not match its digest");
    if (tier == Tier::org) {
        // Org Skills load only with a valid registry signature over exactly this content.
        if (l.signature.empty()) return refuse("skill.signature.missing", "the org Skill has no registry signature");
        auto payload = verify_signature(l.signature, true);
        if (!payload) {
            if (payload.error().code == "keys") return payload.error();
            return refuse("skill.signature.invalid", "the registry signature does not verify");
        }
        if (jstr(*payload, "name") != l.name || jstr(*payload, "version") != l.version || jstr(*payload, "digest") != l.digest ||
            jstr(*payload, "scope") != "org")
            return refuse("skill.signature.mismatch", "the registry signature is for other content");
    }
    auto files = read_package(*bytes, env_.limits);
    if (!files) return refuse("", "the package is invalid: " + files.error().message);
    const auto it = files->find("SKILL.md");
    auto parsed = parse_skill_md(it == files->end() ? std::string_view{} : std::string_view(it->second), true);
    if (const auto* e = std::get_if<ParseError>(&parsed)) return refuse("", "SKILL.md is invalid: " + e->field + ": " + e->message);
    const Manifest& m = std::get<Parsed>(parsed).manifest;
    if (m.name != l.name || m.version != l.version) return refuse("", "SKILL.md names another Skill or version");

    const fs::path name_dir = env_.data_dir / to_string(tier) / utf8_path(l.name);
    const fs::path tmp = name_dir / utf8_path(".tmp-" + l.version);
    const fs::path dir = name_dir / utf8_path(l.version);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    for (const auto& [rel, data] : *files)
        if (!write_file_atomic(tmp / utf8_path(rel), data)) {
            fs::remove_all(tmp, ec);
            return make_error("io", "the Skill could not be written");
        }
    std::string source = l.author;
    if (tier == Tier::group) {
        source.clear();
        for (const auto& g : l.groups) source += (source.empty() ? "" : ", ") + g;
    }
    const json record = {{"name", l.name}, {"version", l.version}, {"scope", l.scope}, {"digest", l.digest}, {"signature", l.signature},
                         {"groups", l.groups}, {"author", l.author}, {"source", source}};
    if (!write_file_atomic(tmp / "package.zip", *bytes) || !write_file_atomic(tmp / "install.json", record.dump(2))) {
        fs::remove_all(tmp, ec);
        return make_error("io", "the Skill could not be written");
    }
    fs::remove_all(dir, ec);
    fs::rename(tmp, dir, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        return make_error("io", "the Skill could not be installed");
    }
    return {};
}

std::optional<ParseError> SkillSync::verify_installed(Tier tier, const fs::path& folder) {
    const std::lock_guard lock(mx_);
    const auto fail = [this, &folder](const std::string& msg) -> std::optional<ParseError> {
        if (env_.security_event) env_.security_event("skill.integrity", path_utf8(folder) + ": " + msg);
        return ParseError{"signature", msg};
    };
    const json rec = read_json_file(folder / "install.json");
    if (!rec.is_object()) return fail("The install record is missing.");
    const auto pkg = read_file(folder / "package.zip");
    if (!pkg || !env_.sha256_hex || env_.sha256_hex(*pkg) != jstr(rec, "digest")) return fail("The package does not match its digest.");
    auto files = read_package(*pkg, env_.limits);
    if (!files) return fail("The package cannot be read.");
    // Every file on disk is the package's, unchanged, and nothing was added.
    std::set<std::string> on_disk;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = path_utf8(fs::relative(it->path(), folder, ec));
        if (rel == "package.zip" || rel == "install.json") continue;
        on_disk.insert(rel);
        const auto f = files->find(rel);
        if (f == files->end() || read_file(it->path()) != f->second) return fail("The file " + rel + " was changed or added.");
    }
    if (on_disk.size() != files->size()) return fail("A file of the package is missing.");
    if (tier == Tier::org) {
        auto payload = verify_signature(jstr(rec, "signature"), true);
        if (!payload) return fail("The registry signature does not verify.");
        const std::string name = path_utf8(folder.parent_path().filename());
        const std::string version = path_utf8(folder.filename());
        if (jstr(*payload, "name") != name || jstr(*payload, "version") != version || jstr(*payload, "digest") != jstr(rec, "digest") ||
            jstr(*payload, "scope") != "org")
            return fail("The registry signature is for other content.");
    }
    return std::nullopt;
}

// ---- removal --------------------------------------------------------------------------------------------------------

void SkillSync::remove_version(Tier tier, const std::string& name, const std::string& version, const std::string& reason, json& removed) {
    const fs::path name_dir = env_.data_dir / to_string(tier) / utf8_path(name);
    const std::string pinned = pinned_of(name_dir);
    json st = read_json_file(name_dir / "state.json");
    if (!st.is_object()) st = json::object();
    std::error_code ec;
    fs::remove_all(name_dir / utf8_path(version), ec);
    if (jstr(st, "previous") == version) st["previous"] = nullptr;
    if (const json* o = jobj(st, "offer"); o && jstr(*o, "version") == version) st["offer"] = nullptr;
    if (version == pinned || pinned.empty()) {
        // The version in use is gone: the previous one, else the highest left, else the Skill is removed.
        const auto left = installed_versions(name_dir);
        std::string next = jstr(st, "previous");
        if (next.empty() || std::find(left.begin(), left.end(), next) == left.end()) next = left.empty() ? "" : left.back();
        if (next.empty()) {
            fs::remove_all(name_dir, ec);
        } else {
            write_file_atomic(name_dir / "pinned", next);
            st["previous"] = nullptr;
            write_file_atomic(name_dir / "state.json", st.dump());
        }
        if (env_.stop_skill) env_.stop_skill(name, "the Skill " + name + " was revoked: " + reason);
    } else {
        write_file_atomic(name_dir / "state.json", st.dump());
    }
    removed.push_back({{"tier", to_string(tier)}, {"name", name}, {"version", version}, {"reason", reason}});
}

void SkillSync::revoke(const std::string& name, const json& versions, const std::string& reason) {
    const std::lock_guard lock(mx_);
    if (!valid_name(name) || !versions.is_array()) return;
    json removed = json::array();
    for (const Tier tier : kRegistryTiers) {
        const fs::path name_dir = env_.data_dir / to_string(tier) / utf8_path(name);
        for (const auto& v : versions) {
            const std::string version = jstr(v, "version"), digest = jstr(v, "digest");
            if (!valid_version(version)) continue;
            const fs::path dir = name_dir / utf8_path(version);
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            const json rec = read_json_file(dir / "install.json");
            if (!digest.empty() && rec.is_object() && jstr(rec, "digest") != digest) continue;  // another build of that version
            remove_version(tier, name, version, reason, removed);
        }
    }
    if (!removed.empty() && env_.changed) env_.changed();
}

// ---- sync -----------------------------------------------------------------------------------------------------------

Result<json> SkillSync::sync() {
    const std::lock_guard lock(mx_);
    flush_usage();
    if (!env_.fetch_list) return make_error("unavailable", "the registry cannot be reached");
    auto list = env_.fetch_list();
    if (!list) return list.error();
    json report = {{"installed", json::array()}, {"updated", json::array()}, {"offered", json::array()},
                   {"removed", json::array()}, {"refused", json::array()}};
    bool changed = false;

    // Revocations first, so that a missed skill-revoked push is caught up (Skill Revocation).
    if (const json* revs = jobj(*list, "revocations"); revs && revs->is_array())
        for (const auto& r : *revs) {
            const auto tier = parse_tier(jstr(r, "scope"));
            const std::string name = jstr(r, "name"), version = jstr(r, "version");
            if (!tier || *tier == Tier::builtin || *tier == Tier::priv || !valid_name(name) || !valid_version(version)) continue;
            const fs::path dir = env_.data_dir / to_string(*tier) / utf8_path(name) / utf8_path(version);
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            const json rec = read_json_file(dir / "install.json");
            if (rec.is_object() && jstr(rec, "digest") != jstr(r, "digest")) continue;
            remove_version(*tier, name, version, jstr(r, "reason", "revoked"), report["removed"]);
            changed = true;
        }

    // Sources policy no longer allows are removed (policy.skills.install_from).
    for (const Tier tier : kRegistryTiers) {
        std::error_code ec;
        const fs::path tier_dir = env_.data_dir / to_string(tier);
        if (!fs::is_directory(tier_dir, ec)) continue;
        std::vector<fs::path> names;
        for (fs::directory_iterator it(tier_dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) names.push_back(it->path());
        for (const auto& nd : names) {
            const json rec = read_json_file(nd / utf8_path(pinned_of(nd)) / "install.json");
            if (tier_allowed(tier, strings_of(rec.is_object() ? jobj(rec, "groups") : nullptr))) continue;
            fs::remove_all(nd, ec);
            report["removed"].push_back({{"tier", to_string(tier)}, {"name", path_utf8(nd.filename())}, {"reason", "not allowed by policy"}});
            changed = true;
        }
    }

    const bool automatic = !env_.auto_update || env_.auto_update();
    if (const json* skills = jobj(*list, "skills"); skills && skills->is_array())
        for (const auto& s : *skills) {
            Listed l{jstr(s, "name"), jstr(s, "version"), jstr(s, "scope"), jstr(s, "digest"), jstr(s, "signature"), jstr(s, "author"),
                     strings_of(jobj(s, "groups"))};
            const auto tier = parse_tier(l.scope);
            const auto refused = [&](const std::string& why) {
                report["refused"].push_back({{"name", l.name}, {"version", l.version}, {"scope", l.scope}, {"reason", why}});
            };
            if (!tier || *tier == Tier::builtin || *tier == Tier::priv || !valid_name(l.name) || !valid_version(l.version) ||
                !is_digest(l.digest)) {
                refused("the registry entry is malformed");
                continue;
            }
            if (!allowed(l)) continue;  // not a source policy installs from
            const fs::path name_dir = env_.data_dir / l.scope / utf8_path(l.name);
            const std::string current = pinned_of(name_dir);
            std::error_code ec;
            const bool present = !current.empty() && fs::is_directory(name_dir / utf8_path(current), ec);
            if (present && compare_versions(l.version, current) <= 0) continue;
            json st = read_json_file(name_dir / "state.json");
            if (!st.is_object()) st = json::object();
            const json entry = {{"name", l.name}, {"version", l.version}, {"scope", l.scope}};
            if (present && (jbool(st, "held") || !automatic)) {
                // Updates are offered when they are not automatic, or while the user pinned a version.
                const json offer = {{"version", l.version}, {"digest", l.digest}, {"signature", l.signature}, {"author", l.author},
                                    {"groups", l.groups}};
                if (st.value("offer", json()) != offer) {
                    st["offer"] = offer;
                    write_file_atomic(name_dir / "state.json", st.dump());
                    report["offered"].push_back(entry);
                    changed = true;
                }
                continue;
            }
            if (Status ins = install(l); !ins) {
                refused(ins.error().message);
                continue;
            }
            write_file_atomic(name_dir / "pinned", l.version);
            if (present) {
                // Keep the version replaced for rollback; older ones go.
                st["previous"] = current;
                for (const auto& v : installed_versions(name_dir))
                    if (v != l.version && v != current) fs::remove_all(name_dir / utf8_path(v), ec);
                report["updated"].push_back(entry);
            } else {
                st["previous"] = nullptr;
                count(l.name, "installs", 1);
                report["installed"].push_back(entry);
            }
            st["offer"] = nullptr;
            write_file_atomic(name_dir / "state.json", st.dump());
            changed = true;
        }
    if (changed && env_.changed) env_.changed();
    return report;
}

// ---- rollback and update --------------------------------------------------------------------------------------------

Status SkillSync::rollback(Tier tier, const std::string& name) {
    const std::lock_guard lock(mx_);
    if (!valid_name(name)) return make_error("not_found", "no such Skill");
    const fs::path name_dir = env_.data_dir / to_string(tier) / utf8_path(name);
    json st = read_json_file(name_dir / "state.json");
    if (!st.is_object()) st = json::object();
    const std::string previous = jstr(st, "previous"), current = pinned_of(name_dir);
    std::error_code ec;
    if (previous.empty() || !fs::is_directory(name_dir / utf8_path(previous), ec))
        return make_error("no_previous", "there is no previous version of " + name + " to roll back to");
    write_file_atomic(name_dir / "pinned", previous);
    st["previous"] = current;
    st["held"] = true;
    // The version rolled back from stays offered, so the user can update again.
    if (!jobj(st, "offer") || st["offer"].is_null()) {
        const json rec = read_json_file(name_dir / utf8_path(current) / "install.json");
        if (rec.is_object())
            st["offer"] = {{"version", current}, {"digest", jstr(rec, "digest")}, {"signature", jstr(rec, "signature")},
                           {"author", jstr(rec, "author")}, {"groups", rec.value("groups", json::array())}};
    }
    write_file_atomic(name_dir / "state.json", st.dump());
    if (env_.changed) env_.changed();
    return {};
}

Status SkillSync::update(Tier tier, const std::string& name) {
    const std::lock_guard lock(mx_);
    if (!valid_name(name)) return make_error("not_found", "no such Skill");
    const fs::path name_dir = env_.data_dir / to_string(tier) / utf8_path(name);
    json st = read_json_file(name_dir / "state.json");
    if (!st.is_object()) st = json::object();
    const std::string current = pinned_of(name_dir);
    std::string target;
    std::error_code ec;
    if (const json* o = jobj(st, "offer"); o && o->is_object()) {
        target = jstr(*o, "version");
        if (!fs::is_directory(name_dir / utf8_path(target), ec)) {
            Listed l{name, target, to_string(tier), jstr(*o, "digest"), jstr(*o, "signature"), jstr(*o, "author"),
                     strings_of(jobj(*o, "groups"))};
            if (!is_digest(l.digest)) return make_error("invalid", "the offered update is malformed");
            if (Status s = install(l); !s) return s;
        }
    } else {
        const auto v = installed_versions(name_dir);
        if (v.empty()) return make_error("not_found", "no such Skill");
        target = v.back();
    }
    write_file_atomic(name_dir / "pinned", target);
    if (target != current) st["previous"] = current;
    st["held"] = false;
    st["offer"] = nullptr;
    for (const auto& v : installed_versions(name_dir))
        if (v != target && v != current) fs::remove_all(name_dir / utf8_path(v), ec);
    write_file_atomic(name_dir / "state.json", st.dump());
    if (env_.changed) env_.changed();
    return {};
}

json SkillSync::state() {
    const std::lock_guard lock(mx_);
    json out = json::array();
    for (const Tier tier : kRegistryTiers) {
        std::error_code ec;
        const fs::path tier_dir = env_.data_dir / to_string(tier);
        if (!fs::is_directory(tier_dir, ec)) continue;
        for (fs::directory_iterator it(tier_dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const json st = read_json_file(it->path() / "state.json");
            json item = {{"tier", to_string(tier)}, {"name", path_utf8(it->path().filename())}, {"current", pinned_of(it->path())},
                         {"previous", nullptr}, {"held", false}, {"offer", nullptr}};
            if (st.is_object()) {
                item["previous"] = st.value("previous", json());
                item["held"] = jbool(st, "held");
                if (const json* o = jobj(st, "offer"); o && o->is_object()) item["offer"] = jstr(*o, "version");
            }
            out.push_back(std::move(item));
        }
    }
    return out;
}

// ---- usage ----------------------------------------------------------------------------------------------------------

void SkillSync::count(const std::string& name, const char* field, int n) {
    const fs::path file = env_.data_dir / "usage.json";
    json u = read_json_file(file);
    if (!u.is_object()) u = json::object();
    json& e = u[name];
    if (!e.is_object()) e = json::object();
    e[field] = jint(e, field) + n;
    write_file_atomic(file, u.dump());
}

void SkillSync::record_run(const std::string& name, bool ok) {
    const std::lock_guard lock(mx_);
    if (!valid_name(name)) return;
    bool registry = false;
    std::error_code ec;
    for (const Tier tier : kRegistryTiers)
        if (fs::is_directory(env_.data_dir / to_string(tier) / utf8_path(name), ec)) registry = true;
    if (!registry) return;  // private Skills are nobody's business
    count(name, "runs", 1);
    if (!ok) count(name, "failures", 1);
}

void SkillSync::flush_usage() {
    const fs::path file = env_.data_dir / "usage.json";
    const json u = read_json_file(file);
    if (!u.is_object() || u.empty() || !env_.post_usage) return;
    json items = json::array();
    for (const auto& [name, c] : u.items())
        items.push_back({{"name", name}, {"installs", jint(c, "installs")}, {"runs", jint(c, "runs")}, {"failures", jint(c, "failures")}});
    if (env_.post_usage({{"skills", items}})) {
        std::error_code ec;
        fs::remove(file, ec);
    }
}

}  // namespace wb::skills
