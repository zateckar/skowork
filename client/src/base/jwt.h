// JWT parsing without verification. Signature verification needs platform crypto and lives in
// platform/crypto.h; this part is portable so it can be fuzzed on the Linux job.
#pragma once

#include "base/encoding.h"
#include "base/json.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

struct Jwt {
    json header;
    json payload;
    std::string signing_input;  // "<b64 header>.<b64 payload>"
    bytes signature;

    [[nodiscard]] std::string alg() const { return jstr(header, "alg"); }
    [[nodiscard]] std::string kid() const { return jstr(header, "kid"); }
    [[nodiscard]] std::string iss() const { return jstr(payload, "iss"); }
    [[nodiscard]] std::string sub() const { return jstr(payload, "sub"); }
    [[nodiscard]] long long exp() const { return jint(payload, "exp"); }
    [[nodiscard]] long long iat() const { return jint(payload, "iat"); }
    // "aud" may be a string or an array.
    [[nodiscard]] bool has_audience(std::string_view aud) const;
    [[nodiscard]] std::vector<std::string> string_list(const char* claim) const;
};

std::optional<Jwt> parse_jwt(std::string_view token);

}  // namespace wb
