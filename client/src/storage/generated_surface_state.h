#pragma once

#include <optional>
#include <string>

#include "base/json.h"
#include "storage/database.h"

namespace wb {

struct GeneratedSurfaceBinding {
    std::string thread_id, run_id, revision;
    long long event_seq = 0;
    int block_index = 0;
};

struct GeneratedQuestionBinding {
    std::string thread_id, run_id, action_id, revision, token;
};

// Local exploration and draft data are stored in encrypted app.db. This class does not accept submissions.
class GeneratedSurfaceStateStore {
public:
    explicit GeneratedSurfaceStateStore(Database& db) : db_(db) {}
    Result<GeneratedSurfaceBinding> bind_assistant(const std::string& thread_id, const std::string& run_id,
                                                    long long event_seq, int block_index);
    Result<json> assistant_surface(const GeneratedSurfaceBinding& binding);
    Result<std::optional<json>> load(const GeneratedSurfaceBinding& binding);
    Status save(const GeneratedSurfaceBinding& binding, const json& state);
    Result<GeneratedQuestionBinding> bind_question(const std::string& thread_id, const std::string& run_id,
                                                    const std::string& action_id, const std::string& revision,
                                                    const std::string& token);
    Result<std::optional<json>> load_question(const GeneratedQuestionBinding& binding);
    Status save_question(const GeneratedQuestionBinding& binding, const json& state);

private:
    Database& db_;
};

}  // namespace wb
