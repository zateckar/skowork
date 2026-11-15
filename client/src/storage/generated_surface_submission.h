#pragma once

#include "storage/generated_surface_state.h"

namespace wb {

// A host-generated ticket binds one explicit button to one revision. Acceptance and enqueue happen together.
class GeneratedSurfaceSubmissionStore {
public:
    explicit GeneratedSurfaceSubmissionStore(Database& db) : db_(db), states_(db) {}
    Result<bool> accepted(const GeneratedSurfaceBinding& binding);
    Result<std::string> prepare(const GeneratedSurfaceBinding& binding, const std::string& button_id);
    Result<long long> submit(const std::string& ticket, const json& draft, const std::string& mode, bool online = true,
                             const json& sections = json::object());

private:
    Database& db_;
    GeneratedSurfaceStateStore states_;
};

}  // namespace wb
