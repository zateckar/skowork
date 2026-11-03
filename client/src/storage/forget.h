// Forget-cascade registry (client/local-storage Forget Data): each feature registers handlers for the entity types
// it derives data from, so deleting a Thread, a Source or an item also deletes indexes, embeddings, facts and
// caches.
#pragma once

#include "base/result.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wb {

class ForgetRegistry {
public:
    using Handler = std::function<Status(const std::string& id)>;

    // entity_type: "thread", "source", "item" (feature-defined ids).
    void on_forget(const std::string& entity_type, std::string feature, Handler handler);

    // Runs every handler registered for the type; all handlers run even if one fails, and the first error is
    // returned.
    Status forget(const std::string& entity_type, const std::string& id) const;

    [[nodiscard]] std::vector<std::string> features_for(const std::string& entity_type) const;

private:
    struct Entry {
        std::string feature;
        Handler handler;
    };
    mutable std::mutex mx_;
    std::map<std::string, std::vector<Entry>> handlers_;
};

}  // namespace wb
