#include "storage/keystore.h"

#include "platform/crypto.h"
#include "platform/dpapi.h"
#include "platform/win.h"

#include <cstring>
#include <fstream>

namespace wb {
namespace {

constexpr char kMagic[4] = {'S', 'K', 'K', '1'};
constexpr wchar_t kPurpose[] = L"keys/v1";

template <typename T>
void put_le(std::vector<std::uint8_t>& out, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

template <typename T>
bool get_le(const std::vector<std::uint8_t>& in, size_t& pos, T& v) {
    if (in.size() - pos < sizeof(T)) return false;
    v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v = static_cast<T>(v | (static_cast<T>(in[pos + i]) << (8 * i)));
    pos += sizeof(T);
    return true;
}

}  // namespace

Status KeyStore::load() {
    blobs_.clear();
    std::ifstream f(path_, std::ios::binary);
    if (!f) return {};  // no store yet
    std::vector<std::uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (in.size() < 8 || std::memcmp(in.data(), kMagic, 4) != 0) return make_error("keystore", "keys.bin: bad header");
    size_t pos = 4;
    std::uint32_t count = 0;
    get_le(in, pos, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t name_len = 0;
        std::uint32_t blob_len = 0;
        if (!get_le(in, pos, name_len) || in.size() - pos < name_len) return make_error("keystore", "keys.bin: truncated");
        std::string name(reinterpret_cast<const char*>(in.data() + pos), name_len);
        pos += name_len;
        if (!get_le(in, pos, blob_len) || in.size() - pos < blob_len) return make_error("keystore", "keys.bin: truncated");
        blobs_[name].assign(in.begin() + static_cast<std::ptrdiff_t>(pos),
                            in.begin() + static_cast<std::ptrdiff_t>(pos + blob_len));
        pos += blob_len;
    }
    return {};
}

Status KeyStore::save() const {
    std::vector<std::uint8_t> out(kMagic, kMagic + 4);
    put_le(out, static_cast<std::uint32_t>(blobs_.size()));
    for (const auto& [name, blob] : blobs_) {
        put_le(out, static_cast<std::uint16_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        put_le(out, static_cast<std::uint32_t>(blob.size()));
        out.insert(out.end(), blob.begin(), blob.end());
    }
    auto tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!f) return make_error("keystore", "keys.bin: write failed");
    }
    if (!::MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return make_error("keystore", "keys.bin: replace failed: " + error_text(::GetLastError()));
    return {};
}

Result<db_key> KeyStore::get(const std::string& name) const {
    const auto it = blobs_.find(name);
    if (it == blobs_.end()) return make_error("not_found", "no key named " + name);
    auto plain = dpapi_unprotect(it->second, kPurpose);
    if (!plain) return plain.error();
    if (plain->size() != sizeof(db_key)) return make_error("keystore", "unwrapped key has unexpected length");
    db_key key{};
    std::memcpy(key.data(), plain->data(), key.size());
    ::SecureZeroMemory(plain->data(), plain->size());
    return key;
}

Result<db_key> KeyStore::get_or_create(const std::string& name) {
    if (blobs_.count(name)) return get(name);
    const auto rnd = random_bytes(sizeof(db_key));
    if (rnd.size() != sizeof(db_key)) return make_error("keystore", "BCryptGenRandom failed");
    auto blob = dpapi_protect(rnd, kPurpose);
    if (!blob) return blob.error();
    blobs_[name] = std::move(*blob);
    if (auto s = save(); !s) return s.error();
    db_key key{};
    std::memcpy(key.data(), rnd.data(), key.size());
    return key;
}

Status KeyStore::remove(const std::string& name) {
    blobs_.erase(name);
    return save();
}

}  // namespace wb
