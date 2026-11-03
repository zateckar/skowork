#include "base/redact.h"

#include "base/strings.h"

#include <array>
#include <cctype>

namespace wb {
namespace {

bool is_token_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '-' || c == '_' || c == '+' || c == '/' || c == '=' || c == '.' || c == '~';
}

constexpr std::array<std::string_view, 13> kKeys = {
    "bearer ", "basic ", "access_token", "refresh_token", "id_token", "client_secret", "password",
    "api_key", "apikey", "authorization", "code_verifier", "\"code\"", "code=",
};

constexpr size_t kLongRun = 40;
constexpr std::string_view kMark = "[redacted]";

}  // namespace

std::string redact_secrets(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const std::string lower = to_lower(in);
    size_t i = 0;
    while (i < in.size()) {
        // 1. Keyed secrets: copy the key and its separators, then redact the value.
        bool keyed = false;
        for (const auto key : kKeys) {
            if (lower.compare(i, key.size(), key) != 0) continue;
            size_t j = i + key.size();
            out.append(in.substr(i, key.size()));
            // Skip separators such as `": "`, `=`, `:` and quotes.
            while (j < in.size() && (in[j] == '"' || in[j] == ':' || in[j] == '=' || in[j] == ' ' || in[j] == '\''))
                out += in[j++];
            // "Authorization: Bearer xyz": keep the scheme word, redact what follows it.
            if (key == "authorization") {
                const std::string_view rest = std::string_view(lower).substr(j);
                for (const std::string_view scheme : {std::string_view("bearer "), std::string_view("basic ")})
                    if (starts_with(rest, scheme)) {
                        out.append(in.substr(j, scheme.size()));
                        j += scheme.size();
                    }
            }
            const size_t start = j;
            while (j < in.size() && is_token_char(in[j])) ++j;
            if (j > start) out.append(kMark);
            i = j;
            keyed = true;
            break;
        }
        if (keyed) continue;
        // 2. Long token-shaped runs (JWTs, opaque tokens, keys).
        if (is_token_char(in[i]) && (i == 0 || !is_token_char(in[i - 1]))) {
            size_t j = i;
            while (j < in.size() && is_token_char(in[j])) ++j;
            const std::string_view run = in.substr(i, j - i);
            const bool jwt = starts_with(run, "eyJ") && run.find('.') != std::string_view::npos;
            // Paths and URLs contain '/', '.', '-' but rarely 40 unbroken characters without them; require
            // that most of the run is alphanumeric so that long file names are kept readable.
            size_t alnum = 0;
            for (const char c : run)
                if (std::isalnum(static_cast<unsigned char>(c))) ++alnum;
            if (jwt || (run.size() >= kLongRun && alnum * 10 >= run.size() * 9)) {
                out.append(kMark);
                i = j;
                continue;
            }
            out.append(run);
            i = j;
            continue;
        }
        out += in[i++];
    }
    return out;
}

bool looks_secret_bearing(std::string_view text) { return redact_secrets(text) != text; }

}  // namespace wb
