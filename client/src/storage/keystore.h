// keys.bin: one random 256-bit key per database, each wrapped with DPAPI (CurrentUser + SID entropy)
// (add-client-foundation design D6).
#pragma once

#include "base/result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace wb {

using db_key = std::array<std::uint8_t, 32>;

// The name of the app.db key. Clients before the operation ledger read "app.db"; a change that makes running an older
// client on the profile unsafe moves the key to the next name (improve-proactive-durable-work, downgrade gate).
inline constexpr const char* kAppDbKey = "app.db#2";

class KeyStore {
public:
    explicit KeyStore(std::filesystem::path path) : path_(std::move(path)) {}

    Status load();
    Result<db_key> get(const std::string& name) const;
    [[nodiscard]] bool contains(const std::string& name) const { return blobs_.count(name) > 0; }
    Result<db_key> get_or_create(const std::string& name);
    Status remove(const std::string& name);
    // The key named `name`, adopting the key stored under `legacy` when there is none yet: the entry moves in one
    // atomic keys.bin replacement and `legacy` is gone afterwards, also when both exist. A client that still reads
    // `legacy` then gets a new random key that cannot open the database (improve-proactive-durable-work, downgrade
    // gate). Fails, leaving keys.bin unchanged, when the store cannot be written.
    Result<db_key> get_adopting(const std::string& name, const std::string& legacy);

private:
    Status save() const;
    std::filesystem::path path_;
    std::map<std::string, std::vector<std::uint8_t>> blobs_;
};

}  // namespace wb
