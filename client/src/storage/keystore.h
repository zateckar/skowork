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

class KeyStore {
public:
    explicit KeyStore(std::filesystem::path path) : path_(std::move(path)) {}

    Status load();
    Result<db_key> get(const std::string& name) const;
    Result<db_key> get_or_create(const std::string& name);
    Status remove(const std::string& name);

private:
    Status save() const;
    std::filesystem::path path_;
    std::map<std::string, std::vector<std::uint8_t>> blobs_;
};

}  // namespace wb
