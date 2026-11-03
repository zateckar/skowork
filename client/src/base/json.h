// JSON for every client module: nlohmann/json (add-agent-runtime design D1a).
// Untrusted input is parsed with parse_json(), which never throws.
#pragma once

#include <nlohmann/json.hpp>

#include <climits>
#include <optional>
#include <string>
#include <string_view>

namespace wb {

using json = nlohmann::json;

// Parses text; returns nullopt for malformed input (no exceptions for untrusted data).
inline std::optional<json> parse_json(std::string_view text) {
    json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

// Typed field accessors that tolerate missing keys and wrong types.
inline std::string jstr(const json& j, const char* key, std::string fallback = {}) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

// A JSON number as a long long (a fraction is cut); nullopt for any other value, or a number outside the range of long
// long, which has no conversion (the cast would be undefined).
inline std::optional<long long> json_int(const json& v) {
    if (v.is_number_unsigned()) {
        const auto u = v.get<unsigned long long>();
        return u <= static_cast<unsigned long long>(LLONG_MAX) ? std::optional<long long>(static_cast<long long>(u)) : std::nullopt;
    }
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) return static_cast<long long>(d);
    }
    return std::nullopt;
}

inline long long jint(const json& j, const char* key, long long fallback = 0) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return it == j.end() ? fallback : json_int(*it).value_or(fallback);
}

inline bool jbool(const json& j, const char* key, bool fallback = false) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

inline const json* jobj(const json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it != j.end() && (it->is_object() || it->is_array()) ? &*it : nullptr;
}

}  // namespace wb
