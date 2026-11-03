#include "model/usage.h"

namespace wb::model {

void UsageLog::record(const UsageEntry& entry) {
    {
        const std::lock_guard lock(mx_);
        if (!entry.run_id.empty()) runs_[entry.run_id] += entry.usage;
        if (!entry.thread_id.empty()) threads_[entry.thread_id] += entry.usage;
        if (entry.run_id.empty() && !entry.feature.empty()) features_[entry.feature] += entry.usage;
        roles_[static_cast<int>(entry.role)] += entry.usage;
    }
    if (observer_) observer_(entry);
}

namespace {
Usage find(const std::map<std::string, Usage>& m, const std::string& key) {
    const auto it = m.find(key);
    return it == m.end() ? Usage{} : it->second;
}
}  // namespace

Usage UsageLog::run_total(const std::string& run_id) const {
    const std::lock_guard lock(mx_);
    return find(runs_, run_id);
}
Usage UsageLog::thread_total(const std::string& thread_id) const {
    const std::lock_guard lock(mx_);
    return find(threads_, thread_id);
}
Usage UsageLog::feature_total(const std::string& feature) const {
    const std::lock_guard lock(mx_);
    return find(features_, feature);
}
Usage UsageLog::role_total(Role role) const {
    const std::lock_guard lock(mx_);
    const auto it = roles_.find(static_cast<int>(role));
    return it == roles_.end() ? Usage{} : it->second;
}

}  // namespace wb::model
