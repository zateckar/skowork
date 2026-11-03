#include "base/encoding.h"
#include "base/jwt.h"
#include "platform/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <gtest/gtest.h>

using namespace wb;

TEST(Crypto, Sha256Vector) {
    EXPECT_EQ(hex_encode(sha256(std::string_view("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, HmacSha256Rfc4231Case2) {
    const std::string k = "Jefe";
    EXPECT_EQ(hex_encode(hmac_sha256(bytes(k.begin(), k.end()), "what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(Crypto, RandomIsRandom) {
    EXPECT_EQ(random_bytes(32).size(), 32u);
    EXPECT_NE(random_urlsafe(32), random_urlsafe(32));
    EXPECT_EQ(random_urlsafe(32).size(), 43u);
}

namespace {

// Signs a JWT with a fresh P-256 key and returns {token, jwks}.
std::pair<std::string, json> es256_token(const json& payload, const std::string& kid) {
    BCRYPT_KEY_HANDLE key = nullptr;
    EXPECT_EQ(::BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0), 0);
    EXPECT_EQ(::BCryptFinalizeKeyPair(key, 0), 0);
    ULONG len = 0;
    ::BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &len, 0);
    std::vector<std::uint8_t> blob(len);
    ::BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), len, &len, 0);
    const auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    const std::uint8_t* xy = blob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
    const bytes x(xy, xy + hdr->cbKey), y(xy + hdr->cbKey, xy + 2 * hdr->cbKey);
    const std::string input = base64url_encode(json{{"alg", "ES256"}, {"kid", kid}, {"typ", "JWT"}}.dump()) + "." +
                              base64url_encode(payload.dump());
    const bytes digest = sha256(input);
    std::vector<std::uint8_t> sig(64);
    ULONG sig_len = 0;
    EXPECT_EQ(::BCryptSignHash(key, nullptr, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                               sig.data(), 64, &sig_len, 0),
              0);
    ::BCryptDestroyKey(key);
    const json jwks = {{"keys", json::array({{{"kty", "EC"}, {"crv", "P-256"}, {"kid", kid}, {"alg", "ES256"},
                                              {"x", base64url_encode(x)}, {"y", base64url_encode(y)}}})}};
    return {input + "." + base64url_encode(sig.data(), sig_len), jwks};
}

}  // namespace

TEST(Crypto, VerifiesEs256AndRejectsTampering) {
    auto [token, jwks] = es256_token({{"sub", "u1"}}, "k1");
    auto jwt = parse_jwt(token);
    ASSERT_TRUE(jwt);
    EXPECT_TRUE(verify_jwt_signature(*jwt, jwks));

    auto tampered = *jwt;
    tampered.signing_input[tampered.signing_input.size() - 2] ^= 1;
    EXPECT_FALSE(verify_jwt_signature(tampered, jwks));

    auto [other, other_jwks] = es256_token({{"sub", "u1"}}, "k1");
    EXPECT_FALSE(verify_jwt_signature(*jwt, other_jwks));  // same kid, different key

    auto unknown = verify_jwt_signature(*jwt, json{{"keys", json::array()}});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, "unknown_kid");
}

TEST(Crypto, RejectsAlgNone) {
    const std::string t = base64url_encode(std::string_view(R"({"alg":"none","kid":"k1"})")) + "." +
                          base64url_encode(std::string_view(R"({"sub":"u"})")) + ".";
    auto jwt = parse_jwt(t);
    if (jwt) EXPECT_FALSE(verify_jwt_signature(*jwt, json{{"keys", json::array()}}));
}
