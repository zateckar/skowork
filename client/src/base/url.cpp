#include "base/url.h"

#include "base/strings.h"

#include <cctype>
#include <charconv>

namespace wb {

bool Url::is_loopback() const {
    return host == "localhost" || host == "127.0.0.1" || host == "::1" || ends_with(host, ".localhost");
}

std::string Url::origin() const {
    std::string o = scheme + "://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host);
    const bool default_port = (port == 443 && (scheme == "https" || scheme == "wss")) ||
                              (port == 80 && (scheme == "http" || scheme == "ws"));
    if (!default_port && port > 0) o += ":" + std::to_string(port);
    return o;
}

std::string Url::path_and_query() const {
    std::string p = path.empty() ? "/" : path;
    if (!query.empty()) p += "?" + query;
    return p;
}

std::optional<Url> parse_url(std::string_view s) {
    Url u;
    const auto colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    for (const char c : s.substr(0, colon)) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return std::nullopt;
        u.scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string_view rest = s.substr(colon + 1);
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        u.fragment = std::string(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    if (const auto q = rest.find('?'); q != std::string_view::npos) {
        u.query = std::string(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }
    if (rest.substr(0, 2) == "//") {
        rest.remove_prefix(2);
        const auto slash = rest.find('/');
        std::string_view authority = rest.substr(0, slash);
        u.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
        if (authority.find('@') != std::string_view::npos) return std::nullopt;  // no userinfo in our URLs
        std::string_view port_part;
        if (!authority.empty() && authority.front() == '[') {
            const auto close = authority.find(']');
            if (close == std::string_view::npos) return std::nullopt;
            u.host = std::string(authority.substr(1, close - 1));
            if (close + 1 < authority.size()) {
                if (authority[close + 1] != ':') return std::nullopt;
                port_part = authority.substr(close + 2);
            }
        } else {
            const auto pc = authority.rfind(':');
            u.host = std::string(authority.substr(0, pc));
            if (pc != std::string_view::npos) port_part = authority.substr(pc + 1);
        }
        if (u.host.empty()) return std::nullopt;
        for (auto& c : u.host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!port_part.empty()) {
            int p = 0;
            const auto [ptr, ec] = std::from_chars(port_part.data(), port_part.data() + port_part.size(), p);
            if (ec != std::errc{} || ptr != port_part.data() + port_part.size() || p <= 0 || p > 65535)
                return std::nullopt;
            u.port = p;
        } else if (u.scheme == "https" || u.scheme == "wss") {
            u.port = 443;
        } else if (u.scheme == "http" || u.scheme == "ws") {
            u.port = 80;
        }
    } else {
        u.path = std::string(rest);  // opaque (private-use scheme) path, e.g. "/oauth2redirect"
    }
    return u;
}

std::string url_encode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string url_decode(std::string_view s) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = nibble(s[i + 1]);
            const int lo = nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) {
        if (!out.empty()) out += '&';
        out += url_encode(k) + "=" + url_encode(v);
    }
    return out;
}

std::multimap<std::string, std::string> parse_query(std::string_view query) {
    std::multimap<std::string, std::string> out;
    for (const auto& part : split(query, '&')) {
        if (part.empty()) continue;
        const auto eq = part.find('=');
        out.emplace(url_decode(part.substr(0, eq)), eq == std::string::npos ? "" : url_decode(part.substr(eq + 1)));
    }
    return out;
}

std::string join_url(std::string_view base, std::string_view path) {
    std::string b(base);
    while (!b.empty() && b.back() == '/') b.pop_back();
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    return b + "/" + std::string(path);
}

}  // namespace wb
