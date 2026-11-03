// Usage accounting (client/model-access Usage Accounting): prompt, completion and reasoning tokens per model
// request, attributed to the Run, the Thread and the model role. Requests made outside a Run (embedding and rerank
// from a feature) are attributed to the feature.
#pragma once

#include "model/types.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wb::model {

struct UsageEntry {
    std::string run_id;
    std::string thread_id;
    std::string feature;  // for requests outside a Run
    Role role = Role::main;
    Usage usage;  // usage.requests is 1 per model request
};

class UsageLog {
public:
    void record(const UsageEntry& entry);

    [[nodiscard]] Usage run_total(const std::string& run_id) const;
    [[nodiscard]] Usage thread_total(const std::string& thread_id) const;
    [[nodiscard]] Usage feature_total(const std::string& feature) const;
    [[nodiscard]] Usage role_total(Role role) const;

    // Called for every recorded entry (the runtime persists it with the Run). Set before use.
    void set_observer(std::function<void(const UsageEntry&)> fn) { observer_ = std::move(fn); }

private:
    mutable std::mutex mx_;
    std::map<std::string, Usage> runs_, threads_, features_;
    std::map<int, Usage> roles_;
    std::function<void(const UsageEntry&)> observer_;
};

}  // namespace wb::model
