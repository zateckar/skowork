#include "ui/strings.h"

#include <nlohmann/json.hpp>

#include "ui/bundles.h"
#include "ui/text16.h"

namespace wb::ui {

Plural plural_category(std::string_view lang, int64_t n) {
    const int64_t a = n < 0 ? -n : n;
    if (a == 1) return Plural::one;
    if (lang == "cs" && a >= 2 && a <= 4) return Plural::few;
    return Plural::other;
}

std::vector<Plural> plural_categories(std::string_view lang) {
    if (lang == "cs") return {Plural::one, Plural::few, Plural::other};
    return {Plural::one, Plural::other};
}

const char* plural_name(Plural p) {
    switch (p) {
    case Plural::one: return "one";
    case Plural::few: return "few";
    default: return "other";
    }
}

Result<std::set<std::string>> template_params(std::string_view t) {
    std::set<std::string> out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '{') {
            if (i + 1 < t.size() && t[i + 1] == '{') {
                ++i;
                continue;
            }
            const size_t e = t.find('}', i);
            if (e == std::string_view::npos) return make_error("strings", "unclosed '{'");
            const std::string_view name = t.substr(i + 1, e - i - 1);
            if (name.empty()) return make_error("strings", "empty parameter");
            for (char c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    return make_error("strings", "invalid parameter name '" + std::string(name) + "'");
            out.emplace(name);
            i = e;
        } else if (t[i] == '}') {
            if (i + 1 < t.size() && t[i + 1] == '}') {
                ++i;
                continue;
            }
            return make_error("strings", "unmatched '}'");
        }
    }
    return out;
}

namespace {

std::u16string fill(std::string_view t, const Params& params) {
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if ((c == '{' || c == '}') && i + 1 < t.size() && t[i + 1] == c) {
            out += c;
            ++i;
        } else if (c == '{') {
            const size_t e = t.find('}', i);
            if (e == std::string_view::npos) {
                out += t.substr(i);
                break;
            }
            const std::string_view name = t.substr(i + 1, e - i - 1);
            bool found = false;
            for (const auto& [k, v] : params) {
                if (k == name) {
                    out += to_utf8(v);
                    found = true;
                    break;
                }
            }
            if (!found) out += t.substr(i, e - i + 1);  // left visible
            i = e;
        } else {
            out += c;
        }
    }
    return to_utf16(out);
}

}  // namespace

Result<Strings> Strings::parse(std::string lang, std::string_view json) {
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return make_error("strings", lang + ": not a JSON object");
    Strings s;
    s.lang_ = std::move(lang);
    for (auto it = j.begin(); it != j.end(); ++it) {
        Entry e;
        if (it.value().is_string()) {
            e.forms[""] = it.value().get<std::string>();
        } else if (it.value().is_object()) {
            for (auto f = it.value().begin(); f != it.value().end(); ++f) {
                if (f.key() != "one" && f.key() != "few" && f.key() != "other")
                    return make_error("strings", s.lang_ + ": " + it.key() + ": unknown plural form '" + f.key() + "'");
                if (!f.value().is_string()) return make_error("strings", s.lang_ + ": " + it.key() + ": not a string");
                e.forms[f.key()] = f.value().get<std::string>();
            }
        } else {
            return make_error("strings", s.lang_ + ": " + it.key() + ": must be a string or plural forms");
        }
        s.entries_.emplace(it.key(), std::move(e));
    }
    return s;
}

std::u16string Strings::get(std::string_view key, const Params& params) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return to_utf16(key);
    const auto f = it->second.forms.find("");
    const std::string& t = f != it->second.forms.end() ? f->second : it->second.forms.begin()->second;
    return fill(t, params);
}

std::u16string Strings::count(std::string_view key, int64_t n, Params params) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return to_utf16(key);
    const auto& forms = it->second.forms;
    auto f = forms.find(plural_name(plural_category(lang_, n)));
    if (f == forms.end()) f = forms.find("other");
    if (f == forms.end()) f = forms.begin();
    params.emplace_back("n", to_utf16(std::to_string(n)));
    return fill(f->second, params);
}

std::string resolve_language(std::string_view setting, std::string_view system_tag) {
    for (std::string_view l : {"cs", "de", "en"})
        if (setting == l) return std::string(l);
    std::string base(system_tag.substr(0, system_tag.find_first_of("-_")));
    for (char& c : base)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (std::string_view l : {"cs", "de", "en"})
        if (base == l) return base;
    return "en";
}

Strings load_bundle(std::string_view lang) {
    const std::string l = resolve_language(lang, "");
    auto s = Strings::parse(l, bundle_json(l));
    return s.ok() ? std::move(s).value() : Strings();
}

std::vector<std::string> check_bundles(const std::vector<Strings>& bundles) {
    std::vector<std::string> issues;
    const Strings* ref = nullptr;
    for (const Strings& b : bundles)
        if (b.lang() == "en") ref = &b;
    if (!ref) return {"no reference bundle 'en'"};

    // The parameters of an entry: the union over its forms ({n} is implicit in plural forms).
    auto params_of = [&](const Strings& b, const std::string& key, const Strings::Entry& e, bool report) {
        std::set<std::string> all;
        for (const auto& [form, t] : e.forms) {
            auto p = template_params(t);
            if (!p) {
                if (report)
                    issues.push_back(b.lang() + ": " + key + (form.empty() ? "" : "." + form) + ": " + p.error().message);
                continue;
            }
            all.insert(p->begin(), p->end());
        }
        if (!e.forms.contains("")) all.erase("n");
        return all;
    };

    for (const Strings& b : bundles) {
        for (const auto& [key, e] : b.entries()) {
            const bool plural = !e.forms.contains("");
            if (plural) {
                for (Plural p : plural_categories(b.lang()))
                    if (!e.forms.contains(plural_name(p)))
                        issues.push_back(b.lang() + ": " + key + ": missing plural form '" + plural_name(p) + "'");
            }
            const auto rit = ref->entries().find(key);
            if (&b != ref && rit == ref->entries().end()) {
                issues.push_back(b.lang() + ": " + key + ": not in the reference bundle");
                continue;
            }
            const std::set<std::string> mine = params_of(b, key, e, true);
            if (&b == ref) continue;
            if (plural != !rit->second.forms.contains("")) {
                issues.push_back(b.lang() + ": " + key + ": plural in one language and plain in the other");
                continue;
            }
            const std::set<std::string> theirs = params_of(*ref, key, rit->second, false);
            for (const auto& p : theirs)
                if (!mine.contains(p)) issues.push_back(b.lang() + ": " + key + ": missing parameter {" + p + "}");
            for (const auto& p : mine)
                if (!theirs.contains(p)) issues.push_back(b.lang() + ": " + key + ": unknown parameter {" + p + "}");
        }
        if (&b == ref) continue;
        for (const auto& [key, e] : ref->entries())
            if (!b.entries().contains(key)) issues.push_back(b.lang() + ": " + key + ": missing");
    }
    return issues;
}

}  // namespace wb::ui
