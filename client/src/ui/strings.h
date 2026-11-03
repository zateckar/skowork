// UI string resources (add-native-ui design D9): one JSON bundle per language (cs, de, en) under
// client/src/ui/strings/. A value is either a string or, for counted text, an object of plural forms. Parameters are
// written {name}; "{{" and "}}" are literal braces. English is the reference bundle: check_bundles() reports every
// key or parameter that another bundle lacks or adds, and the build runs it so a gap fails the build.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/result.h"

namespace wb::ui {

enum class Plural { one, few, other };

// The plural category of an integer count: Czech one (1), few (2-4), other; German and English one (1), other.
Plural plural_category(std::string_view lang, int64_t n);
// The categories a bundle of the language must provide for every counted entry.
std::vector<Plural> plural_categories(std::string_view lang);
const char* plural_name(Plural p);

using Params = std::vector<std::pair<std::string, std::u16string>>;

class Strings {
public:
    static Result<Strings> parse(std::string lang, std::string_view json);
    const std::string& lang() const { return lang_; }

    // The string with its parameters filled in. A missing key returns the key itself, so a gap is visible.
    std::u16string get(std::string_view key, const Params& params = {}) const;
    // The plural form for n; the parameter {n} is filled with the number.
    std::u16string count(std::string_view key, int64_t n, Params params = {}) const;
    bool has(std::string_view key) const { return entries_.find(std::string(key)) != entries_.end(); }

    struct Entry {
        std::map<std::string, std::string> forms;  // "" for a plain string, else the plural category names
    };
    const std::map<std::string, Entry, std::less<>>& entries() const { return entries_; }

private:
    std::string lang_;
    std::map<std::string, Entry, std::less<>> entries_;
};

// The {name} parameters of a template; malformed braces are reported as an error.
Result<std::set<std::string>> template_params(std::string_view tmpl);

// Checks the bundles against the reference language "en". Returns one line per problem; empty means consistent.
std::vector<std::string> check_bundles(const std::vector<Strings>& bundles);

}  // namespace wb::ui
