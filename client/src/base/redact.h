// Secret redaction for diagnostic logs (client/desktop-host Diagnostic Logs: no tokens or credentials).
#pragma once

#include <string>
#include <string_view>

namespace wb {

// Replaces token-shaped substrings with "[redacted]":
//  - JWTs (three base64url segments separated by dots, starting with "eyJ");
//  - values after "Bearer ", "Basic ", "access_token", "refresh_token", "id_token", "code", "client_secret",
//    "password", "api_key", "Authorization" and "Proxy-Authorization";
//  - any run of 40 or more base64/base64url/hex characters (opaque refresh tokens, keys).
std::string redact_secrets(std::string_view line);

// True if the text still contains something that looks like a secret (used by the log-scanning test).
bool looks_secret_bearing(std::string_view text);

}  // namespace wb
