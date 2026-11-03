#include "identity/oidc.h"

#include "base/url.h"
#include "platform/crypto.h"
#include "platform/http.h"

namespace wb {

Result<OidcEndpoints> OidcProvider::parse_discovery(const json& doc, const std::string& expected_issuer) {
    OidcEndpoints e;
    e.issuer = jstr(doc, "issuer");
    if (e.issuer != expected_issuer) return make_error("discovery", "issuer in discovery document does not match");
    e.authorization = jstr(doc, "authorization_endpoint");
    e.token = jstr(doc, "token_endpoint");
    e.revocation = jstr(doc, "revocation_endpoint");
    e.end_session = jstr(doc, "end_session_endpoint");
    e.userinfo = jstr(doc, "userinfo_endpoint");
    e.jwks = jstr(doc, "jwks_uri");
    for (const auto* u : {&e.authorization, &e.token, &e.jwks})
        if (u->empty() || !HttpClient::url_allowed(*u)) return make_error("discovery", "missing or insecure endpoint");
    for (const auto* u : {&e.revocation, &e.end_session, &e.userinfo})
        if (!u->empty() && !HttpClient::url_allowed(*u)) return make_error("discovery", "insecure endpoint");
    return e;
}

Result<OidcEndpoints> OidcProvider::endpoints() {
    const std::lock_guard lock(mx_);
    if (have_endpoints_) return endpoints_;
    HttpRequest req;
    req.url = join_url(issuer_, ".well-known/openid-configuration");
    req.headers = {{"Accept", "application/json"}};
    auto resp = http().send(req);
    if (!resp) return resp.error();
    if (resp->status != 200) return make_error("discovery", "discovery document HTTP " + std::to_string(resp->status));
    auto doc = parse_json(resp->body);
    if (!doc) return make_error("discovery", "discovery document is not JSON");
    auto e = parse_discovery(*doc, issuer_);
    if (!e) return e;
    endpoints_ = *e;
    have_endpoints_ = true;
    return endpoints_;
}

Result<json> OidcProvider::fetch_jwks(bool force) {
    auto ep = endpoints();
    if (!ep) return ep.error();
    const std::lock_guard lock(mx_);
    if (!force && !jwks_.is_null()) return jwks_;
    HttpRequest req;
    req.url = ep->jwks;
    auto resp = http().send(req);
    if (!resp) return resp.error();
    if (resp->status != 200) return make_error("jwks", "key set HTTP " + std::to_string(resp->status));
    auto doc = parse_json(resp->body);
    if (!doc || !doc->is_object()) return make_error("jwks", "key set is not JSON");
    jwks_ = std::move(*doc);
    return jwks_;
}

Status OidcProvider::verify_id_token(const std::string& id_token, const std::string& client_id,
                                     const std::string& nonce, const Clock& clock, Jwt& out) {
    auto jwt = parse_jwt(id_token);
    if (!jwt) return make_error("id_token", "malformed ID token");
    auto keys = fetch_jwks(false);
    if (!keys) return keys.error();
    Status sig = verify_jwt_signature(*jwt, *keys);
    if (!sig && sig.error().code == "unknown_kid") {
        keys = fetch_jwks(true);
        if (!keys) return keys.error();
        sig = verify_jwt_signature(*jwt, *keys);
    }
    if (!sig) return sig;
    if (jwt->iss() != issuer_) return make_error("id_token", "issuer mismatch");
    if (!jwt->has_audience(client_id)) return make_error("id_token", "audience mismatch");
    const std::string azp = jstr(jwt->payload, "azp");
    if (azp != client_id) return make_error("id_token", "authorized party mismatch");
    if (jstr(jwt->payload, "nonce") != nonce) return make_error("id_token", "nonce mismatch");
    const long long now = clock.unix_seconds();
    constexpr long long skew = 60;
    if (jwt->exp() == 0 || jwt->exp() + skew < now) return make_error("id_token", "ID token expired");
    if (jwt->iat() > now + skew) return make_error("id_token", "ID token issued in the future");
    if (jwt->sub().empty()) return make_error("id_token", "no subject");
    out = std::move(*jwt);
    return {};
}

}  // namespace wb
