#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb {

struct Url {
    std::string scheme;  // lower case
    std::string host;    // lower case, without brackets for IPv6
    int port = 0;        // explicit or scheme default
    std::string path;    // starts with '/', or empty for opaque URIs
    std::string query;   // without '?'
    std::string fragment;

    [[nodiscard]] bool is_loopback() const;
    [[nodiscard]] std::string origin() const;          // scheme://host[:port]
    [[nodiscard]] std::string path_and_query() const;  // path[?query]
};

// Parses absolute URLs (http, https, ws, wss) and private-use URIs like "com.example.workbench:/oauth2redirect?..."
std::optional<Url> parse_url(std::string_view s);

std::string url_encode(std::string_view s);  // RFC 3986 unreserved kept, everything else %XX
std::string url_decode(std::string_view s);  // also maps '+' to ' ' (form encoding)
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);
std::multimap<std::string, std::string> parse_query(std::string_view query);

// Joins a base URL and a relative path ("https://x/a/" + "b" -> "https://x/a/b").
std::string join_url(std::string_view base, std::string_view path);

}  // namespace wb
