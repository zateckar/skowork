#pragma once

#include "base/result.h"
#include "model/types.h"

namespace wb::triage {

struct Event {
    std::string id, revision, text;
};

// improve-triage-results D1: a relevant result also says whether the user must act (action "required") or it is for
// information ("info"), and gives one category.
inline constexpr const char* kCategories[] = {"request", "approval", "question", "meeting", "deadline", "decision", "update"};

struct Classification {
    std::string id, revision, outcome, reason;
    std::string action, category;  // relevant results only
};

// The system instructions the classifier sends (add-evaluation-export design D3).
std::string classification_instructions();

Result<std::vector<Classification>> classify(model::IChatModel& model, const std::string& responsibilities,
                                              const std::vector<Event>& events,
                                              const std::shared_ptr<CancelToken>& cancel = {});
// The same with other system instructions, a feature name for usage attribution and the usage reported back (an
// evaluation replay, add-evaluation-export design D3, D4).
struct ClassifyOptions {
    std::string instructions;
    std::string feature = "event-triage";
    model::Usage* usage = nullptr;  // added to when the model answered
};
Result<std::vector<Classification>> classify(model::IChatModel& model, const std::string& responsibilities,
                                              const std::vector<Event>& events, const ClassifyOptions& options,
                                              const std::shared_ptr<CancelToken>& cancel = {});

}  // namespace wb::triage
