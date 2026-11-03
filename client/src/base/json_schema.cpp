#include "base/json_schema.h"

#include <cmath>

namespace wb {
namespace {

constexpr int kMaxSchemaDepth = 32;

class Validator {
public:
    explicit Validator(size_t max_errors) : max_(max_errors) {}
    std::vector<std::string> errors;

    void check(const json& schema, const json& v, const std::string& path, int depth) {
        if (full() || depth > kMaxSchemaDepth) return;
        if (schema.is_boolean()) {
            if (!schema.get<bool>()) add(path, "no value is allowed here");
            return;
        }
        if (!schema.is_object()) return;

        if (const auto t = schema.find("type"); t != schema.end()) {
            if (!type_ok(*t, v)) {
                add(path, "must be of type " + type_names(*t) + " but is " + type_of(v));
                return;  // the keywords below assume the type
            }
        }
        if (const auto e = schema.find("enum"); e != schema.end() && e->is_array()) {
            bool found = false;
            for (const auto& item : *e)
                if (item == v) found = true;
            if (!found) add(path, "must be one of " + e->dump());
        }
        if (const auto c = schema.find("const"); c != schema.end() && !(*c == v)) add(path, "must equal " + c->dump());

        if (v.is_number()) {
            const double d = v.get<double>();
            number_bound(schema, "minimum", path, d, [](double a, double b) { return a >= b; }, "at least");
            number_bound(schema, "maximum", path, d, [](double a, double b) { return a <= b; }, "at most");
            number_bound(schema, "exclusiveMinimum", path, d, [](double a, double b) { return a > b; }, "greater than");
            number_bound(schema, "exclusiveMaximum", path, d, [](double a, double b) { return a < b; }, "less than");
        }
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            long long len = 0;
            for (const char ch : s)
                if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++len;
            size_bound(schema, "minLength", path, len, true, "characters");
            size_bound(schema, "maxLength", path, len, false, "characters");
        }
        if (v.is_array()) {
            const auto n = static_cast<long long>(v.size());
            size_bound(schema, "minItems", path, n, true, "items");
            size_bound(schema, "maxItems", path, n, false, "items");
            if (const auto it = schema.find("items"); it != schema.end() && (it->is_object() || it->is_boolean())) {
                for (size_t i = 0; i < v.size() && !full(); ++i)
                    check(*it, v[i], path + "[" + std::to_string(i) + "]", depth + 1);
            }
        }
        if (v.is_object()) {
            if (const auto r = schema.find("required"); r != schema.end() && r->is_array()) {
                for (const auto& name : *r)
                    if (name.is_string() && !v.contains(name.get_ref<const std::string&>()))
                        add(path, "missing required property '" + name.get_ref<const std::string&>() + "'");
            }
            const auto props = schema.find("properties");
            const bool have_props = props != schema.end() && props->is_object();
            if (have_props) {
                for (const auto& [key, sub] : props->items()) {
                    const auto it = v.find(key);
                    if (it != v.end()) check(sub, *it, path + "." + key, depth + 1);
                }
            }
            if (const auto ap = schema.find("additionalProperties"); ap != schema.end()) {
                for (const auto& [key, val] : v.items()) {
                    if (have_props && props->contains(key)) continue;
                    if (ap->is_boolean()) {
                        if (!ap->get<bool>()) add(path, "unexpected property '" + key + "'");
                    } else if (ap->is_object()) {
                        check(*ap, val, path + "." + key, depth + 1);
                    }
                }
            }
        }
        if (const auto any = schema.find("anyOf"); any != schema.end() && any->is_array() && !any->empty()) {
            bool ok = false;
            for (const auto& sub : *any)
                if (matches(sub, v, depth + 1)) ok = true;
            if (!ok) add(path, "does not match any of the allowed forms");
        }
        if (const auto one = schema.find("oneOf"); one != schema.end() && one->is_array() && !one->empty()) {
            int count = 0;
            for (const auto& sub : *one)
                if (matches(sub, v, depth + 1)) ++count;
            if (count != 1) add(path, count == 0 ? "does not match any of the allowed forms" : "matches more than one form");
        }
    }

private:
    [[nodiscard]] bool full() const { return errors.size() >= max_; }
    void add(const std::string& path, const std::string& message) {
        if (!full()) errors.push_back(path + ": " + message);
    }

    bool matches(const json& schema, const json& v, int depth) const {
        Validator sub(1);
        sub.check(schema, v, "$", depth);
        return sub.errors.empty();
    }

    static std::string type_of(const json& v) {
        if (v.is_null()) return "null";
        if (v.is_boolean()) return "boolean";
        if (v.is_number_integer() || v.is_number_unsigned()) return "integer";
        if (v.is_number()) return "number";
        if (v.is_string()) return "string";
        if (v.is_array()) return "array";
        return "object";
    }

    static std::string type_names(const json& t) {
        if (t.is_string()) return t.get<std::string>();
        std::string out;
        if (t.is_array())
            for (const auto& e : t)
                if (e.is_string()) out += (out.empty() ? "" : " or ") + e.get<std::string>();
        return out.empty() ? "?" : out;
    }

    static bool one_type_ok(const std::string& name, const json& v) {
        if (name == "string") return v.is_string();
        if (name == "boolean") return v.is_boolean();
        if (name == "null") return v.is_null();
        if (name == "array") return v.is_array();
        if (name == "object") return v.is_object();
        if (name == "number") return v.is_number();
        if (name == "integer") {
            if (v.is_number_integer() || v.is_number_unsigned()) return true;
            if (v.is_number_float()) {
                const double d = v.get<double>();
                return std::isfinite(d) && d == std::floor(d);
            }
            return false;
        }
        return true;  // unknown type names do not restrict
    }

    static bool type_ok(const json& t, const json& v) {
        if (t.is_string()) return one_type_ok(t.get_ref<const std::string&>(), v);
        if (t.is_array()) {
            for (const auto& e : t)
                if (e.is_string() && one_type_ok(e.get_ref<const std::string&>(), v)) return true;
            return false;
        }
        return true;
    }

    template <typename Cmp>
    void number_bound(const json& schema, const char* key, const std::string& path, double d, Cmp ok, const char* word) {
        const auto it = schema.find(key);
        if (it == schema.end() || !it->is_number()) return;
        const double bound = it->get<double>();
        if (!ok(d, bound)) add(path, std::string("must be ") + word + " " + it->dump());
    }

    void size_bound(const json& schema, const char* key, const std::string& path, long long n, bool minimum,
                    const char* unit) {
        const auto it = schema.find(key);
        if (it == schema.end() || !it->is_number_integer()) return;
        const long long bound = it->get<long long>();
        if (minimum ? n < bound : n > bound)
            add(path, std::string("must have ") + (minimum ? "at least " : "at most ") + std::to_string(bound) + " " + unit);
    }

    size_t max_;
};

}  // namespace

std::vector<std::string> validate_json(const json& schema, const json& value, size_t max_errors) {
    Validator v(max_errors);
    v.check(schema, value, "$", 0);
    return std::move(v.errors);
}

}  // namespace wb
