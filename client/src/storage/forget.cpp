#include "storage/forget.h"

namespace wb {

void ForgetRegistry::on_forget(const std::string& entity_type, std::string feature, Handler handler) {
    const std::lock_guard lock(mx_);
    handlers_[entity_type].push_back({std::move(feature), std::move(handler)});
}

Status ForgetRegistry::forget(const std::string& entity_type, const std::string& id) const {
    std::vector<Entry> entries;
    {
        const std::lock_guard lock(mx_);
        const auto it = handlers_.find(entity_type);
        if (it != handlers_.end()) entries = it->second;
    }
    Status first;
    for (const auto& e : entries) {
        Status s = e.handler(id);
        if (!s && first.ok()) first = make_error(s.error().code, e.feature + ": " + s.error().message);
    }
    return first;
}

std::vector<std::string> ForgetRegistry::features_for(const std::string& entity_type) const {
    const std::lock_guard lock(mx_);
    std::vector<std::string> out;
    const auto it = handlers_.find(entity_type);
    if (it != handlers_.end())
        for (const auto& e : it->second) out.push_back(e.feature);
    return out;
}

}  // namespace wb
