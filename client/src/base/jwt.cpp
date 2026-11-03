#include "base/jwt.h"

namespace wb {

bool Jwt::has_audience(std::string_view aud) const {
    const auto it = payload.find("aud");
    if (it == payload.end()) return false;
    if (it->is_string()) return it->get<std::string>() == aud;
    if (it->is_array())
        for (const auto& a : *it)
            if (a.is_string() && a.get<std::string>() == aud) return true;
    return false;
}

std::vector<std::string> Jwt::string_list(const char* claim) const {
    std::vector<std::string> out;
    const auto it = payload.find(claim);
    if (it == payload.end()) return out;
    if (it->is_string()) out.push_back(it->get<std::string>());
    if (it->is_array())
        for (const auto& a : *it)
            if (a.is_string()) out.push_back(a.get<std::string>());
    return out;
}

std::optional<Jwt> parse_jwt(std::string_view token) {
    if (token.size() > 64 * 1024) return std::nullopt;
    const auto d1 = token.find('.');
    if (d1 == std::string_view::npos) return std::nullopt;
    const auto d2 = token.find('.', d1 + 1);
    if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos) return std::nullopt;
    const auto h = base64url_decode(token.substr(0, d1));
    const auto p = base64url_decode(token.substr(d1 + 1, d2 - d1 - 1));
    const auto s = base64url_decode(token.substr(d2 + 1));
    if (!h || !p || !s) return std::nullopt;
    auto hj = parse_json(std::string_view(reinterpret_cast<const char*>(h->data()), h->size()));
    auto pj = parse_json(std::string_view(reinterpret_cast<const char*>(p->data()), p->size()));
    if (!hj || !pj || !hj->is_object() || !pj->is_object()) return std::nullopt;
    Jwt jwt;
    jwt.header = std::move(*hj);
    jwt.payload = std::move(*pj);
    jwt.signing_input = std::string(token.substr(0, d2));
    jwt.signature = std::move(*s);
    return jwt;
}

}  // namespace wb
