// OpenID Connect discovery and key sets for the configured issuer (client/identity Identity Provider
// Configuration, ID Token Verification And User Binding).
#pragma once

#include "base/json.h"
#include "base/jwt.h"
#include "base/result.h"
#include "base/time.h"

#include <mutex>
#include <string>

namespace wb {

struct OidcEndpoints {
    std::string issuer;
    std::string authorization;
    std::string token;
    std::string revocation;
    std::string end_session;
    std::string userinfo;
    std::string jwks;
};

class OidcProvider {
public:
    explicit OidcProvider(std::string issuer) : issuer_(std::move(issuer)) {}

    // Fetches the discovery document once (again after a failure). The document's issuer must equal the configured
    // issuer exactly.
    Result<OidcEndpoints> endpoints();

    // Verifies the ID token: signature (keys refetched once on an unknown kid), iss, aud == client_id, azp when
    // present, nonce, exp/iat with 60 s skew.
    Status verify_id_token(const std::string& id_token, const std::string& client_id, const std::string& nonce,
                           const Clock& clock, Jwt& out);

    // Parsing of the discovery document (exposed for tests).
    static Result<OidcEndpoints> parse_discovery(const json& doc, const std::string& expected_issuer);

private:
    Result<json> fetch_jwks(bool force);
    std::string issuer_;
    std::mutex mx_;
    bool have_endpoints_ = false;
    OidcEndpoints endpoints_;
    json jwks_;
};

}  // namespace wb
