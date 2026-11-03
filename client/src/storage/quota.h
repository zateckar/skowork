// Storage quota (client/local-storage Storage Quota): policy.storage.max_size_gb (default 10). At 90% the client
// clears expired caches, pauses new indexing and warns with a usage breakdown.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace wb {

struct StorageUsage {
    std::uint64_t total = 0;
    std::map<std::string, std::uint64_t> by_category;  // databases, knowledge, logs, cache, skills, browser, other
};

StorageUsage measure_storage(const std::filesystem::path& root);

enum class QuotaState { normal, warning, exceeded };

class StorageQuota {
public:
    explicit StorageQuota(std::uint64_t max_bytes) : max_bytes_(max_bytes) {}
    void set_max_bytes(std::uint64_t b) { max_bytes_ = b; }
    [[nodiscard]] std::uint64_t max_bytes() const { return max_bytes_; }

    // Measures the root; at >= 90% runs `clear_caches` once, re-measures, and reports the state.
    QuotaState check(const std::filesystem::path& root, const std::function<void()>& clear_caches, StorageUsage& usage);

    // True while new indexing must pause (state warning or exceeded at the last check).
    [[nodiscard]] bool indexing_paused() const { return state_ != QuotaState::normal; }
    [[nodiscard]] QuotaState state() const { return state_; }

private:
    std::uint64_t max_bytes_;
    QuotaState state_ = QuotaState::normal;
};

}  // namespace wb
