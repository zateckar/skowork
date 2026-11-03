// JSON for every client module: nlohmann/json (add-agent-runtime design D1a).
// Untrusted input is parsed with parse_json(), which never throws.
#pragma once

#include <nlohmann/json.hpp>

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

inline long long jint(const json& j, const char* key, long long fallback = 0) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_number_integer() || it->is_number_unsigned()) return it->get<long long>();
    if (it->is_number_float()) return static_cast<long long>(it->get<double>());
    return fallback;
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
