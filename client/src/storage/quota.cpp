#include "storage/quota.h"

#include "base/strings.h"

namespace wb {
namespace {

std::string category_of(const std::filesystem::path& rel) {
    const std::string first = to_lower(rel.begin() != rel.end() ? rel.begin()->string() : "");
    const std::string ext = to_lower(rel.extension().string());
    if (first == "kg.db" || starts_with(first, "kg.db")) return "knowledge";
    if (ext == ".db" || starts_with(first, "app.db") || starts_with(first, "telemetry-buffer.db") || ext == ".bak" ||
        ext == ".db-wal" || ext == ".db-shm")
        return "databases";
    if (first == "logs") return "logs";
    if (first == "cache") return "cache";
    if (first == "skills") return "skills";
    if (first == "browser") return "browser";
    if (first == "workspace") return "workspace";
    if (first == "attachments") return "attachments";
    return "other";
}

}  // namespace

StorageUsage measure_storage(const std::filesystem::path& root) {
    StorageUsage u;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto size = static_cast<std::uint64_t>(it->file_size(ec));
        if (ec) continue;
        u.total += size;
        u.by_category[category_of(std::filesystem::relative(it->path(), root, ec))] += size;
    }
    return u;
}

QuotaState StorageQuota::check(const std::filesystem::path& root, const std::function<void()>& clear_caches,
                               StorageUsage& usage) {
    usage = measure_storage(root);
    const auto threshold = max_bytes_ / 10 * 9;
    if (usage.total >= threshold && clear_caches) {
        clear_caches();
        usage = measure_storage(root);
    }
    if (usage.total >= max_bytes_) state_ = QuotaState::exceeded;
    else if (usage.total >= threshold) state_ = QuotaState::warning;
    else state_ = QuotaState::normal;
    return state_;
}

}  // namespace wb
