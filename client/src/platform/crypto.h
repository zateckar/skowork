// Windows CNG (BCrypt) wrappers: randomness, hashing, and JWS signature verification with JWKs.
#pragma once

#include "base/encoding.h"
#include "base/json.h"
#include "base/jwt.h"
#include "base/result.h"

#include <string>
#include <string_view>

namespace wb {

bytes random_bytes(size_t n);
std::string random_urlsafe(size_t n_bytes);  // base64url of n random bytes
bytes sha256(std::string_view data);
bytes sha256(const bytes& data);
bytes hmac_sha256(const bytes& key, std::string_view data);

// AES-256-GCM with a 12-byte nonce; the sealed form is the ciphertext followed by the 16-byte tag.
// aes_gcm_open fails ("crypto") when the key, nonce, tag or associated data do not match.
Result<bytes> aes_gcm_seal(const bytes& key, const bytes& nonce, const std::uint8_t* plain, size_t n,
                           std::string_view aad);
Result<bytes> aes_gcm_open(const bytes& key, const bytes& nonce, const std::uint8_t* sealed, size_t n,
                           std::string_view aad);

// Verifies the JWT signature with the key from `jwks` ({"keys": [...]}) whose kid matches the token's kid
// (or the only key of a matching type when the token has no kid). Supports RS256 and ES256.
Status verify_jwt_signature(const Jwt& jwt, const json& jwks);

}  // namespace wb
