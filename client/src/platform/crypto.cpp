#include "platform/crypto.h"

#include "platform/win.h"

#include <bcrypt.h>

#include <cstring>

namespace wb {
namespace {

struct alg_traits {
    using type = BCRYPT_ALG_HANDLE;
    static BCRYPT_ALG_HANDLE invalid() noexcept { return nullptr; }
    static void close(BCRYPT_ALG_HANDLE h) noexcept { ::BCryptCloseAlgorithmProvider(h, 0); }
};
struct key_traits {
    using type = BCRYPT_KEY_HANDLE;
    static BCRYPT_KEY_HANDLE invalid() noexcept { return nullptr; }
    static void close(BCRYPT_KEY_HANDLE h) noexcept { ::BCryptDestroyKey(h); }
};
using unique_alg = unique_res<alg_traits>;
using unique_key = unique_res<key_traits>;

bytes hash(LPCWSTR alg, const bytes* key, const std::uint8_t* data, size_t n) {
    unique_alg a;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(a.put(), alg, nullptr, key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0)))
        return {};
    bytes out(32);
    const NTSTATUS st = ::BCryptHash(a.get(), key ? const_cast<std::uint8_t*>(key->data()) : nullptr,
                                     key ? static_cast<ULONG>(key->size()) : 0, const_cast<std::uint8_t*>(data),
                                     static_cast<ULONG>(n), out.data(), static_cast<ULONG>(out.size()));
    if (!BCRYPT_SUCCESS(st)) out.clear();
    return out;
}

const json* find_key(const json& jwks, const std::string& kid, const std::string& kty) {
    const json* keys = jobj(jwks, "keys");
    if (!keys || !keys->is_array()) return nullptr;
    const json* only = nullptr;
    int count = 0;
    for (const auto& k : *keys) {
        if (jstr(k, "kty") != kty) continue;
        const std::string use = jstr(k, "use");
        if (!use.empty() && use != "sig") continue;
        if (!kid.empty() && jstr(k, "kid") == kid) return &k;
        only = &k;
        ++count;
    }
    return kid.empty() && count == 1 ? only : nullptr;
}

Status verify_rs256(const Jwt& jwt, const json& jwk) {
    const auto n = base64url_decode(jstr(jwk, "n"));
    const auto e = base64url_decode(jstr(jwk, "e"));
    if (!n || !e || n->empty() || e->empty()) return make_error("jwks", "bad RSA key");
    bytes blob(sizeof(BCRYPT_RSAKEY_BLOB));
    auto* hdr = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    hdr->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    hdr->BitLength = static_cast<ULONG>(n->size() * 8);
    hdr->cbPublicExp = static_cast<ULONG>(e->size());
    hdr->cbModulus = static_cast<ULONG>(n->size());
    blob.insert(blob.end(), e->begin(), e->end());
    blob.insert(blob.end(), n->begin(), n->end());
    unique_alg alg;
    unique_key key;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(alg.put(), BCRYPT_RSA_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(::BCryptImportKeyPair(alg.get(), nullptr, BCRYPT_RSAPUBLIC_BLOB, key.put(), blob.data(),
                                              static_cast<ULONG>(blob.size()), 0)))
        return make_error("jwks", "cannot import RSA key");
    bytes digest = sha256(jwt.signing_input);
    BCRYPT_PKCS1_PADDING_INFO pad{BCRYPT_SHA256_ALGORITHM};
    const NTSTATUS st = ::BCryptVerifySignature(key.get(), &pad, digest.data(), static_cast<ULONG>(digest.size()),
                                                const_cast<std::uint8_t*>(jwt.signature.data()),
                                                static_cast<ULONG>(jwt.signature.size()), BCRYPT_PAD_PKCS1);
    if (!BCRYPT_SUCCESS(st)) return make_error("signature", "RS256 signature invalid");
    return {};
}

Status verify_es256(const Jwt& jwt, const json& jwk) {
    if (jstr(jwk, "crv") != "P-256") return make_error("jwks", "unsupported curve");
    const auto x = base64url_decode(jstr(jwk, "x"));
    const auto y = base64url_decode(jstr(jwk, "y"));
    if (!x || !y || x->size() != 32 || y->size() != 32) return make_error("jwks", "bad EC key");
    if (jwt.signature.size() != 64) return make_error("signature", "ES256 signature length");
    bytes blob(sizeof(BCRYPT_ECCKEY_BLOB));
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    blob.insert(blob.end(), x->begin(), x->end());
    blob.insert(blob.end(), y->begin(), y->end());
    unique_alg alg;
    unique_key key;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(alg.put(), BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(::BCryptImportKeyPair(alg.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, key.put(), blob.data(),
                                              static_cast<ULONG>(blob.size()), 0)))
        return make_error("jwks", "cannot import EC key");
    bytes digest = sha256(jwt.signing_input);
    const NTSTATUS st = ::BCryptVerifySignature(key.get(), nullptr, digest.data(), static_cast<ULONG>(digest.size()),
                                                const_cast<std::uint8_t*>(jwt.signature.data()),
                                                static_cast<ULONG>(jwt.signature.size()), 0);
    if (!BCRYPT_SUCCESS(st)) return make_error("signature", "ES256 signature invalid");
    return {};
}

}  // namespace

bytes random_bytes(size_t n) {
    bytes out(n);
    if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        out.clear();
    return out;
}

std::string random_urlsafe(size_t n_bytes) { return base64url_encode(random_bytes(n_bytes)); }

bytes sha256(std::string_view data) {
    return hash(BCRYPT_SHA256_ALGORITHM, nullptr, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

bytes sha256(const bytes& data) { return hash(BCRYPT_SHA256_ALGORITHM, nullptr, data.data(), data.size()); }

bytes hmac_sha256(const bytes& key, std::string_view data) {
    return hash(BCRYPT_SHA256_ALGORITHM, &key, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

namespace {

Result<bytes> aes_gcm(bool seal, const bytes& key, const bytes& nonce, const std::uint8_t* in, size_t n,
                      std::string_view aad) {
    constexpr size_t kTag = 16;
    if (key.size() != 32 || nonce.size() != 12) return make_error("crypto", "bad AES-GCM key or nonce");
    if (!seal && n < kTag) return make_error("crypto", "sealed data too short");
    unique_alg alg;
    unique_key k;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(alg.put(), BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(::BCryptSetProperty(alg.get(), BCRYPT_CHAINING_MODE,
                                            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) ||
        !BCRYPT_SUCCESS(::BCryptGenerateSymmetricKey(alg.get(), k.put(), nullptr, 0,
                                                     const_cast<std::uint8_t*>(key.data()), 32, 0)))
        return make_error("crypto", "cannot set up AES-GCM");
    const size_t body = seal ? n : n - kTag;
    bytes out(body + (seal ? kTag : 0));
    std::uint8_t tag[kTag] = {};
    if (!seal) std::memcpy(tag, in + body, kTag);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<std::uint8_t*>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag;
    info.cbTag = kTag;
    ULONG written = 0;
    std::uint8_t dummy = 0;
    PUCHAR src = body ? const_cast<std::uint8_t*>(in) : &dummy;
    PUCHAR dst = body ? out.data() : &dummy;
    const NTSTATUS st = seal ? ::BCryptEncrypt(k.get(), src, static_cast<ULONG>(body), &info, nullptr, 0, dst,
                                               static_cast<ULONG>(body), &written, 0)
                             : ::BCryptDecrypt(k.get(), src, static_cast<ULONG>(body), &info, nullptr, 0, dst,
                                               static_cast<ULONG>(body), &written, 0);
    if (!BCRYPT_SUCCESS(st)) return make_error("crypto", seal ? "AES-GCM seal failed" : "AES-GCM authentication failed");
    if (seal) std::memcpy(out.data() + body, tag, kTag);
    return out;
}

}  // namespace

Result<bytes> aes_gcm_seal(const bytes& key, const bytes& nonce, const std::uint8_t* plain, size_t n,
                           std::string_view aad) {
    return aes_gcm(true, key, nonce, plain, n, aad);
}

Result<bytes> aes_gcm_open(const bytes& key, const bytes& nonce, const std::uint8_t* sealed, size_t n,
                           std::string_view aad) {
    return aes_gcm(false, key, nonce, sealed, n, aad);
}

Status verify_jwt_signature(const Jwt& jwt, const json& jwks) {
    const std::string alg = jwt.alg();
    if (alg == "RS256") {
        const json* k = find_key(jwks, jwt.kid(), "RSA");
        if (!k) return make_error("unknown_kid", "no RSA key for kid");
        return verify_rs256(jwt, *k);
    }
    if (alg == "ES256") {
        const json* k = find_key(jwks, jwt.kid(), "EC");
        if (!k) return make_error("unknown_kid", "no EC key for kid");
        return verify_es256(jwt, *k);
    }
    return make_error("alg", "unsupported algorithm " + alg);
}

}  // namespace wb
